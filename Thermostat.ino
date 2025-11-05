#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>          // WiFi config portal
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"

// ===== DHT settings =====
#define DHTPIN 4
#define DHTTYPE DHT22
DHT d(DHTPIN, DHTTYPE);

// ===== LCD settings =====
LiquidCrystal_I2C lcd(0x27, 16, 2);  // change to 0x3F if needed

// ===== Relay settings =====
#define RELAY_PIN 5                  // GPIO driving relay
// Active LOW relay: LOW = ON (closed), HIGH = OFF (open)

// ===== Control modes =====
enum ControlMode {
  MODE_AUTO = 0,
  MODE_FORCE_ON = 1,
  MODE_FORCE_OFF = 2
};

ControlMode controlMode = MODE_AUTO;

// ===== Web server =====
WebServer server(80);

// State
float temperatureF = 0.0f;
float humidity     = 0.0f;
float setPointF    = 50.0f;          // default desired temp
bool  heaterOn     = false;

unsigned long lastReadMs      = 0;
const unsigned long readEvery = 2000; // ms

// IP display state
bool showIP       = false;
unsigned long ipShowEnd = 0;
String ipStr;

// Backlight state
bool backlightOn = true;

// ===== Helpers =====
String heaterStatusText() {
  return heaterOn ? "ON" : "OFF";
}

String modeText() {
  switch (controlMode) {
    case MODE_FORCE_ON:  return "Force ON";
    case MODE_FORCE_OFF: return "Force OFF";
    case MODE_AUTO:
    default:             return "Auto";
  }
}

String backlightText() {
  return backlightOn ? "On" : "Off";
}

// ===== Web handlers =====
void handleRoot() {
  String page =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>Smart Thermostat</title>"
    "<style>"
    "body{font-family:Arial;text-align:center;padding-top:40px;background:#f5f5f5;margin:0;}"
    "h1{margin-bottom:5px;color:#333;}"
    "p{font-size:1.1rem;color:#444;}"
    "hr{margin:20px auto;width:80%;}"
    "form{margin:10px 0;}"
    "input{font-size:1rem;padding:4px;margin-top:8px;}"
    "button{font-size:1rem;padding:4px 10px;margin:4px;border:none;border-radius:4px;"
    "background:#3498db;color:white;cursor:pointer;}"
    "button:hover{background:#2980b9;}"
    ".danger{background:#e74c3c;}"
    ".danger:hover{background:#c0392b;}"
    "</style>"
    "</head><body>"
    "<h1>Smart Thermostat</h1>"
    "<p><b>Current Temp:</b> " + String(temperatureF, 1) + " &deg;F</p>"
    "<p><b>Humidity:</b> " + String(humidity, 0) + " %</p>"
    "<p><b>Setpoint:</b> " + String(setPointF, 1) + " &deg;F</p>"
    "<p><b>Heater:</b> " + heaterStatusText() + "</p>"
    "<p><b>Mode:</b> " + modeText() + "</p>"
    "<p><b>Screen:</b> " + backlightText() + "</p>"
    "<hr>"
    "<form action=\"/set\" method=\"GET\">"
      "<label>New setpoint (&deg;F): </label>"
      "<input type=\"number\" name=\"sp\" step=\"1\" value=\"" + String(setPointF, 0) + "\">"
      "<button type=\"submit\">Update</button>"
    "</form>"
    "<hr>"
    "<p><b>Mode Control</b></p>"
    "<form action=\"/mode\" method=\"GET\">"
      "<button type=\"submit\" name=\"m\" value=\"auto\">Auto</button>"
      "<button type=\"submit\" name=\"m\" value=\"on\">Force ON</button>"
      "<button type=\"submit\" name=\"m\" value=\"off\">Force OFF</button>"
    "</form>"
    "<hr>"
    "<p><b>Display</b></p>"
    "<form action=\"/screen\" method=\"GET\">"
      "<button type=\"submit\" name=\"s\" value=\"on\">Screen ON</button>"
      "<button type=\"submit\" name=\"s\" value=\"off\">Screen OFF</button>"
    "</form>"
    "<hr>"
    "<p><b>System</b></p>"
    "<button class=\"danger\" type=\"button\" "
      "onclick=\"if(confirm('Factory reset WiFi settings? You will need to reconfigure the thermostat.'))"
      "{ window.location.href='/resetwifi'; }\">"
      "Factory Reset WiFi"
    "</button>"
    "<p style='margin-top:20px;font-size:0.9rem;color:#666;'>"
    "Auto: heater ON at (SP-3&deg;F), OFF at (SP+3&deg;F). Force ON/OFF ignore temperature."
    "</p>"
    "</body></html>";

  server.send(200, "text/html", page);
}

void handleSet() {
  if (server.hasArg("sp")) {
    float newSp = server.arg("sp").toFloat();
    if (newSp < 35.0f) newSp = 35.0f;
    if (newSp > 90.0f) newSp = 90.0f;
    setPointF = newSp;
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleMode() {
  if (server.hasArg("m")) {
    String m = server.arg("m");
    if (m == "auto") {
      controlMode = MODE_AUTO;
    } else if (m == "on") {
      controlMode = MODE_FORCE_ON;
    } else if (m == "off") {
      controlMode = MODE_FORCE_OFF;
    }
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleScreen() {
  if (server.hasArg("s")) {
    String s = server.arg("s");
    if (s == "on") {
      backlightOn = true;
      lcd.backlight();
    } else if (s == "off") {
      backlightOn = false;
      lcd.noBacklight();
    }
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleResetWiFi() {
  server.send(200, "text/html",
    "<html><body><h3>Resetting WiFi settings...</h3>"
    "<p>Device will reboot into setup mode.</p></body></html>");

  delay(1000);
  WiFiManager wm;
  wm.resetSettings();    // clear stored WiFi credentials
  delay(500);
  ESP.restart();
}

// ===== Control & I/O =====
void updateHeaterControl() {
  if (controlMode == MODE_FORCE_ON) {
    heaterOn = true;
  } else if (controlMode == MODE_FORCE_OFF) {
    heaterOn = false;
  } else { // MODE_AUTO
    if (!isnan(temperatureF)) {
      const float onThreshold  = setPointF - 3.0f;
      const float offThreshold = setPointF + 3.0f;

      if (!heaterOn && temperatureF <= onThreshold) {
        heaterOn = true;
      } else if (heaterOn && temperatureF >= offThreshold) {
        heaterOn = false;
      }
    }
  }
  // Active LOW relay: LOW = ON, HIGH = OFF
  digitalWrite(RELAY_PIN, heaterOn ? LOW : HIGH);
}

// ===== LCD layouts =====
void showIPScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("IP Address:");
  lcd.setCursor(0, 1);
  lcd.print(ipStr); // may truncate
}

void updateLCD() {
  lcd.clear();

  // Line 0: "Set Temp: {set}F"
  lcd.setCursor(0, 0);
  lcd.print("Set Temp: ");
  lcd.print((int)setPointF);
  lcd.print("F");

  // Line 1: "Cur Temp: {cur}F"
  lcd.setCursor(0, 1);
  lcd.print("Cur Temp: ");
  lcd.print((int)temperatureF);
  lcd.print("F");
}

void setup() {
  // LCD
  lcd.init();
  lcd.backlight();
  backlightOn = true;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Thermostat");

  // DHT
  d.begin();

  // Relay pin
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // OFF (active LOW)

  // WiFi via WiFiManager
  WiFi.mode(WIFI_STA);
  WiFiManager wm;

  // On-screen hint during setup portal
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Setup Mode");
  lcd.setCursor(0, 1);
  lcd.print("Join Therm-Setup");

  bool res = wm.autoConnect("Therm-Setup"); // AP name

  if (!res) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi failed");
    delay(3000);
    ESP.restart();
  }

  // Connected
  ipStr = WiFi.localIP().toString();
  ipShowEnd = millis() + 30000; // 30 seconds
  showIP = true;

  showIPScreen();

  // Web server routes
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/mode", handleMode);
  server.on("/screen", handleScreen);
  server.on("/resetwifi", handleResetWiFi);
  server.begin();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastReadMs >= readEvery) {
    lastReadMs = now;

    float tF = d.readTemperature(true); // Fahrenheit
    float h  = d.readHumidity();

    if (!isnan(tF) && !isnan(h)) {
      temperatureF = tF;
      humidity = h;
    }

    updateHeaterControl();

    // Handle LCD mode
    if (showIP && now > ipShowEnd) {
      showIP = false;
    }

    if (!showIP) {
      updateLCD();
    }
    // backlight state is handled only by /screen
  }
}

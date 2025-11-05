#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"

// ===== WiFi settings =====
const char* ssid     = "YourMom";
const char* password = "12345678";

// ===== DHT settings =====
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

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
bool showIP       = true;
unsigned long ipShowEnd = 0;
String ipStr;

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

// ===== Web handlers =====
void handleRoot() {
  String page =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>Smart Thermostat</title>"
    "<style>"
    "body{font-family:Arial;text-align:center;padding-top:40px;}"
    "h1{margin-bottom:5px;}"
    "p{font-size:1.1rem;}"
    "input{font-size:1rem;padding:4px;margin-top:8px;}"
    "button{font-size:1rem;padding:4px 10px;margin:4px;}"
    "</style>"
    "</head><body>"
    "<h1>Smart Thermostat</h1>"
    "<p><b>Current Temp:</b> " + String(temperatureF, 1) + " &deg;F</p>"
    "<p><b>Humidity:</b> " + String(humidity, 0) + " %</p>"
    "<p><b>Setpoint:</b> " + String(setPointF, 1) + " &deg;F</p>"
    "<p><b>Heater:</b> " + heaterStatusText() + "</p>"
    "<p><b>Mode:</b> " + modeText() + "</p>"
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
    "<p style='margin-top:20px;font-size:0.9rem;color:#555;'>"
    "Auto: heater ON at (SP-3&deg;F), OFF at (SP+3&deg;F). "
    "Force ON/OFF ignore temperature."
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
  lcd.print(ipStr); // may truncate, that's fine
}

void updateLCD() {
  lcd.clear();

  // Line 0: "Set Temp: {set_temp}"
  lcd.setCursor(0, 0);
  lcd.print("Set Temp:");
  lcd.setCursor(10, 0);
  lcd.print(setPointF, 0); // no units to save space

  // Line 1: "Current Temp: {temp}"
  lcd.setCursor(0, 1);
  lcd.print("Current Temp:");
  // 13 chars used, put value at col 13
  lcd.setCursor(13, 1);
  lcd.print(temperatureF, 0);
}

void setup() {
  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Thermostat");

  // DHT
  dht.begin();

  // Relay pin
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // OFF (active LOW)

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  lcd.setCursor(0, 1);
  lcd.print("WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  ipStr = WiFi.localIP().toString();
  ipShowEnd = millis() + 30000; // 30 seconds
  showIP = true;

  showIPScreen();

  // Web server
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/mode", handleMode);
  server.begin();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastReadMs >= readEvery) {
    lastReadMs = now;

    float tF = dht.readTemperature(true); // Fahrenheit
    float h  = dht.readHumidity();

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
    // If showIP == true, we keep the IP screen as-is
  }
}

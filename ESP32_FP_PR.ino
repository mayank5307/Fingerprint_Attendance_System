#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <Wire.h>
#include <time.h>

// Fallback color definitions for optical sensor LED ring control
#ifndef FINGERPRINT_LED_RED
#define FINGERPRINT_LED_RED 0x01
#endif
#ifndef FINGERPRINT_LED_BLUE
#define FINGERPRINT_LED_BLUE 0x02
#endif
#ifndef FINGERPRINT_LED_PURPLE
#define FINGERPRINT_LED_PURPLE 0x03
#endif
#ifndef FINGERPRINT_LED_GREEN
#define FINGERPRINT_LED_GREEN 0x04
#endif
#ifndef FINGERPRINT_LED_YELLOW
#define FINGERPRINT_LED_YELLOW 0x05
#endif
#ifndef FINGERPRINT_LED_CYAN
#define FINGERPRINT_LED_CYAN 0x06
#endif
#ifndef FINGERPRINT_LED_WHITE
#define FINGERPRINT_LED_WHITE 0x07
#endif

// Hardware I2C OLED (Default ESP32 I2C Pins: SDA = GPIO 21, SCL = GPIO 22)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

WiFiMulti wifiMulti;

// Hardware Serial 2 pins updated to GPIO 4 & 5 to avoid GPIO 16/17 PSRAM conflict
#define RXD2 4
#define TXD2 5

HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// NTP time server configuration (IST: GMT+5:30 -> 19800 seconds)
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

WebServer server(80);

enum AppState {
  STATE_SCAN,
  STATE_ENROLL_WAIT_FINGER,
  STATE_ENROLL_WAIT_REMOVE,
  STATE_ENROLL_WAIT_FINGER2,
  STATE_ENROLL_WAIT_REMOVE2,
  STATE_ENROLL_SUCCESS,
  STATE_TEST_SCAN
};
AppState currentState = STATE_SCAN;

uint8_t enrollId = 0;
String enrollName = "";
unsigned long enrollStartTime = 0;
#define ENROLL_TIMEOUT 60000

String statusMessage = "System Ready";
String currentDisplayMessage = "Scan Finger";
String currentDisplaySub = "Ready";
unsigned long lastOLEDUpdate = 0;

// OLED Timed Alerts & Test Timers
bool alertOLEDActive = false;
unsigned long alertOLEDTime = 0;
bool testLEDActive = false;
unsigned long testLEDTime = 0;
bool testOLEDActive = false;
unsigned long testOLEDTime = 0;

// OLED SSID Horizontal Scroll variables
int oledSsidScrollX = 0;
int oledSsidDirection = 1;
unsigned long lastOledSsidScroll = 0;

// Disconnected Wi-Fi Saved Network Loop timer variables
String savedSsidList[10];
int savedSsidCount = 0;
int currentSavedSsidIndex = 0;
unsigned long lastSavedSsidToggleTime = 0;

// Reboot flag for web-initiated network changes
bool shouldReboot = false;
unsigned long rebootTime = 0;

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FP Attendance Dashboard</title>
</head>
<body>
  <h2>FP Attendance Dashboard</h2>
  <p>Please upload index.html to SPIFFS flash or view through the web interface.</p>
</body>
</html>
)rawliteral";

String getDateString();
String getTimeString();
String getOledTimeString();
void updateOLED();
void triggerOLEDAlert(String line1, String line2);
bool initializeFingerprintSensor();
void updateSavedSsidList();
void setupTime();
void loadAndInitWiFi();
void saveProfile(int id, String name);
String getProfileName(int id);
void addLog(int id, String name);

void triggerOLEDAlert(String line1, String line2) {
  currentDisplayMessage = line1;
  currentDisplaySub = line2;
  alertOLEDActive = true;
  alertOLEDTime = millis();
  updateOLED();
}

void updateSavedSsidList() {
  savedSsidCount = 0;
  File f = SPIFFS.open("/wifi_config.json", "r");
  if (f) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, f)) {
      JsonArray arr;
      if (doc.is<JsonObject>() && doc.containsKey("networks")) {
        arr = doc["networks"].as<JsonArray>();
      } else if (doc.is<JsonArray>()) {
        arr = doc.as<JsonArray>();
      }
      for (JsonObject net : arr) {
        if (savedSsidCount < 10) {
          savedSsidList[savedSsidCount++] = net["ssid"].as<String>();
        }
      }
    }
    f.close();
  }
}

void updateOLED() {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_5x8_tr);
  String timeStr = getOledTimeString();
  int timeW = u8g2.getStrWidth(timeStr.c_str());
  
  u8g2.setCursor(128 - timeW, 10);
  u8g2.print(timeStr);

  u8g2.setFont(u8g2_font_6x10_tr);
  String ssidStr = "";
  if (WiFi.status() == WL_CONNECTED) {
    ssidStr = WiFi.SSID();
  } else {
    if (savedSsidCount == 0) updateSavedSsidList();
    if (savedSsidCount > 0) {
      if (millis() - lastSavedSsidToggleTime > 5000) {
        lastSavedSsidToggleTime = millis();
        currentSavedSsidIndex = (currentSavedSsidIndex + 1) % savedSsidCount;
      }
      ssidStr = "Saved: " + savedSsidList[currentSavedSsidIndex];
    } else {
      ssidStr = "WiFi Disconnected";
    }
  }

  int ssidW = u8g2.getStrWidth(ssidStr.c_str());
  int availableSsidWidth = 128 - timeW - 4;

  if (ssidW <= availableSsidWidth) {
    u8g2.setCursor(0, 10);
    u8g2.print(ssidStr);
    oledSsidScrollX = 0;
  } else {
    if (millis() - lastOledSsidScroll > 500) {
      lastOledSsidScroll = millis();
      int maxScroll = ssidW - availableSsidWidth;
      
      if (oledSsidDirection > 0) {
        oledSsidScrollX += 2;
        if (oledSsidScrollX >= maxScroll) {
          oledSsidScrollX = maxScroll;
          oledSsidDirection = -1;
        }
      } else {
        oledSsidScrollX -= 6;
        if (oledSsidScrollX <= 0) {
          oledSsidScrollX = 0;
          oledSsidDirection = 1;
        }
      }
    }
    
    u8g2.setClipWindow(0, 0, availableSsidWidth, 12);
    u8g2.setCursor(-oledSsidScrollX, 10);
    u8g2.print(ssidStr);
    u8g2.setMaxClipWindow();
  }

  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor(0, 22);
  if (WiFi.status() == WL_CONNECTED) {
    u8g2.print(WiFi.localIP().toString());
  } else {
    u8g2.print("Idle (No WiFi)");
  }

  u8g2.setCursor(0, 36);
  if (WiFi.status() != WL_CONNECTED && !alertOLEDActive && currentState == STATE_SCAN) {
    u8g2.print("WiFi Disconnected");
  } else {
    u8g2.print(currentDisplayMessage.substring(0, 21));
  }

  u8g2.setCursor(0, 50);
  if (WiFi.status() != WL_CONNECTED && !alertOLEDActive && currentState == STATE_SCAN) {
    if (savedSsidCount > 0) {
      String subMsg = "Try: " + savedSsidList[currentSavedSsidIndex];
      u8g2.print(subMsg.substring(0, 21));
    } else {
      u8g2.print("System Idle");
    }
  } else {
    u8g2.print(currentDisplaySub.substring(0, 21));
  }

  u8g2.sendBuffer();
}

bool initializeFingerprintSensor() {
  uint32_t bauds[] = {57600, 9600, 115200};
  for (uint32_t baud : bauds) {
    Serial.printf("Testing fingerprint sensor at %u baud on RX: %d, TX: %d...\n", baud, RXD2, TXD2);
    mySerial.begin(baud, SERIAL_8N1, RXD2, TXD2);
    delay(100);
    while (mySerial.available()) mySerial.read(); // Clear hardware buffer
    finger.begin(baud);
    if (finger.verifyPassword()) {
      Serial.printf("Fingerprint sensor detected successfully at %u baud!\n", baud);
      return true;
    }
  }
  return false;
}

void setupTime() { configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); }

String getDateString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo) || timeinfo.tm_year < 100) return "00/00/0000";
  char buff[20];
  strftime(buff, sizeof(buff), "%d/%m/%Y", &timeinfo);
  return String(buff);
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo) || timeinfo.tm_year < 100) return "--:--:-- --";
  char buff[24];
  strftime(buff, sizeof(buff), "%I:%M:%S %p", &timeinfo);
  return String(buff);
}

String getOledTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo) || timeinfo.tm_year < 100) return "--:--:--";
  char buff[16];
  strftime(buff, sizeof(buff), "%I:%M:%S", &timeinfo);
  return String(buff);
}

void loadAndInitWiFi() {
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  // Create default configuration if /wifi_config.json does not exist
  if (!SPIFFS.exists("/wifi_config.json")) {
    File f = SPIFFS.open("/wifi_config.json", "w");
    if (f) {
      DynamicJsonDocument doc(2048);
      JsonObject root = doc.to<JsonObject>();
      root["selected"] = "MAYANK SONAGARA VIVO";
      JsonArray arr = root.createNestedArray("networks");

      JsonObject w1 = arr.createNestedObject();
      w1["ssid"] = "CANDOR";
      w1["password"] = "a1b2c3d4e5";

      JsonObject w2 = arr.createNestedObject();
      w2["ssid"] = "MAYANK SONAGARA VIVO";
      w2["password"] = "10101010";

      JsonObject w3 = arr.createNestedObject();
      w3["ssid"] = "kishan";
      w3["password"] = "Kishan@5307";

      serializeJson(doc, f);
      f.close();
      Serial.println("Created default /wifi_config.json");
    }
  }

  // Structure to hold saved networks from JSON
  struct SavedNet {
    String ssid;
    String password;
  };
  SavedNet savedNets[10];
  int savedCount = 0;
  String selectedSsid = "";

  File f = SPIFFS.open("/wifi_config.json", "r");
  if (f) {
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (!err) {
      JsonArray arr;
      if (doc.is<JsonObject>()) {
        if (doc.containsKey("selected")) selectedSsid = doc["selected"].as<String>();
        arr = doc["networks"].as<JsonArray>();
      } else if (doc.is<JsonArray>()) {
        arr = doc.as<JsonArray>();
      }

      for (JsonObject net : arr) {
        const char *s = net["ssid"];
        const char *p = net["password"];
        if (s && strlen(s) > 0 && savedCount < 10) {
          savedNets[savedCount].ssid = String(s);
          savedNets[savedCount].password = p ? String(p) : "";
          wifiMulti.addAP(s, p ? p : "");
          savedCount++;
        }
      }
    }
  }

  if (savedCount == 0) {
    Serial.println("No saved Wi-Fi networks found in SPIFFS.");
    return;
  }

  // Scan surrounding networks to locate the strongest available saved network
  Serial.println("Scanning nearby Wi-Fi networks to select the strongest saved AP...");
  int scanCount = WiFi.scanNetworks();
  String bestSsid = "";
  String bestPass = "";
  int bestRssi = -999;

  for (int i = 0; i < scanCount; ++i) {
    String scannedSsid = WiFi.SSID(i);
    int scannedRssi = WiFi.RSSI(i);

    for (int j = 0; j < savedCount; ++j) {
      if (scannedSsid == savedNets[j].ssid) {
        // If this matched network has a stronger signal, pick it
        if (scannedRssi > bestRssi) {
          bestRssi = scannedRssi;
          bestSsid = savedNets[j].ssid;
          bestPass = savedNets[j].password;
        }
      }
    }
  }

  // Fallback to the web UI selected SSID if scan didn't pick up any saved networks
  if (bestSsid.length() == 0 && selectedSsid.length() > 0) {
    for (int j = 0; j < savedCount; ++j) {
      if (savedNets[j].ssid == selectedSsid) {
        bestSsid = savedNets[j].ssid;
        bestPass = savedNets[j].password;
        break;
      }
    }
  }

  // Ultimate fallback to first saved network
  if (bestSsid.length() == 0 && savedCount > 0) {
    bestSsid = savedNets[0].ssid;
    bestPass = savedNets[0].password;
  }

  if (bestSsid.length() > 0) {
    Serial.printf("Connecting to strongest available Wi-Fi: %s (RSSI: %d dBm)...\n", bestSsid.c_str(), bestRssi);
    WiFi.begin(bestSsid.c_str(), bestPass.c_str());
    unsigned long startTry = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTry < 6000) {
      delay(100);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected successfully! Syncing NTP time...");
    setupTime();
  } else {
    Serial.println("\nDirect connection timed out, attempting background WiFiMulti...");
    wifiMulti.run();
  }
}

void saveProfile(int id, String name) {
  DynamicJsonDocument doc(8192);
  JsonObject root;

  if (SPIFFS.exists("/profiles.json")) {
    File file = SPIFFS.open("/profiles.json", "r");
    if (file) {
      DeserializationError err = deserializeJson(doc, file);
      file.close();
      if (!err && doc.is<JsonObject>()) {
        root = doc.as<JsonObject>();
      } else {
        root = doc.to<JsonObject>();
      }
    } else {
      root = doc.to<JsonObject>();
    }
  } else {
    root = doc.to<JsonObject>();
  }

  String idStr = String(id);
  JsonObject profile;
  if (root.containsKey(idStr) && root[idStr].is<JsonObject>()) {
    profile = root[idStr].as<JsonObject>();
  } else {
    profile = root.createNestedObject(idStr);
  }

  profile["name"] = name;
  profile["enrollTime"] = getDateString() + " " + getTimeString();

  File file = SPIFFS.open("/profiles.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.printf("Profile saved for ID %d (%s)\n", id, name.c_str());
  } else {
    Serial.println("Error opening /profiles.json for writing!");
  }
}

String getProfileName(int id) {
  File file = SPIFFS.open("/profiles.json", "r");
  DynamicJsonDocument doc(8192);
  if (file) {
    deserializeJson(doc, file);
    file.close();
    String key = String(id);
    if (doc.containsKey(key)) {
      if (doc[key].is<JsonObject>()) {
        return doc[key]["name"].as<String>();
      } else {
        return doc[key].as<String>();
      }
    }
  }
  return "User " + String(id);
}

void addLog(int id, String name) {
  DynamicJsonDocument doc(8192);
  JsonArray arr;

  if (SPIFFS.exists("/logs.json")) {
    File file = SPIFFS.open("/logs.json", "r");
    if (file) {
      DeserializationError error = deserializeJson(doc, file);
      file.close();
      if (!error && doc.is<JsonArray>()) {
        arr = doc.as<JsonArray>();
      } else {
        arr = doc.to<JsonArray>();
      }
    } else {
      arr = doc.to<JsonArray>();
    }
  } else {
    arr = doc.to<JsonArray>();
  }

  JsonObject newLog = arr.createNestedObject();
  newLog["id"] = id;
  newLog["name"] = name;
  newLog["date"] = getDateString();
  newLog["time"] = getTimeString();

  while (arr.size() > 150) {
    arr.remove(0);
  }

  File file = SPIFFS.open("/logs.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.printf("Log added for ID %d (%s). Total logs in history: %d\n", id, name.c_str(), arr.size());
  } else {
    Serial.println("Error writing to /logs.json");
  }
}

void handleRoot() {
  if (SPIFFS.exists("/index.html")) {
    File file = SPIFFS.open("/index.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  } else {
    server.send(200, "text/html", INDEX_HTML);
  }
}

void handleInfo() {
  DynamicJsonDocument doc(256);
  doc["time"] = getTimeString();
  doc["date"] = getDateString();
  doc["ssid"] = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "Disconnected";
  doc["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "0.0.0.0";
  doc["domain"] = "esp32-fp.local";
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleStatus() {
  DynamicJsonDocument doc(256);
  doc["message"] = statusMessage;
  doc["state"] = String((int)currentState);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleLogs() {
  File file = SPIFFS.open("/logs.json", "r");
  if (!file) {
    server.send(200, "application/json", "[]");
    return;
  }
  server.streamFile(file, "application/json");
  file.close();
}

void handleProfiles() {
  File file = SPIFFS.open("/profiles.json", "r");
  if (!file) {
    server.send(200, "application/json", "{}");
    return;
  }
  server.streamFile(file, "application/json");
  file.close();
}

void handleNextId() {
  File file = SPIFFS.open("/profiles.json", "r");
  DynamicJsonDocument doc(8192);
  if (file) {
    deserializeJson(doc, file);
    file.close();
  }
  int nextId = 1;
  while (nextId <= 127) {
    if (!doc.containsKey(String(nextId))) break;
    nextId++;
  }
  DynamicJsonDocument res(64);
  res["id"] = nextId;
  String json;
  serializeJson(res, json);
  server.send(200, "application/json", json);
}

void handleEnroll() {
  if (!server.hasArg("id") || !server.hasArg("name")) {
    server.send(400, "text/plain", "Missing args");
    return;
  }
  enrollId = server.arg("id").toInt();
  enrollName = server.arg("name");
  currentState = STATE_ENROLL_WAIT_FINGER;
  enrollStartTime = millis();
  statusMessage = "Place Finger";
  triggerOLEDAlert("Enrolling...", enrollName);
  finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 100, FINGERPRINT_LED_PURPLE, 0);
  server.send(200, "text/plain", "Enrollment started");
}

void handleRemove() {
  if (!server.hasArg("id")) { server.send(400, "text/plain", "Missing ID"); return; }
  int id = server.arg("id").toInt();

  uint8_t sensorRes = finger.deleteModel(id);
  Serial.printf("Deleting model ID %d from sensor. Status: 0x%X\n", id, sensorRes);

  DynamicJsonDocument doc(8192);
  if (SPIFFS.exists("/profiles.json")) {
    File file = SPIFFS.open("/profiles.json", "r");
    if (file) {
      deserializeJson(doc, file);
      file.close();
    }
  }
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();

  doc.remove(String(id));

  File file = SPIFFS.open("/profiles.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }

  triggerOLEDAlert("User Deleted", "ID: " + String(id));
  server.send(200, "text/plain", "Removed");
}

void handleRemoveAll() {
  File file = SPIFFS.open("/profiles.json", "w"); file.print("{}"); file.close();
  file = SPIFFS.open("/logs.json", "w"); file.print("[]"); file.close();

  uint8_t sensorRes = finger.emptyDatabase();
  Serial.printf("Emptying hardware sensor database. Status: 0x%X\n", sensorRes);

  triggerOLEDAlert("All Data Deleted", "Sensor Purged");
  server.send(200, "text/plain", "All data deleted");

  if (sensorRes != FINGERPRINT_OK) {
    Serial.println("emptyDatabase() did not return OK, running per-ID fallback purge...");
    for (uint8_t i = 1; i < 128; i++) {
      finger.deleteModel(i);
      delay(2);
    }
    Serial.println("Fallback per-ID sensor purge complete.");
  }
}

void handleWifiSaved() {
  File f = SPIFFS.open("/wifi_config.json", "r");
  if (!f) {
    server.send(200, "application/json", "[]");
    return;
  }
  DynamicJsonDocument doc(2048);
  deserializeJson(doc, f);
  f.close();
  JsonArray arr;
  if (doc.is<JsonObject>() && doc.containsKey("networks")) {
    arr = doc["networks"].as<JsonArray>();
  } else if (doc.is<JsonArray>()) {
    arr = doc.as<JsonArray>();
  } else {
    server.send(200, "application/json", "[]");
    return;
  }
  String json;
  serializeJson(arr, json);
  server.send(200, "application/json", json);
}

void handleWifiScan() {
  int n = WiFi.scanNetworks();
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; ++i) {
    JsonObject net = arr.createNestedObject();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  String json;
  serializeJson(arr, json);
  server.send(200, "application/json", json);
}

void handleWifiConnect() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Missing SSID");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("password") ? server.arg("password") : "";

  File f = SPIFFS.open("/wifi_config.json", "r");
  DynamicJsonDocument doc(2048);
  if (f) {
    deserializeJson(doc, f);
    f.close();
  }

  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) root = doc.to<JsonObject>();
  root["selected"] = ssid;

  JsonArray arr;
  if (root.containsKey("networks")) {
    arr = root["networks"].as<JsonArray>();
  } else {
    arr = root.createNestedArray("networks");
  }

  bool found = false;
  for (JsonObject net : arr) {
    if (net["ssid"].as<String>() == ssid) {
      net["password"] = pass;
      found = true;
      break;
    }
  }
  if (!found) {
    JsonObject newNet = arr.createNestedObject();
    newNet["ssid"] = ssid;
    newNet["password"] = pass;
  }

  f = SPIFFS.open("/wifi_config.json", "w");
  serializeJson(doc, f);
  f.close();

  updateSavedSsidList();

  shouldReboot = true;
  rebootTime = millis();

  server.send(200, "text/plain", "Wi-Fi updated. Rebooting...");
}

void handleWifiDelete() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Missing SSID");
    return;
  }
  String ssid = server.arg("ssid");

  File f = SPIFFS.open("/wifi_config.json", "r");
  DynamicJsonDocument doc(2048);
  if (f) {
    deserializeJson(doc, f);
    f.close();
  }

  if (doc.is<JsonObject>() && doc.containsKey("networks")) {
    JsonArray arr = doc["networks"].as<JsonArray>();
    for (int i = arr.size() - 1; i >= 0; i--) {
      if (arr[i]["ssid"].as<String>() == ssid) {
        arr.remove(i);
      }
    }
    if (doc["selected"].as<String>() == ssid) {
      doc["selected"] = "";
    }
    f = SPIFFS.open("/wifi_config.json", "w");
    serializeJson(doc, f);
    f.close();
  }

  updateSavedSsidList();
  server.send(200, "text/plain", "Network deleted");
}

void handleTestLed() {
  if (!server.hasArg("color")) {
    server.send(400, "text/plain", "Missing color");
    return;
  }
  String color = server.arg("color");
  testLEDActive = true;
  testLEDTime = millis();

  if (color == "red") finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_RED, 0);
  else if (color == "blue") finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_BLUE, 0);
  else if (color == "green") finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_GREEN, 0);
  else if (color == "yellow") finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_YELLOW, 0);
  else if (color == "purple") finger.LEDcontrol(FINGERPRINT_LED_PURPLE, 0, FINGERPRINT_LED_PURPLE, 0);
  else if (color == "cyan") finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_CYAN, 0);
  else if (color == "white") finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_WHITE, 0);
  else if (color == "off") { finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0); testLEDActive = false; }

  server.send(200, "text/plain", "LED command sent");
}

void handleTestScan() {
  currentState = STATE_TEST_SCAN;
  statusMessage = "Testing Scanner...";
  triggerOLEDAlert("Scanner Test", "Place Finger");
  finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 128, FINGERPRINT_LED_PURPLE, 0);
  server.send(200, "text/plain", "Test scan started");
}

void handleTestOled() {
  if (!server.hasArg("mode")) {
    server.send(400, "text/plain", "Missing mode");
    return;
  }
  String mode = server.arg("mode");
  testOLEDActive = true;
  testOLEDTime = millis();

  u8g2.clearBuffer();
  if (mode == "fill") {
    u8g2.drawBox(0, 0, 128, 64);
  }
  u8g2.sendBuffer();

  server.send(200, "text/plain", "OLED test triggered");
}

void processFingerprint() {
  if (testLEDActive && millis() - testLEDTime > 5000) {
    testLEDActive = false;
    finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
  }

  if (testOLEDActive && millis() - testOLEDTime > 5000) {
    testOLEDActive = false;
    updateOLED();
  }

  if (alertOLEDActive && millis() - alertOLEDTime > 3000) {
    alertOLEDActive = false;
    if (currentState == STATE_SCAN) {
      currentDisplayMessage = "Scan Finger";
      currentDisplaySub = "Ready";
      statusMessage = "System Ready";
    }
  }

  if (currentState != STATE_SCAN && currentState != STATE_TEST_SCAN && currentState != STATE_ENROLL_SUCCESS) {
    if (millis() - enrollStartTime > ENROLL_TIMEOUT) {
      currentState = STATE_SCAN;
      statusMessage = "System Ready";
      triggerOLEDAlert("Enroll Timeout", "Cancelled");
      finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
      return;
    }
  }
  delay(50);
  uint8_t p = finger.getImage();

  if (currentState == STATE_TEST_SCAN) {
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz();
      if (p == FINGERPRINT_OK) {
        triggerOLEDAlert("Sensor OK", "Image Captured");
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_GREEN, 1);
      } else {
        triggerOLEDAlert("Sensor Error", "Image Failed");
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_RED, 1);
      }
      currentState = STATE_SCAN;
      delay(1000);
      finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
    }
    return;
  }

  if (currentState == STATE_SCAN) {
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz();
      if (p != FINGERPRINT_OK) return;

      p = finger.fingerSearch();
      if (p == FINGERPRINT_OK) {
        uint8_t id = finger.fingerID;
        String name = getProfileName(id);
        addLog(id, name);
        statusMessage = "Scanned: " + name;
        triggerOLEDAlert("ID: " + String(id), name);
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_GREEN, 1);
        delay(1500);
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
      } else if (p == FINGERPRINT_NOTFOUND) {
        statusMessage = "Unknown User";
        triggerOLEDAlert("Access Denied", "Unknown Finger");
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_RED, 1);
        delay(1500);
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
      }
    }
    return;
  }

  if (currentState == STATE_ENROLL_WAIT_FINGER) {
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz(1);
      if (p == FINGERPRINT_OK) {
        currentState = STATE_ENROLL_WAIT_REMOVE;
        statusMessage = "Remove Finger";
        triggerOLEDAlert("Finger Captured", "Lift Finger");
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 100, FINGERPRINT_LED_CYAN, 0);
      }
    }
    return;
  }

  if (currentState == STATE_ENROLL_WAIT_REMOVE) {
    if (p == FINGERPRINT_NOFINGER) {
      currentState = STATE_ENROLL_WAIT_FINGER2;
      statusMessage = "Place Same Finger";
      triggerOLEDAlert("Lifted", "Place Finger Again");
      finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_PURPLE, 0);
    }
    return;
  }

  if (currentState == STATE_ENROLL_WAIT_FINGER2) {
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz(2);
      if (p == FINGERPRINT_OK) {
        p = finger.createModel();
        if (p == FINGERPRINT_OK) {
          p = finger.storeModel(enrollId);
          if (p == FINGERPRINT_OK) {
            saveProfile(enrollId, enrollName);
            currentState = STATE_ENROLL_WAIT_REMOVE2;
            statusMessage = "Remove Finger";
            triggerOLEDAlert("CAPTURED", "Lift Finger");
            finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 100, FINGERPRINT_LED_CYAN, 0);
          } else {
            currentState = STATE_SCAN;
            statusMessage = "Store Error";
            triggerOLEDAlert("Store Error", "Failed to Save");
            finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_RED, 1);
            delay(1000);
            finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
          }
        } else {
          currentState = STATE_SCAN;
          statusMessage = "Enroll Failed";
          triggerOLEDAlert("Match Error", "Try Again");
          finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_RED, 1);
          delay(1000);
          finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
        }
      }
    }
    return;
  }

  if (currentState == STATE_ENROLL_WAIT_REMOVE2) {
    if (p == FINGERPRINT_NOFINGER) {
      currentState = STATE_ENROLL_SUCCESS;
      statusMessage = "Enrolled!";
      triggerOLEDAlert("Enrolled!", enrollName);
      finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_GREEN, 1);
      enrollStartTime = millis();
    }
    return;
  }

  if (currentState == STATE_ENROLL_SUCCESS) {
    if (millis() - enrollStartTime > 2500) {
      currentState = STATE_SCAN;
      statusMessage = "System Ready";
      finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
    }
    return;
  }
}

void setup() {
  Serial.begin(115200);

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(10, 25, "Booting System...");
  u8g2.sendBuffer();

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed!");
  }

  if (!SPIFFS.exists("/profiles.json")) {
    File file = SPIFFS.open("/profiles.json", "w"); file.print("{}"); file.close();
  }
  if (!SPIFFS.exists("/logs.json")) {
    File file = SPIFFS.open("/logs.json", "w"); file.print("[]"); file.close();
  }

  loadAndInitWiFi();
  updateSavedSsidList();

  if (MDNS.begin("esp32-fp")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS responder started: http://esp32-fp.local");
  }

  setupTime();

  if (initializeFingerprintSensor()) {
    currentDisplayMessage = "Scan Finger";
    currentDisplaySub = "Ready";
    finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 255, FINGERPRINT_LED_BLUE, 0);
  } else {
    Serial.println("Fingerprint sensor not found on pins 4/5!");
    currentDisplayMessage = "No Sensor";
    currentDisplaySub = "Check RX/TX Pins";
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/logs", HTTP_GET, handleLogs);
  server.on("/api/profiles", HTTP_GET, handleProfiles);
  server.on("/api/next_id", HTTP_GET, handleNextId);
  server.on("/api/enroll", HTTP_POST, handleEnroll);
  server.on("/api/remove", HTTP_POST, handleRemove);
  server.on("/api/remove_all", HTTP_POST, handleRemoveAll);
  server.on("/api/wifi/saved", HTTP_GET, handleWifiSaved);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/delete", HTTP_POST, handleWifiDelete);
  server.on("/api/test/led", HTTP_POST, handleTestLed);
  server.on("/api/test/scan", HTTP_POST, handleTestScan);
  server.on("/api/test/oled", HTTP_POST, handleTestOled);

  server.enableCORS(true);
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      server.send(200);
      return;
    }
    if (SPIFFS.exists(server.uri())) {
      File file = SPIFFS.open(server.uri(), "r");
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
    handleRoot();
  });

  server.begin();
  updateOLED();
}

void loop() {
  server.handleClient();

  if (shouldReboot && millis() - rebootTime > 1500) {
    ESP.restart();
  }

  static bool wasConnected = false;
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  
  if (isConnected && !wasConnected) {
    wasConnected = true;
    setupTime();
    updateOLED();
  } else if (!isConnected && wasConnected) {
    wasConnected = false;
  }

  processFingerprint();

  if (currentState == STATE_SCAN && millis() - lastOLEDUpdate > 200 && !testOLEDActive) {
    updateOLED();
    lastOLEDUpdate = millis();
  }
}
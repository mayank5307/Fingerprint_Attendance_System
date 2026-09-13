# Fingerprint_Attendance_System

An end-to-end, IoT-enabled biometric attendance system powered by an ESP32 micro-controller. The device features optical fingerprint verification, an integrated SH1106 OLED display, an RGB LED optical indicator ring, active cooling fans[cite: 1], and a custom 3D-printed desktop enclosure[cite: 1].

The system exposes a lightweight REST API and serves an embedded responsive web dashboard directly from SPIFFS flash memory.

---

## Key Features

* **Biometric Authentication:** Real-time fingerprint matching using the Adafruit Fingerprint library[cite: 1].
* **Embedded Web Dashboard:** Responsive SPA built with native JS/HTML/CSS and hosted via ESP32 WebServer/SPIFFS.
* **Monthly Attendance Reporting:** Generates and exports monthly log reports into downloadable Excel (`.xlsx`) files.
* **Wi-Fi Management & Auto-Failover:** Dynamic connection manager that scans local networks, saves multiple Wi-Fi credentials to SPIFFS, and automatically connects to the strongest available access point[cite: 1].
* **OLED & LED Status Ring:** Displays real-time status messages, system IP address, clock time via NTP, and animated ring lighting feedback during operations[cite: 1].
* **Remote Device Testing:** Web UI controls for testing fingerprint optics, OLED pixel diagnostic fills, and LED ring colors.
* **mDNS Support:** Easy access via local domain `http://esp32-fp.local`[cite: 1, 2].

---

## Hardware Components

* ESP32 Development Board[cite: 1]
* Optical/Capacitive Fingerprint Sensor with Ring LED[cite: 1]
* SH1106 I2C OLED Display (128x64)[cite: 1]
* 5V DC Cooling Fan[cite: 1]
* Custom 3D-Printed Desktop Enclosure[cite: 1]

---

## Pin Mapping

| Component | ESP32 Pin | Function |
| :--- | :--- | :--- |
| **OLED SDA** | GPIO 21 | I2C Data[cite: 1] |
| **OLED SCL** | GPIO 22 | I2C Clock[cite: 1] |
| **Fingerprint RX** | GPIO 4 | Serial2 RX[cite: 1] |
| **Fingerprint TX** | GPIO 5 | Serial2 TX[cite: 1] |

---

## Software Stack & Dependencies

* **Platform:** Arduino / ESP-IDF (ESP32 Board Package)[cite: 1]
* **Key Libraries:** `WiFi`, `WebServer`, `SPIFFS`, `ESPmDNS`, `ArduinoJson`, `Adafruit_Fingerprint`, `U8g2lib`[cite: 1]
* **Frontend:** HTML5, CSS3, JavaScript (ES6+), SheetJS (XLSX export)[cite: 2]

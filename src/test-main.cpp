#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <LittleFS.h>
#include <secret.h>

// Forward declarations
void printMenu();
void setupOTA();
void handleOTA();

// Pin definitions for ESP32-S3-DevKitC-1
#define LED_PIN 2  // Built-in LED

// Pump control
#define PUMP_PIN 4

// Valve pins
#define VALVE1_PIN 5
#define VALVE2_PIN 6
#define VALVE3_PIN 7
#define VALVE4_PIN 15
#define VALVE5_PIN 16
#define VALVE6_PIN 17

// Rain sensor pins (one per valve)
#define RAIN_SENSOR1_PIN 8
#define RAIN_SENSOR2_PIN 9
#define RAIN_SENSOR3_PIN 10
#define RAIN_SENSOR4_PIN 11
#define RAIN_SENSOR5_PIN 12
#define RAIN_SENSOR6_PIN 13

// Water level sensor pin
#define WATER_LEVEL_SENSOR_PIN 19

// DS3231 RTC I2C pins
#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 3
#define DS3231_I2C_ADDRESS 0x68

const int NUM_VALVES = 6;
const int VALVE_PINS[NUM_VALVES] = {VALVE1_PIN, VALVE2_PIN, VALVE3_PIN, VALVE4_PIN, VALVE5_PIN, VALVE6_PIN};
const int RAIN_SENSOR_PINS[NUM_VALVES] = {RAIN_SENSOR1_PIN, RAIN_SENSOR2_PIN, RAIN_SENSOR3_PIN, RAIN_SENSOR4_PIN, RAIN_SENSOR5_PIN, RAIN_SENSOR6_PIN};

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n\n\n");
  Serial.println("╔════════════════════════════════════════════╗");
  Serial.println("║   ESP32 WATERING SYSTEM HARDWARE TEST      ║");
  Serial.println("║   Version: 1.0.0                           ║");
  Serial.println("╚════════════════════════════════════════════╝");
  Serial.println();
  
  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Initialize pump pin
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  
  // Initialize valve pins
  for (int i = 0; i < NUM_VALVES; i++) {
    pinMode(VALVE_PINS[i], OUTPUT);
    digitalWrite(VALVE_PINS[i], LOW);
  }
  
  // Initialize rain sensor pins with internal pull-up
  for (int i = 0; i < NUM_VALVES; i++) {
    pinMode(RAIN_SENSOR_PINS[i], INPUT_PULLUP);
  }

  // Initialize water level sensor pin with internal pull-up
  pinMode(WATER_LEVEL_SENSOR_PIN, INPUT_PULLUP);

  // Initialize I2C for DS3231 RTC
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.println("I2C initialized (SDA: GPIO 14, SCL: GPIO 3)");

  Serial.println("Hardware initialized. All outputs set to LOW/OFF.");
  Serial.println();

  // Initialize LittleFS for web UI
  Serial.println("Initializing LittleFS...");
  if (!LittleFS.begin(false)) {
    Serial.println("⚠️ LittleFS mount failed, formatting...");
    if (!LittleFS.begin(true)) {
      Serial.println("❌ LittleFS format failed!");
    } else {
      Serial.println("✓ LittleFS formatted and mounted");
    }
  } else {
    Serial.println("✓ LittleFS mounted successfully");
  }

  // Connect to WiFi for OTA support
  Serial.println("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("Connecting to WiFi for OTA support...");
  Serial.print("SSID: ");
  Serial.println(SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, SSID_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("OTA Web Interface: http://");
    Serial.println(WiFi.localIP());
    Serial.println("/firmware");

    // Setup OTA
    setupOTA();
  } else {
    Serial.println("\n✗ WiFi Connection Failed!");
    Serial.println("OTA will not be available.");
    Serial.println("Test mode will work without WiFi.");
  }
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

  printMenu();
}

void printMenu() {
  Serial.println("═══════════════════════════════════════════════");
  Serial.println("              HARDWARE TEST MENU");
  Serial.println("═══════════════════════════════════════════════");
  Serial.println("LED TEST:");
  Serial.println("  L - Toggle LED (GPIO 2)");
  Serial.println();
  Serial.println("PUMP TEST:");
  Serial.println("  P - Toggle Pump (GPIO 4)");
  Serial.println();
  Serial.println("VALVE TESTS (Individual):");
  Serial.println("  1 - Toggle Valve 1 (GPIO 5)");
  Serial.println("  2 - Toggle Valve 2 (GPIO 6)");
  Serial.println("  3 - Toggle Valve 3 (GPIO 7)");
  Serial.println("  4 - Toggle Valve 4 (GPIO 15)");
  Serial.println("  5 - Toggle Valve 5 (GPIO 16)");
  Serial.println("  6 - Toggle Valve 6 (GPIO 17)");
  Serial.println();
  Serial.println("VALVE TESTS (All):");
  Serial.println("  A - Turn ALL valves ON");
  Serial.println("  Z - Turn ALL valves OFF");
  Serial.println();
  Serial.println("RAIN SENSOR TESTS:");
  Serial.println("  R - Read ALL rain sensors (once)");
  Serial.println("  M - Monitor ALL rain sensors (continuous)");
  Serial.println("  S - Stop monitoring");
  Serial.println();
  Serial.println("WATER LEVEL SENSOR TEST:");
  Serial.println("  W - Read water level sensor (GPIO 19)");
  Serial.println("  N - Monitor water level sensor (continuous)");
  Serial.println();
  Serial.println("DS3231 RTC TESTS:");
  Serial.println("  T - Read RTC time and temperature");
  Serial.println("  I - Scan I2C bus for devices");
  Serial.println();
  Serial.println("FULL SYSTEM TESTS:");
  Serial.println("  F - Full sequence test (all components)");
  Serial.println("  X - Turn EVERYTHING OFF (emergency stop)");
  Serial.println();
  Serial.println("OTHER:");
  Serial.println("  H - Show this menu");
  Serial.println("═══════════════════════════════════════════════");
  Serial.println("Enter command:");
}

void printSeparator() {
  Serial.println("───────────────────────────────────────────────");
}

void testLED() {
  static bool ledState = false;
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState);
  Serial.println("LED (GPIO 2): " + String(ledState ? "ON ✓" : "OFF ✗"));
  Serial.println("→ Check if onboard LED is " + String(ledState ? "lit" : "off"));
  printSeparator();
}

void testPump() {
  static bool pumpState = false;
  pumpState = !pumpState;
  digitalWrite(PUMP_PIN, pumpState);
  Serial.println("PUMP (GPIO 4): " + String(pumpState ? "ON ✓" : "OFF ✗"));
  Serial.println("→ Check if pump relay clicks and pump runs");
  Serial.println("⚠ WARNING: Make sure pump has water!");
  printSeparator();
}

void testValve(int valveNum) {
  if (valveNum < 1 || valveNum > 6) return;
  
  int idx = valveNum - 1;
  static bool valveStates[6] = {false, false, false, false, false, false};
  valveStates[idx] = !valveStates[idx];
  
  digitalWrite(VALVE_PINS[idx], valveStates[idx]);
  Serial.println("VALVE " + String(valveNum) + " (GPIO " + String(VALVE_PINS[idx]) + "): " + 
                 String(valveStates[idx] ? "OPEN ✓" : "CLOSED ✗"));
  Serial.println("→ Check if valve " + String(valveNum) + " relay clicks and valve opens/closes");
  printSeparator();
}

void testAllValvesOn() {
  Serial.println("Opening ALL valves...");
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], HIGH);
    Serial.println("  Valve " + String(i + 1) + " (GPIO " + String(VALVE_PINS[i]) + "): OPEN ✓");
    delay(200);
  }
  Serial.println("→ All valves should be open now");
  Serial.println("⚠ WARNING: Make sure you have enough water pressure!");
  printSeparator();
}

void testAllValvesOff() {
  Serial.println("Closing ALL valves...");
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], LOW);
    Serial.println("  Valve " + String(i + 1) + " (GPIO " + String(VALVE_PINS[i]) + "): CLOSED ✗");
    delay(200);
  }
  Serial.println("→ All valves should be closed now");
  printSeparator();
}

void readRainSensors() {
  Serial.println("RAIN SENSOR READINGS:");
  Serial.println("(LOW = Rain detected / Sensor wet)");
  Serial.println("(HIGH = Dry / No rain)");
  Serial.println();
  
  for (int i = 0; i < NUM_VALVES; i++) {
    int sensorValue = digitalRead(RAIN_SENSOR_PINS[i]);
    String status = (sensorValue == LOW) ? "WET/RAIN ☔" : "DRY ☀";
    Serial.println("  Sensor " + String(i + 1) + " (GPIO " + String(RAIN_SENSOR_PINS[i]) + "): " + 
                   String(sensorValue) + " = " + status);
  }
  Serial.println();
  Serial.println("→ Test by touching sensor with wet finger");
  printSeparator();
}

bool monitorMode = false;
bool waterLevelMonitorMode = false;
unsigned long lastMonitorTime = 0;

void monitorRainSensors() {
  unsigned long currentTime = millis();
  if (currentTime - lastMonitorTime >= 500) {
    Serial.println("\n╔══ RAIN SENSOR MONITOR (Press 'S' to stop) ══╗");
    for (int i = 0; i < NUM_VALVES; i++) {
      int sensorValue = digitalRead(RAIN_SENSOR_PINS[i]);
      String bar = (sensorValue == LOW) ? "████████" : "░░░░░░░░";
      String status = (sensorValue == LOW) ? "WET" : "DRY";
      Serial.println("Sensor " + String(i + 1) + " (GPIO " + String(RAIN_SENSOR_PINS[i]) + "): [" +
                     bar + "] " + status);
    }
    Serial.println("╚════════════════════════════════════════════════╝");
    lastMonitorTime = currentTime;
  }
}

void readWaterLevelSensor() {
  Serial.println("WATER LEVEL SENSOR READING:");
  Serial.println("(LOW = Water detected / Tank has water)");
  Serial.println("(HIGH = No water / Tank empty)");
  Serial.println();

  int sensorValue = digitalRead(WATER_LEVEL_SENSOR_PIN);
  String status = (sensorValue == LOW) ? "WATER DETECTED 💧" : "NO WATER/EMPTY ⚠️";
  Serial.println("  Water Level Sensor (GPIO " + String(WATER_LEVEL_SENSOR_PIN) + "): " +
                 String(sensorValue) + " = " + status);
  Serial.println();
  Serial.println("→ Sensor should show LOW when submerged in water");
  printSeparator();
}

void monitorWaterLevelSensor() {
  unsigned long currentTime = millis();
  if (currentTime - lastMonitorTime >= 500) {
    Serial.println("\n╔═══ WATER LEVEL MONITOR (Press 'S' to stop) ═══╗");
    int sensorValue = digitalRead(WATER_LEVEL_SENSOR_PIN);
    String bar = (sensorValue == LOW) ? "████████████████" : "░░░░░░░░░░░░░░░░";
    String status = (sensorValue == LOW) ? "WATER 💧" : "EMPTY ⚠️ ";
    Serial.println("Water Level (GPIO " + String(WATER_LEVEL_SENSOR_PIN) + "): [" +
                   bar + "] " + status);
    Serial.println("╚════════════════════════════════════════════════╝");
    lastMonitorTime = currentTime;
  }
}

// DS3231 Helper Functions
uint8_t bcdToDec(uint8_t val) {
  return (val / 16 * 10) + (val % 16);
}

uint8_t readDS3231Register(uint8_t reg) {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 1);
  return Wire.read();
}

void readDS3231Time() {
  Serial.println("DS3231 RTC READING:");
  Serial.println("I2C Address: 0x68");
  Serial.println();

  // Check if DS3231 is responding
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  byte error = Wire.endTransmission();

  if (error != 0) {
    Serial.println("❌ ERROR: DS3231 not found on I2C bus!");
    Serial.println("   Check connections:");
    Serial.println("   - SDA → GPIO 14");
    Serial.println("   - SCL → GPIO 3");
    Serial.println("   - VCC → 3.3V or 5V");
    Serial.println("   - GND → GND");
    printSeparator();
    return;
  }

  // Read time registers (0x00 to 0x06)
  uint8_t second = bcdToDec(readDS3231Register(0x00) & 0x7F);
  uint8_t minute = bcdToDec(readDS3231Register(0x01));
  uint8_t hour = bcdToDec(readDS3231Register(0x02) & 0x3F);
  uint8_t dayOfWeek = bcdToDec(readDS3231Register(0x03));
  uint8_t day = bcdToDec(readDS3231Register(0x04));
  uint8_t month = bcdToDec(readDS3231Register(0x05) & 0x1F);
  uint8_t year = bcdToDec(readDS3231Register(0x06));

  // Read temperature (0x11 and 0x12)
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0x11);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 2);
  int8_t tempMSB = Wire.read();
  uint8_t tempLSB = Wire.read();
  float temperature = tempMSB + ((tempLSB >> 6) * 0.25);

  Serial.println("✓ DS3231 Connected!");
  Serial.println();
  Serial.println("DATE & TIME:");
  Serial.printf("  %04d-%02d-%02d (20%02d-%02d-%02d)\n",
                2000 + year, month, day, year, month, day);

  const char* daysOfWeek[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  Serial.printf("  %s %02d:%02d:%02d\n",
                dayOfWeek >= 1 && dayOfWeek <= 7 ? daysOfWeek[dayOfWeek] : "???",
                hour, minute, second);
  Serial.println();
  Serial.println("TEMPERATURE:");
  Serial.printf("  %.2f °C (%.2f °F)\n", temperature, temperature * 9.0/5.0 + 32.0);
  Serial.println();
  Serial.println("→ If time is incorrect, you can set it via Arduino code");
  printSeparator();
}

void scanI2CBus() {
  Serial.println("I2C BUS SCANNER:");
  Serial.println("Scanning I2C bus (addresses 0x01 to 0x7F)...");
  Serial.println();

  int devicesFound = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    byte error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("✓ Device found at 0x");
      if (addr < 16) Serial.print("0");
      Serial.print(addr, HEX);

      // Identify known devices
      if (addr == 0x68) Serial.print(" (DS3231 RTC)");
      else if (addr == 0x57) Serial.print(" (AT24C32 EEPROM)");

      Serial.println();
      devicesFound++;
    }
  }

  Serial.println();
  if (devicesFound == 0) {
    Serial.println("❌ No I2C devices found!");
    Serial.println("   Check your wiring and power supply.");
  } else {
    Serial.println("Total devices found: " + String(devicesFound));
  }
  printSeparator();
}

void fullSequenceTest() {
  Serial.println("\n");
  Serial.println("╔════════════════════════════════════════════╗");
  Serial.println("║       FULL SEQUENCE TEST STARTING          ║");
  Serial.println("╚════════════════════════════════════════════╝");
  Serial.println();
  
  // Test LED
  Serial.println("1/4 Testing LED...");
  digitalWrite(LED_PIN, HIGH);
  delay(1000);
  digitalWrite(LED_PIN, LOW);
  Serial.println("    ✓ LED test complete");
  delay(1000);
  
  // Test Pump
  Serial.println("\n2/4 Testing Pump...");
  digitalWrite(PUMP_PIN, HIGH);
  Serial.println("    Pump ON for 3 seconds");
  delay(3000);
  digitalWrite(PUMP_PIN, LOW);
  Serial.println("    ✓ Pump test complete");
  delay(1000);
  
  // Test each valve individually
  Serial.println("\n3/4 Testing Valves (one by one)...");
  for (int i = 0; i < NUM_VALVES; i++) {
    Serial.println("    Testing Valve " + String(i + 1) + " (GPIO " + String(VALVE_PINS[i]) + ")...");
    digitalWrite(VALVE_PINS[i], HIGH);
    delay(2000);
    digitalWrite(VALVE_PINS[i], LOW);
    Serial.println("    ✓ Valve " + String(i + 1) + " complete");
    delay(500);
  }
  
  // Test rain sensors
  Serial.println("\n4/6 Testing Rain Sensors...");
  readRainSensors();

  // Test water level sensor
  Serial.println("\n5/6 Testing Water Level Sensor...");
  readWaterLevelSensor();

  // Test DS3231 RTC
  Serial.println("\n6/6 Testing DS3231 RTC...");
  readDS3231Time();

  Serial.println("\n╔════════════════════════════════════════════╗");
  Serial.println("║       FULL SEQUENCE TEST COMPLETE          ║");
  Serial.println("╚════════════════════════════════════════════╝");
  Serial.println();
  printSeparator();
}

void emergencyStop() {
  Serial.println("\n⚠️ EMERGENCY STOP - TURNING EVERYTHING OFF ⚠️");
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], LOW);
  }
  Serial.println("✓ All outputs disabled");
  printSeparator();
}

// ============================================
// OTA Web Server
// ============================================
WebServer otaServer(80);

void serveFile(const char* path, const char* contentType) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    otaServer.send(404, "text/plain", "File not found");
    return;
  }
  otaServer.streamFile(file, contentType);
  file.close();
}

void handleOTAPage() {
  if (!otaServer.authenticate(OTA_USER, OTA_PASSWORD)) {
    return otaServer.requestAuthentication();
  }
  serveFile("/test/firmware.html", "text/html");
}

void handleOTAUpdate() {
  HTTPUpload& upload = otaServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA Update: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA Update Success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleOTAUpdateComplete() {
  otaServer.send(200, "text/plain", "OK");
  delay(1000);
  ESP.restart();
}

void handleRoot() {
  serveFile("/test/index.html", "text/html");
}

void handleDeviceInfo() {
  String json = "{";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"heap\":" + String(ESP.getFreeHeap() / 1024);
  json += "}";
  otaServer.send(200, "application/json", json);
}

void setupOTA() {
  otaServer.on("/", HTTP_GET, handleRoot);
  otaServer.on("/firmware", HTTP_GET, handleOTAPage);
  otaServer.on("/api/info", HTTP_GET, handleDeviceInfo);
  otaServer.on("/update", HTTP_POST, handleOTAUpdateComplete, handleOTAUpdate);
  otaServer.begin();
  Serial.println("✓ OTA Web Server started");
}

void loop() {
  // Handle OTA requests (if WiFi connected)
  if (WiFi.status() == WL_CONNECTED) {
    otaServer.handleClient();
  }

  // Handle monitoring modes
  if (monitorMode) {
    monitorRainSensors();
  }
  if (waterLevelMonitorMode) {
    monitorWaterLevelSensor();
  }

  // Check for serial input
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    
    // Clear any remaining characters in buffer
    while (Serial.available() > 0) {
      Serial.read();
    }
    
    Serial.println("\nCommand: " + String(cmd));
    Serial.println();
    
    switch (cmd) {
      case 'L':
      case 'l':
        testLED();
        break;
        
      case 'P':
      case 'p':
        testPump();
        break;
        
      case '1':
        testValve(1);
        break;
      case '2':
        testValve(2);
        break;
      case '3':
        testValve(3);
        break;
      case '4':
        testValve(4);
        break;
      case '5':
        testValve(5);
        break;
      case '6':
        testValve(6);
        break;
        
      case 'A':
      case 'a':
        testAllValvesOn();
        break;
        
      case 'Z':
      case 'z':
        testAllValvesOff();
        break;
        
      case 'R':
      case 'r':
        readRainSensors();
        break;
        
      case 'M':
      case 'm':
        monitorMode = true;
        Serial.println("→ Rain sensor monitoring ENABLED");
        Serial.println("  (Press 'S' to stop)");
        printSeparator();
        break;
        
      case 'S':
      case 's':
        monitorMode = false;
        waterLevelMonitorMode = false;
        Serial.println("→ All monitoring STOPPED");
        printSeparator();
        break;

      case 'W':
      case 'w':
        readWaterLevelSensor();
        break;

      case 'N':
      case 'n':
        waterLevelMonitorMode = true;
        Serial.println("→ Water level sensor monitoring ENABLED");
        Serial.println("  (Press 'S' to stop)");
        printSeparator();
        break;

      case 'T':
      case 't':
        readDS3231Time();
        break;

      case 'I':
      case 'i':
        scanI2CBus();
        break;

      case 'F':
      case 'f':
        fullSequenceTest();
        break;
        
      case 'X':
      case 'x':
        emergencyStop();
        break;
        
      case 'H':
      case 'h':
      case '?':
        printMenu();
        break;
        
      default:
        Serial.println("Unknown command. Press 'H' for menu.");
        printSeparator();
        break;
    }
  }
  
  delay(10);
}
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Update.h>
#include <LittleFS.h>
#include <secret.h>
#include <Adafruit_NeoPixel.h>

// Forward declarations
void printMenu();
void setupOTA();
void handleOTA();
void webLog(const String& message);
void readDS3231Time();

// WebSocket server on port 81
WebSocketsServer webSocket = WebSocketsServer(81);

// Command queue for WebSocket commands
String pendingCommand = "";

// Global variable to store time data from WebSocket
struct TimeData {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t dayOfWeek;
  uint8_t day;
  uint8_t month;
  uint8_t year;
  bool hasData;
} pendingTimeData = {0, 0, 0, 0, 0, 0, 0, false};

// Helper function to log to both Serial and WebSocket
void webLog(const String& message) {
  Serial.println(message);
  String msg = message; // Create non-const copy
  webSocket.broadcastTXT(msg);
}

// WebSocket event handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] WebSocket Disconnected\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] WebSocket Connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
        webSocket.sendTXT(num, "✓ Connected to ESP32 Hardware Test");
      }
      break;
    case WStype_TEXT:
      {
        // Command received from web dashboard
        String cmd = String((char*)payload);
        cmd.trim();

        // Handle heartbeat PING
        if (cmd == "PING") {
          webSocket.sendTXT(num, "PONG");
          return;
        }

        // Check if this is a time data packet (format: TIME:YYYY,MM,DD,HH,MM,SS,DOW)
        if (cmd.startsWith("TIME:")) {
          String timeStr = cmd.substring(5);
          int year, month, day, hour, minute, second, dow;

          if (sscanf(timeStr.c_str(), "%d,%d,%d,%d,%d,%d,%d",
                     &year, &month, &day, &hour, &minute, &second, &dow) == 7) {
            // Store the time data
            pendingTimeData.year = year - 2000;  // Convert to years since 2000
            pendingTimeData.month = month;
            pendingTimeData.day = day;
            pendingTimeData.hour = hour;
            pendingTimeData.minute = minute;
            pendingTimeData.second = second;
            pendingTimeData.dayOfWeek = dow;
            pendingTimeData.hasData = true;

            Serial.printf("[WebSocket] Time data received: 20%02d-%02d-%02d %02d:%02d:%02d (DOW:%d)\n",
                         pendingTimeData.year, pendingTimeData.month, pendingTimeData.day,
                         pendingTimeData.hour, pendingTimeData.minute, pendingTimeData.second,
                         pendingTimeData.dayOfWeek);

            // Trigger the set RTC command
            pendingCommand = "U";
          }
        }
        else if (cmd.length() >= 1) {
          pendingCommand = cmd;
          Serial.printf("[WebSocket] Command queued: %s\n", cmd.c_str());
        }
      }
      break;
  }
}

// Pin definitions for ESP32-S3-N8R2
#define LED_PIN 48  // Built-in RGB NeoPixel LED
#define NUM_LEDS 1

// NeoPixel LED object (global)
Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

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
#define RAIN_SENSOR_POWER_PIN 18  // Optocoupler control for sensor power

// Water level sensor pin
#define WATER_LEVEL_SENSOR_PIN 19

// Master Overflow Sensor pin (2N2222 transistor circuit)
#define MASTER_OVERFLOW_SENSOR_PIN 42  // LOW = overflow detected, HIGH = normal

// Overflow Sensor Debouncing Constants (same as production)
const int OVERFLOW_DEBOUNCE_SAMPLES = 7;        // Number of readings to take
const int OVERFLOW_DEBOUNCE_THRESHOLD = 5;      // Minimum LOW readings to declare overflow (5 out of 7)
const unsigned long OVERFLOW_DEBOUNCE_DELAY_MS = 5; // Delay between readings (5ms)

// DS3231 RTC I2C pins
#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 3
#define DS3231_I2C_ADDRESS 0x68

// DS3231 Battery Measurement pins
#define BATTERY_ADC_PIN 1        // ADC pin (reads voltage divider)
#define BATTERY_CONTROL_PIN 2    // Controls transistor (HIGH = measure, LOW = off)

// Battery measurement calibration
// Adjust this value to match your multimeter reading
// Formula: CALIBRATION_FACTOR = (multimeter_voltage / program_voltage)
// Example: If multimeter shows 3.23V and program shows 3.02V:
//          CALIBRATION_FACTOR = 3.23 / 3.02 = 1.0695
const float BATTERY_VOLTAGE_CALIBRATION = 1.0695;  // Calibrated for your setup

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

  // Initialize NeoPixel LED
  pixels.begin();
  pixels.clear();
  pixels.show();  // Initialize all pixels to 'off'
  Serial.println("RGB NeoPixel LED initialized (GPIO 48)");

  // Initialize pump pin
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  
  // Initialize valve pins
  for (int i = 0; i < NUM_VALVES; i++) {
    pinMode(VALVE_PINS[i], OUTPUT);
    digitalWrite(VALVE_PINS[i], LOW);
  }

  // Initialize rain sensor power pin
  pinMode(RAIN_SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);  // Sensors off by default

  // Initialize rain sensor pins with internal pull-up
  for (int i = 0; i < NUM_VALVES; i++) {
    pinMode(RAIN_SENSOR_PINS[i], INPUT_PULLUP);
  }

  // Initialize water level sensor pin with internal pull-up
  pinMode(WATER_LEVEL_SENSOR_PIN, INPUT_PULLUP);

  // Initialize master overflow sensor pin with internal pull-up
  pinMode(MASTER_OVERFLOW_SENSOR_PIN, INPUT_PULLUP);

  // Initialize I2C for DS3231 RTC
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.println("I2C initialized (SDA: GPIO 14, SCL: GPIO 3)");

  // Initialize battery measurement pins
  pinMode(BATTERY_CONTROL_PIN, OUTPUT);
  digitalWrite(BATTERY_CONTROL_PIN, LOW);  // Transistor OFF by default
  pinMode(BATTERY_ADC_PIN, INPUT);         // ADC input

  // Configure ADC for battery measurement (0-3.3V range)
  analogReadResolution(12);  // 12-bit resolution (0-4095)
  analogSetAttenuation(ADC_11db);  // 0-3.3V range
  Serial.println("Battery measurement initialized (GPIO 1: ADC, GPIO 2: Control)");

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

  // Mask SSID: show only first and last character
  String maskedSSID = String(SSID);
  if (maskedSSID.length() > 2) {
    maskedSSID = maskedSSID.substring(0, 1) + "****" + maskedSSID.substring(maskedSSID.length() - 1);
  } else {
    maskedSSID = "****";
  }
  Serial.println(maskedSSID);

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
    Serial.print("Web Dashboard: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/dashboard");
    Serial.print("OTA Interface: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/firmware");

    // Setup OTA and WebSocket
    setupOTA();

    // Start WebSocket server
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.println("✓ WebSocket server started on port 81");
  } else {
    Serial.println("\n✗ WiFi Connection Failed!");
    Serial.println("OTA and WebSocket will not be available.");
    Serial.println("Test mode will work without WiFi.");
  }
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

  printMenu();
}

void printMenu() {
  Serial.println("═══════════════════════════════════════════════");
  Serial.println("              HARDWARE TEST MENU");
  Serial.println("═══════════════════════════════════════════════");
  Serial.println("RGB LED TEST:");
  Serial.println("  L - Cycle RGB LED colors (GPIO 48 NeoPixel)");
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
  Serial.println("  R1-R6 - Read specific sensor (e.g., R1, R6)");
  Serial.println("  M - Monitor ALL rain sensors (continuous)");
  Serial.println("  M1-M6 - Monitor specific sensor (e.g., M1, M6)");
  Serial.println("  S - Stop monitoring");
  Serial.println();
  Serial.println("WATER LEVEL SENSOR TEST:");
  Serial.println("  W - Read water level sensor (GPIO 19)");
  Serial.println("  N - Monitor water level sensor (continuous)");
  Serial.println();
  Serial.println("MASTER OVERFLOW SENSOR TEST:");
  Serial.println("  O - Read master overflow sensor (GPIO 42) - RAW single reading");
  Serial.println("  D - Read master overflow sensor (GPIO 42) - DEBOUNCED production logic");
  Serial.println("  V - Monitor master overflow sensor (continuous)");
  Serial.println();
  Serial.println("DS3231 RTC TESTS:");
  Serial.println("  T - Read RTC time and temperature");
  Serial.println("  I - Scan I2C bus for devices");
  Serial.println("  U - Set RTC to current time (use dashboard)");
  Serial.println("  K - Reset RTC to epoch (2000-01-01 00:00:00)");
  Serial.println("  B - Read battery voltage (VBAT)");
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
  webLog("───────────────────────────────────────────────");
}

void testLED() {
  static uint8_t colorIndex = 0;

  // Cycle through colors: OFF → RED → GREEN → BLUE → YELLOW → CYAN → MAGENTA → WHITE → OFF
  const uint32_t colors[] = {
    pixels.Color(0, 0, 0),       // OFF
    pixels.Color(255, 0, 0),     // RED
    pixels.Color(0, 255, 0),     // GREEN
    pixels.Color(0, 0, 255),     // BLUE
    pixels.Color(255, 255, 0),   // YELLOW
    pixels.Color(0, 255, 255),   // CYAN
    pixels.Color(255, 0, 255),   // MAGENTA
    pixels.Color(255, 255, 255)  // WHITE
  };
  const char* colorNames[] = {"OFF", "RED", "GREEN", "BLUE", "YELLOW", "CYAN", "MAGENTA", "WHITE"};

  pixels.setPixelColor(0, colors[colorIndex]);
  pixels.show();

  webLog("RGB LED (GPIO 48): " + String(colorNames[colorIndex]));
  webLog("→ Check if onboard RGB LED shows " + String(colorNames[colorIndex]));

  colorIndex = (colorIndex + 1) % 8;
  printSeparator();
}

void testPump() {
  static bool pumpState = false;
  pumpState = !pumpState;
  digitalWrite(PUMP_PIN, pumpState);
  webLog("PUMP (GPIO 4): " + String(pumpState ? "ON ✓" : "OFF ✗"));
  webLog("→ Check if pump relay clicks and pump runs");
  webLog("⚠ WARNING: Make sure pump has water!");
  printSeparator();
}

void testValve(int valveNum) {
  if (valveNum < 1 || valveNum > 6) return;

  int idx = valveNum - 1;
  static bool valveStates[6] = {false, false, false, false, false, false};
  valveStates[idx] = !valveStates[idx];

  digitalWrite(VALVE_PINS[idx], valveStates[idx]);
  webLog("VALVE " + String(valveNum) + " (GPIO " + String(VALVE_PINS[idx]) + "): " +
         String(valveStates[idx] ? "OPEN ✓" : "CLOSED ✗"));
  webLog("→ Check if valve " + String(valveNum) + " relay clicks and valve opens/closes");
  printSeparator();
}

void testAllValvesOn() {
  webLog("Opening ALL valves...");
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], HIGH);
    webLog("  Valve " + String(i + 1) + " (GPIO " + String(VALVE_PINS[i]) + "): OPEN ✓");
    delay(200);
  }
  webLog("→ All valves should be open now");
  webLog("⚠ WARNING: Make sure you have enough water pressure!");
  printSeparator();
}

void testAllValvesOff() {
  webLog("Closing ALL valves...");
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], LOW);
    webLog("  Valve " + String(i + 1) + " (GPIO " + String(VALVE_PINS[i]) + "): CLOSED ✗");
    delay(200);
  }
  webLog("→ All valves should be closed now");
  printSeparator();
}

void readRainSensors() {
  webLog("RAIN SENSOR READINGS:");
  webLog("(LOW = Rain detected / Sensor wet)");
  webLog("(HIGH = Dry / No rain)");
  webLog("");

  // Power on sensors (requires both valve pins + GPIO 18)
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], HIGH);
  }
  digitalWrite(RAIN_SENSOR_POWER_PIN, HIGH);
  delay(100);  // Wait for stabilization

  // Read and display sensor values
  for (int i = 0; i < NUM_VALVES; i++) {
    int sensorValue = digitalRead(RAIN_SENSOR_PINS[i]);
    String status = (sensorValue == LOW) ? "WET/RAIN ☔" : "DRY ☀";
    webLog("  Sensor " + String(i + 1) + " (GPIO " + String(RAIN_SENSOR_PINS[i]) + "): " +
           String(sensorValue) + " = " + status);
  }

  // Power off everything
  digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], LOW);
  }

  webLog("");
  webLog("→ Test by touching sensor with wet finger");
  printSeparator();
}

void readSpecificRainSensor(int sensorIndex) {
  webLog("RAIN SENSOR " + String(sensorIndex + 1) + " READING:");
  webLog("(LOW = Rain detected / Sensor wet)");
  webLog("(HIGH = Dry / No rain)");
  webLog("");

  // Power on specific sensor (valve pin + GPIO 18)
  digitalWrite(VALVE_PINS[sensorIndex], HIGH);
  digitalWrite(RAIN_SENSOR_POWER_PIN, HIGH);
  delay(100);  // Wait for stabilization

  // Read and display sensor value
  int sensorValue = digitalRead(RAIN_SENSOR_PINS[sensorIndex]);
  String status = (sensorValue == LOW) ? "WET/RAIN ☔" : "DRY ☀";
  webLog("  Sensor " + String(sensorIndex + 1) +
         " (Valve GPIO " + String(VALVE_PINS[sensorIndex]) +
         ", Sensor GPIO " + String(RAIN_SENSOR_PINS[sensorIndex]) + "): " +
         String(sensorValue) + " = " + status);

  // Power off
  digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
  digitalWrite(VALVE_PINS[sensorIndex], LOW);

  webLog("");
  webLog("→ Test by touching sensor with wet finger");
  printSeparator();
}

bool monitorMode = false;
int monitorSensorIndex = -1;  // -1 = all sensors, 0-5 = specific sensor
bool waterLevelMonitorMode = false;
bool overflowMonitorMode = false;
unsigned long lastMonitorTime = 0;

void monitorRainSensors() {
  unsigned long currentTime = millis();
  if (currentTime - lastMonitorTime >= 500) {
    webLog("");
    if (monitorSensorIndex == -1) {
      // Monitor all sensors
      webLog("╔══ RAIN SENSOR MONITOR (Press 'S' to stop) ══╗");
      for (int i = 0; i < NUM_VALVES; i++) {
        int sensorValue = digitalRead(RAIN_SENSOR_PINS[i]);
        String bar = (sensorValue == LOW) ? "████████" : "░░░░░░░░";
        String status = (sensorValue == LOW) ? "WET" : "DRY";
        webLog("Sensor " + String(i + 1) + " (GPIO " + String(RAIN_SENSOR_PINS[i]) + "): [" +
               bar + "] " + status);
      }
      webLog("╚════════════════════════════════════════════════╝");
    } else {
      // Monitor specific sensor
      int i = monitorSensorIndex;
      int sensorValue = digitalRead(RAIN_SENSOR_PINS[i]);
      String bar = (sensorValue == LOW) ? "████████" : "░░░░░░░░";
      String status = (sensorValue == LOW) ? "WET" : "DRY";
      webLog("╔══ SENSOR " + String(i + 1) + " MONITOR (Press 'S' to stop) ══╗");
      webLog("Sensor " + String(i + 1) +
             " (Valve GPIO " + String(VALVE_PINS[i]) +
             ", Sensor GPIO " + String(RAIN_SENSOR_PINS[i]) + "): [" +
             bar + "] " + status);
      webLog("╚════════════════════════════════════════════════╝");
    }
    lastMonitorTime = currentTime;
  }
}

void readWaterLevelSensor() {
  webLog("WATER LEVEL SENSOR READING:");
  webLog("(HIGH = Water detected / Tank has water)");
  webLog("(LOW = No water / Tank empty)");
  webLog("");

  int sensorValue = digitalRead(WATER_LEVEL_SENSOR_PIN);
  String status = (sensorValue == HIGH) ? "WATER DETECTED 💧" : "NO WATER/EMPTY ⚠️";
  webLog("  Water Level Sensor (GPIO " + String(WATER_LEVEL_SENSOR_PIN) + "): " +
         String(sensorValue) + " = " + status);
  webLog("");
  webLog("→ Sensor should show HIGH when submerged in water");
  printSeparator();
}

void monitorWaterLevelSensor() {
  unsigned long currentTime = millis();
  if (currentTime - lastMonitorTime >= 500) {
    webLog("");
    webLog("╔═══ WATER LEVEL MONITOR (Press 'S' to stop) ═══╗");
    int sensorValue = digitalRead(WATER_LEVEL_SENSOR_PIN);
    String bar = (sensorValue == HIGH) ? "████████████████" : "░░░░░░░░░░░░░░░░";
    String status = (sensorValue == HIGH) ? "WATER 💧" : "EMPTY ⚠️ ";
    webLog("Water Level (GPIO " + String(WATER_LEVEL_SENSOR_PIN) + "): [" +
           bar + "] " + status);
    webLog("╚════════════════════════════════════════════════╝");
    lastMonitorTime = currentTime;
  }
}

void readMasterOverflowSensor() {
  webLog("MASTER OVERFLOW SENSOR READING:");
  webLog("(LOW = Overflow detected / Water present)");
  webLog("(HIGH = Normal / Dry)");
  webLog("");
  webLog("Circuit: Rain sensor → 2N2222 transistor → GPIO 42");
  webLog("NOTE: Test mode shows RAW single readings for diagnostics");
  webLog("      Production uses debounced multi-sample reading (5/7 threshold)");
  webLog("");

  int sensorValue = digitalRead(MASTER_OVERFLOW_SENSOR_PIN);
  String status = (sensorValue == LOW) ? "⚠️ OVERFLOW DETECTED! ⚠️" : "✓ NORMAL (Dry)";
  String emoji = (sensorValue == LOW) ? "💧🚨" : "✓";

  webLog("  Master Overflow Sensor (GPIO " + String(MASTER_OVERFLOW_SENSOR_PIN) + "): " +
         String(sensorValue) + " = " + status);
  webLog("");

  if (sensorValue == LOW) {
    webLog("⚠️ WARNING: Water overflow detected!");
    webLog("   Check trays for overflow condition");
    webLog("   In production, this triggers emergency stop");
  } else {
    webLog("✓ No overflow - system is safe to operate");
  }

  webLog("");
  webLog("→ Test by wetting the rain sensor to simulate overflow");
  printSeparator();
}

void readMasterOverflowSensorDebounced() {
  webLog("MASTER OVERFLOW SENSOR READING (DEBOUNCED):");
  webLog("(Production-grade multi-sample reading with noise filtering)");
  webLog("");
  webLog("Circuit: Rain sensor → 2N2222 transistor → GPIO 42");
  webLog("Algorithm: " + String(OVERFLOW_DEBOUNCE_SAMPLES) + " samples, " +
         String(OVERFLOW_DEBOUNCE_DELAY_MS) + "ms delay, " +
         String(OVERFLOW_DEBOUNCE_THRESHOLD) + "/" + String(OVERFLOW_DEBOUNCE_SAMPLES) + " threshold");
  webLog("");

  // Software debouncing: Take multiple readings (same as production)
  int lowReadings = 0;
  String readingSequence = "";

  webLog("Taking " + String(OVERFLOW_DEBOUNCE_SAMPLES) + " readings with " +
         String(OVERFLOW_DEBOUNCE_DELAY_MS) + "ms delays...");
  webLog("");

  for (int i = 0; i < OVERFLOW_DEBOUNCE_SAMPLES; i++) {
    int reading = digitalRead(MASTER_OVERFLOW_SENSOR_PIN);
    if (reading == LOW) {
      lowReadings++;
      readingSequence += "L";
    } else {
      readingSequence += "H";
    }

    // Add space between readings for readability
    if (i < OVERFLOW_DEBOUNCE_SAMPLES - 1) {
      readingSequence += " ";
      delay(OVERFLOW_DEBOUNCE_DELAY_MS);
    }
  }

  webLog("Reading sequence: [" + readingSequence + "]");
  webLog("  (L=LOW/wet, H=HIGH/dry)");
  webLog("");
  webLog("Results:");
  webLog("  LOW readings:  " + String(lowReadings) + "/" + String(OVERFLOW_DEBOUNCE_SAMPLES));
  webLog("  HIGH readings: " + String(OVERFLOW_DEBOUNCE_SAMPLES - lowReadings) + "/" + String(OVERFLOW_DEBOUNCE_SAMPLES));
  webLog("  Threshold:     " + String(OVERFLOW_DEBOUNCE_THRESHOLD) + " LOW readings required");
  webLog("");

  // Determine final state (same logic as production)
  bool overflowDetected = (lowReadings >= OVERFLOW_DEBOUNCE_THRESHOLD);

  if (overflowDetected) {
    webLog("╔════════════════════════════════════════════════╗");
    webLog("║  ⚠️  OVERFLOW DETECTED! ⚠️                      ║");
    webLog("╚════════════════════════════════════════════════╝");
    webLog("");
    webLog("✓ Detection confirmed: " + String(lowReadings) + " out of " +
           String(OVERFLOW_DEBOUNCE_SAMPLES) + " readings were LOW");
    webLog("⚠️  In production, this triggers emergency stop:");
    webLog("   • All valves CLOSED");
    webLog("   • Pump STOPPED");
    webLog("   • System LOCKED until /reset_overflow");
  } else {
    webLog("╔════════════════════════════════════════════════╗");
    webLog("║  ✓ NORMAL (No overflow)                       ║");
    webLog("╚════════════════════════════════════════════════╝");
    webLog("");
    webLog("✓ System safe: Only " + String(lowReadings) + " out of " +
           String(OVERFLOW_DEBOUNCE_SAMPLES) + " readings were LOW");
    webLog("✓ Threshold not met (" + String(OVERFLOW_DEBOUNCE_THRESHOLD) + " required)");
    if (lowReadings > 0) {
      webLog("ℹ️  Note: Some noise detected (" + String(lowReadings) +
             " spikes), but filtered by debouncing");
    }
  }

  webLog("");
  webLog("→ This is the SAME logic used in production firmware");
  webLog("→ Filters electrical noise from pump/valve switching");
  printSeparator();
}

void monitorMasterOverflowSensor() {
  unsigned long currentTime = millis();
  if (currentTime - lastMonitorTime >= 500) {
    webLog("");
    webLog("╔═ OVERFLOW SENSOR MONITOR (Press 'S' to stop) ═╗");
    int sensorValue = digitalRead(MASTER_OVERFLOW_SENSOR_PIN);
    String bar = (sensorValue == LOW) ? "████████████████" : "░░░░░░░░░░░░░░░░";
    String status = (sensorValue == LOW) ? "OVERFLOW 🚨" : "NORMAL ✓";
    webLog("Overflow (GPIO " + String(MASTER_OVERFLOW_SENSOR_PIN) + "): [" +
           bar + "] " + status);

    if (sensorValue == LOW) {
      webLog("⚠️ EMERGENCY: Water overflow detected!");
    }

    webLog("╚════════════════════════════════════════════════╝");
    lastMonitorTime = currentTime;
  }
}

// DS3231 Helper Functions
uint8_t bcdToDec(uint8_t val) {
  return (val / 16 * 10) + (val % 16);
}

uint8_t decToBcd(uint8_t val) {
  return ((val / 10 * 16) + (val % 10));
}

uint8_t readDS3231Register(uint8_t reg) {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 1);
  return Wire.read();
}

void writeDS3231Register(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void setDS3231Time(uint8_t second, uint8_t minute, uint8_t hour, uint8_t dayOfWeek, uint8_t day, uint8_t month, uint8_t year) {
  writeDS3231Register(0x00, decToBcd(second));
  writeDS3231Register(0x01, decToBcd(minute));
  writeDS3231Register(0x02, decToBcd(hour));
  writeDS3231Register(0x03, decToBcd(dayOfWeek));
  writeDS3231Register(0x04, decToBcd(day));
  writeDS3231Register(0x05, decToBcd(month));
  writeDS3231Register(0x06, decToBcd(year));
}

void setRTCFromBrowser() {
  webLog("SET DS3231 RTC FROM BROWSER TIME:");
  webLog("");

  // Check if DS3231 is responding
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  byte error = Wire.endTransmission();

  if (error != 0) {
    webLog("❌ ERROR: DS3231 not found on I2C bus!");
    printSeparator();
    return;
  }

  if (!pendingTimeData.hasData) {
    webLog("❌ ERROR: No time data received from browser!");
    webLog("   This command should be triggered from the web dashboard.");
    printSeparator();
    return;
  }

  // Display the time to be set
  const char* daysOfWeek[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  char buffer[50];
  snprintf(buffer, sizeof(buffer), "20%02d-%02d-%02d %s %02d:%02d:%02d",
           pendingTimeData.year, pendingTimeData.month, pendingTimeData.day,
           daysOfWeek[pendingTimeData.dayOfWeek],
           pendingTimeData.hour, pendingTimeData.minute, pendingTimeData.second);

  webLog("Setting RTC to browser time:");
  webLog("  " + String(buffer));
  webLog("");

  // Write to DS3231
  setDS3231Time(
    pendingTimeData.second,
    pendingTimeData.minute,
    pendingTimeData.hour,
    pendingTimeData.dayOfWeek,
    pendingTimeData.day,
    pendingTimeData.month,
    pendingTimeData.year
  );

  // Clear the data
  pendingTimeData.hasData = false;

  delay(100);
  webLog("✓ DS3231 RTC updated successfully!");
  webLog("");
  webLog("Verifying RTC time...");
  delay(500);
  readDS3231Time();
}

void resetRTCToEpoch() {
  webLog("RESET DS3231 RTC TO EPOCH:");
  webLog("");

  // Check if DS3231 is responding
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  byte error = Wire.endTransmission();

  if (error != 0) {
    webLog("❌ ERROR: DS3231 not found on I2C bus!");
    printSeparator();
    return;
  }

  webLog("Setting RTC to: 2000-01-01 Saturday 00:00:00");

  // Set to epoch: 2000-01-01 00:00:00 (Saturday)
  setDS3231Time(
    0,   // second
    0,   // minute
    0,   // hour
    7,   // dayOfWeek (7 = Saturday, 2000-01-01 was Saturday)
    1,   // day
    1,   // month
    0    // year (0 = 2000)
  );

  delay(100);
  webLog("✓ RTC reset to epoch!");
  webLog("");
  webLog("Verifying RTC time...");
  delay(500);
  readDS3231Time();
}

void readBatteryVoltage() {
  webLog("DS3231 BATTERY VOLTAGE MEASUREMENT:");
  webLog("");
  webLog("Circuit: VBAT → 100kΩ → GPIO1(ADC) → 100kΩ → Transistor → GND");
  webLog("Control: GPIO2 → 10kΩ → Transistor Base");
  webLog("");

  // Turn on transistor to enable voltage divider
  digitalWrite(BATTERY_CONTROL_PIN, HIGH);
  webLog("Enabling measurement circuit...");
  delay(100);  // Wait for circuit to stabilize

  // Take multiple readings and average them
  const int numReadings = 10;
  long adcSum = 0;

  for (int i = 0; i < numReadings; i++) {
    adcSum += analogRead(BATTERY_ADC_PIN);
    delay(10);
  }

  // Turn off transistor to save battery
  digitalWrite(BATTERY_CONTROL_PIN, LOW);
  webLog("Measurement circuit disabled.");
  webLog("");

  // Calculate average ADC value
  float adcAverage = adcSum / (float)numReadings;

  // Convert to voltage (ESP32-S3 ADC with 11db attenuation: 0-3.3V → 0-4095)
  // Note: ESP32 ADC is non-linear, this is approximate
  float adcVoltage = (adcAverage / 4095.0) * 3.3;

  // Battery voltage is 2x the ADC voltage (voltage divider with R1=R2=100kΩ)
  float batteryVoltageRaw = adcVoltage * 2.0;

  // Apply calibration factor to correct ESP32 ADC non-linearity
  float batteryVoltage = batteryVoltageRaw * BATTERY_VOLTAGE_CALIBRATION;

  char buffer[100];
  webLog("MEASUREMENT RESULTS:");
  snprintf(buffer, sizeof(buffer), "  ADC Raw Value: %.0f (average of %d readings)", adcAverage, numReadings);
  webLog(buffer);
  snprintf(buffer, sizeof(buffer), "  ADC Voltage: %.3f V", adcVoltage);
  webLog(buffer);
  snprintf(buffer, sizeof(buffer), "  Battery Voltage (raw): %.3f V", batteryVoltageRaw);
  webLog(buffer);
  snprintf(buffer, sizeof(buffer), "  Battery Voltage (calibrated): %.3f V", batteryVoltage);
  webLog(buffer);
  snprintf(buffer, sizeof(buffer), "  Calibration Factor: %.4f", BATTERY_VOLTAGE_CALIBRATION);
  webLog(buffer);
  webLog("");

  // Battery status interpretation (CR2032 typical voltages)
  webLog("BATTERY STATUS:");
  if (batteryVoltage >= 2.8) {
    webLog("  ✓ GOOD (≥2.8V) - Battery is healthy");
  } else if (batteryVoltage >= 2.5) {
    webLog("  ⚠️ FAIR (2.5-2.8V) - Battery is usable but aging");
  } else if (batteryVoltage >= 2.0) {
    webLog("  ⚠️ LOW (2.0-2.5V) - Consider replacing soon");
  } else if (batteryVoltage >= 1.5) {
    webLog("  ❌ CRITICAL (<2.0V) - Replace battery immediately");
  } else {
    webLog("  ❌ ERROR - Check circuit connections");
  }
  webLog("");

  webLog("CIRCUIT NOTES:");
  webLog("  • Measurement only active when GPIO2 is HIGH");
  webLog("  • Voltage divider draws ~15µA during measurement");
  webLog("  • CR2032 nominal: 3.0V, min: 2.0V");
  webLog("");
  webLog("CALIBRATION:");
  webLog("  To recalibrate, measure battery with multimeter,");
  webLog("  then update BATTERY_VOLTAGE_CALIBRATION in code:");
  webLog("  CALIBRATION = (multimeter_reading / raw_reading)");
  printSeparator();
}

void readDS3231Time() {
  webLog("DS3231 RTC READING:");
  webLog("I2C Address: 0x68");
  webLog("");

  // Check if DS3231 is responding
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  byte error = Wire.endTransmission();

  if (error != 0) {
    webLog("❌ ERROR: DS3231 not found on I2C bus!");
    webLog("   Check connections:");
    webLog("   - SDA → GPIO 14");
    webLog("   - SCL → GPIO 3");
    webLog("   - VCC → 3.3V or 5V");
    webLog("   - GND → GND");
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

  webLog("✓ DS3231 Connected!");
  webLog("");
  webLog("DATE & TIME:");

  char buffer[100];
  snprintf(buffer, sizeof(buffer), "  %04d-%02d-%02d (20%02d-%02d-%02d)",
           2000 + year, month, day, year, month, day);
  webLog(buffer);

  const char* daysOfWeek[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  snprintf(buffer, sizeof(buffer), "  %s %02d:%02d:%02d",
           dayOfWeek >= 1 && dayOfWeek <= 7 ? daysOfWeek[dayOfWeek] : "???",
           hour, minute, second);
  webLog(buffer);

  webLog("");
  webLog("TEMPERATURE:");
  snprintf(buffer, sizeof(buffer), "  %.2f °C (%.2f °F)", temperature, temperature * 9.0/5.0 + 32.0);
  webLog(buffer);
  webLog("");
  webLog("→ Use 'U' to set time or 'K' to reset");
  printSeparator();
}

void scanI2CBus() {
  webLog("I2C BUS SCANNER:");
  webLog("Scanning I2C bus (addresses 0x01 to 0x7F)...");
  webLog("");

  int devicesFound = 0;
  char buffer[100];

  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    byte error = Wire.endTransmission();

    if (error == 0) {
      String device = "";
      if (addr == 0x68) device = " (DS3231 RTC)";
      else if (addr == 0x57) device = " (AT24C32 EEPROM)";

      snprintf(buffer, sizeof(buffer), "✓ Device found at 0x%02X%s", addr, device.c_str());
      webLog(buffer);
      devicesFound++;
    }
  }

  webLog("");
  if (devicesFound == 0) {
    webLog("❌ No I2C devices found!");
    webLog("   Check your wiring and power supply.");
  } else {
    webLog("Total devices found: " + String(devicesFound));
  }
  printSeparator();
}

void fullSequenceTest() {
  webLog("");
  webLog("╔════════════════════════════════════════════╗");
  webLog("║       FULL SEQUENCE TEST STARTING          ║");
  webLog("╚════════════════════════════════════════════╝");
  webLog("");

  // Test RGB LED
  webLog("1/7 Testing RGB LED...");

  // Red
  pixels.setPixelColor(0, pixels.Color(255, 0, 0));
  pixels.show();
  webLog("    RED");
  delay(500);

  // Green
  pixels.setPixelColor(0, pixels.Color(0, 255, 0));
  pixels.show();
  webLog("    GREEN");
  delay(500);

  // Blue
  pixels.setPixelColor(0, pixels.Color(0, 0, 255));
  pixels.show();
  webLog("    BLUE");
  delay(500);

  // Off
  pixels.clear();
  pixels.show();
  webLog("    ✓ RGB LED test complete");
  delay(1000);

  // Test Pump
  webLog("");
  webLog("2/7 Testing Pump...");
  digitalWrite(PUMP_PIN, HIGH);
  webLog("    Pump ON for 3 seconds");
  delay(3000);
  digitalWrite(PUMP_PIN, LOW);
  webLog("    ✓ Pump test complete");
  delay(1000);

  // Test each valve individually
  webLog("");
  webLog("3/7 Testing Valves (one by one)...");
  for (int i = 0; i < NUM_VALVES; i++) {
    webLog("    Testing Valve " + String(i + 1) + " (GPIO " + String(VALVE_PINS[i]) + ")...");
    digitalWrite(VALVE_PINS[i], HIGH);
    delay(2000);
    digitalWrite(VALVE_PINS[i], LOW);
    webLog("    ✓ Valve " + String(i + 1) + " complete");
    delay(500);
  }

  // Test rain sensors
  webLog("");
  webLog("4/7 Testing Rain Sensors...");
  readRainSensors();

  // Test water level sensor
  webLog("");
  webLog("5/8 Testing Water Level Sensor...");
  readWaterLevelSensor();

  // Test master overflow sensor
  webLog("");
  webLog("6/8 Testing Master Overflow Sensor...");
  readMasterOverflowSensor();

  // Test DS3231 RTC
  webLog("");
  webLog("7/8 Testing DS3231 RTC...");
  readDS3231Time();

  // Test Battery Voltage
  webLog("");
  webLog("8/8 Testing DS3231 Battery Voltage...");
  readBatteryVoltage();

  webLog("");
  webLog("╔════════════════════════════════════════════╗");
  webLog("║       FULL SEQUENCE TEST COMPLETE          ║");
  webLog("╚════════════════════════════════════════════╝");
  webLog("");
  printSeparator();
}

void emergencyStop() {
  webLog("");
  webLog("⚠️ EMERGENCY STOP - TURNING EVERYTHING OFF ⚠️");
  digitalWrite(PUMP_PIN, LOW);
  pixels.clear();
  pixels.show();
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], LOW);
  }
  webLog("✓ All outputs disabled");
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
  serveFile("/web/test/firmware.html", "text/html");
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
  serveFile("/web/test/index.html", "text/html");
}

void handleDeviceInfo() {
  String json = "{";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"heap\":" + String(ESP.getFreeHeap() / 1024);
  json += "}";
  otaServer.send(200, "application/json", json);
}

void handleDashboard() {
  serveFile("/web/test/dashboard.html", "text/html");
}

void setupOTA() {
  otaServer.on("/", HTTP_GET, handleRoot);
  otaServer.on("/dashboard", HTTP_GET, handleDashboard);
  otaServer.on("/firmware", HTTP_GET, handleOTAPage);
  otaServer.on("/api/info", HTTP_GET, handleDeviceInfo);
  otaServer.on("/update", HTTP_POST, handleOTAUpdateComplete, handleOTAUpdate);
  otaServer.begin();
  Serial.println("✓ OTA Web Server started");
}

void loop() {
  // Handle OTA requests and WebSocket (if WiFi connected)
  if (WiFi.status() == WL_CONNECTED) {
    otaServer.handleClient();
    webSocket.loop();
  }

  // Handle monitoring modes
  if (monitorMode) {
    monitorRainSensors();
  }
  if (waterLevelMonitorMode) {
    monitorWaterLevelSensor();
  }
  if (overflowMonitorMode) {
    monitorMasterOverflowSensor();
  }

  // Process command from WebSocket or Serial
  char cmd = '\0';
  String fullCommand = "";

  // Check for WebSocket command first
  if (pendingCommand.length() > 0) {
    fullCommand = pendingCommand;
    cmd = pendingCommand.charAt(0);
    pendingCommand = "";
  }
  // Then check for serial input
  else if (Serial.available() > 0) {
    cmd = Serial.read();
    fullCommand = String(cmd);

    // Check if there's a number following (for R1-R6, M1-M6 commands)
    delay(10);  // Small delay to let full command arrive
    if (Serial.available() > 0) {
      char next = Serial.peek();
      if (next >= '1' && next <= '6') {
        fullCommand += String((char)Serial.read());
      }
    }

    // Clear any remaining characters in buffer
    while (Serial.available() > 0) {
      Serial.read();
    }
  }

  // Process command if we have one
  if (fullCommand.length() > 0) {
    webLog("\nCommand: " + fullCommand);
    webLog("");

    // Handle multi-character commands (R1-R6, M1-M6)
    if (fullCommand.length() == 2) {
      char firstChar = fullCommand.charAt(0);
      char secondChar = fullCommand.charAt(1);

      // Check for R1-R6 (read specific sensor)
      if ((firstChar == 'R' || firstChar == 'r') && secondChar >= '1' && secondChar <= '6') {
        int sensorIndex = secondChar - '1';  // Convert '1'-'6' to 0-5
        readSpecificRainSensor(sensorIndex);
        return;  // Skip the switch statement
      }

      // Check for M1-M6 (monitor specific sensor)
      if ((firstChar == 'M' || firstChar == 'm') && secondChar >= '1' && secondChar <= '6') {
        int sensorIndex = secondChar - '1';  // Convert '1'-'6' to 0-5
        monitorMode = true;
        monitorSensorIndex = sensorIndex;
        // Power on specific sensor
        digitalWrite(VALVE_PINS[sensorIndex], HIGH);
        digitalWrite(RAIN_SENSOR_POWER_PIN, HIGH);
        delay(100);  // Wait for stabilization
        webLog("→ Sensor " + String(sensorIndex + 1) + " monitoring ENABLED");
        webLog("  (Valve GPIO " + String(VALVE_PINS[sensorIndex]) + " + GPIO 18 powered ON)");
        webLog("  (Press 'S' to stop)");
        printSeparator();
        return;  // Skip the switch statement
      }
    }

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
        monitorSensorIndex = -1;  // All sensors
        // Power on all sensors (keep them on during monitoring)
        for (int i = 0; i < NUM_VALVES; i++) {
          digitalWrite(VALVE_PINS[i], HIGH);
        }
        digitalWrite(RAIN_SENSOR_POWER_PIN, HIGH);
        delay(100);  // Wait for stabilization
        webLog("→ ALL rain sensors monitoring ENABLED");
        webLog("  (All valve pins + GPIO 18 powered ON)");
        webLog("  (Press 'S' to stop)");
        printSeparator();
        break;

      case 'S':
      case 's':
        monitorMode = false;
        monitorSensorIndex = -1;
        waterLevelMonitorMode = false;
        overflowMonitorMode = false;
        // Power off all sensors
        digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
        for (int i = 0; i < NUM_VALVES; i++) {
          digitalWrite(VALVE_PINS[i], LOW);
        }
        webLog("→ All monitoring STOPPED");
        webLog("  (All power turned OFF)");
        printSeparator();
        break;

      case 'W':
      case 'w':
        readWaterLevelSensor();
        break;

      case 'N':
      case 'n':
        waterLevelMonitorMode = true;
        webLog("→ Water level sensor monitoring ENABLED");
        webLog("  (Press 'S' to stop)");
        printSeparator();
        break;

      case 'O':
      case 'o':
        readMasterOverflowSensor();
        break;

      case 'D':
      case 'd':
        readMasterOverflowSensorDebounced();
        break;

      case 'V':
      case 'v':
        overflowMonitorMode = true;
        webLog("→ Master overflow sensor monitoring ENABLED");
        webLog("  (Press 'S' to stop)");
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

      case 'U':
      case 'u':
        setRTCFromBrowser();
        break;

      case 'K':
      case 'k':
        resetRTCToEpoch();
        break;

      case 'B':
      case 'b':
        readBatteryVoltage();
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
        webLog("Unknown command. Press 'H' for menu.");
        printSeparator();
        break;
    }
  }
  
  delay(10);
}
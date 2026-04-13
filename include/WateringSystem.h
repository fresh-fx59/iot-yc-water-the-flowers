#ifndef WATERING_SYSTEM_H
#define WATERING_SYSTEM_H

#include "DebugHelper.h"
#include "TelegramNotifier.h"
#include "PlantLightController.h"
#include "ValveController.h"
#include "config.h"
#include "DS3231RTC.h"
#include "LearningAlgorithm.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

// NeoPixel LED (1 pixel on GPIO 48)
Adafruit_NeoPixel statusLED(1, LED_PIN, NEO_GRB + NEO_KHZ800);

// Learning data file paths (simplified two-file system)
// To reset learning data: swap the filenames below, old file auto-deletes on
// boot
const char *LEARNING_DATA_FILE =
    "/learning_data_v1.19.12.json"; // ACTIVE: Current learning data (v1.19.12 reset - fixed interval growth bug)
const char *LEARNING_DATA_FILE_OLD =
    "/learning_data_v1.19.2.json"; // OLD: Will be deleted on boot (previous version)

// ============================================
// Session Tracking Struct (for Telegram notifications)
// ============================================
struct WateringSessionData {
  int trayNumber;          // 1-6 (display format)
  unsigned long startTime; // millis() when valve opened
  unsigned long endTime;   // millis() when completed
  float duration;          // duration in seconds
  String status;           // "OK", "TIMEOUT", "ALREADY_WET", "MANUAL_STOP"
  bool active;             // is this session being tracked?

  WateringSessionData()
      : trayNumber(0), startTime(0), endTime(0), duration(0.0), status(""),
        active(false) {}
};

// ============================================
// WateringSystem Class
// ============================================
class WateringSystem {
private:
  // ========== State Variables ==========
  PumpState pumpState;
  ValveController *valves[NUM_VALVES];
  int activeValveCount;
  unsigned long lastStatePublish;
  String lastStateJson;

  // Sequential watering state
  bool sequentialMode;
  int currentSequenceIndex;
  int sequenceValves[NUM_VALVES];
  int sequenceLength;

  // Telegram session tracking
  bool telegramSessionActive;
  String sessionTriggerType;
  WateringSessionData sessionData[NUM_VALVES];

  // Auto-watering tracking
  int autoWateringValveIndex; // Currently auto-watering valve (-1 if none)

  // Halt mode (for emergency firmware updates)
  bool haltMode; // If true, block all watering operations

  // Master overflow sensor tracking
  bool overflowDetected; // If true, water overflow detected - block all watering
  unsigned long lastOverflowCheck; // Last time overflow sensor was checked
  unsigned long lastOverflowResetTime; // When overflow was last reset (for learning algorithm)
  int overflowDetectionStreak; // Consecutive debounced detections before emergency latch

  // Water level sensor tracking
  bool waterLevelLow; // If true, water tank is empty - block all watering
  unsigned long lastWaterLevelCheck; // Last time water level sensor was checked
  bool waterLevelLowNotificationSent; // Track if low water notification was sent
  unsigned long waterLevelLowFirstDetectedTime; // When LOW was first detected (for 10s delay)
  bool waterLevelLowWaitingLogged; // Track if we've logged the waiting message (prevent spam)
  PlantLightController plantLight;
  unsigned long lastPlantLightScheduleCheck;

  // Thread-safe Telegram notification queue (Core 1 queues, Core 0 sends)
  // Uses FreeRTOS queue for safe cross-core access without manual synchronization.
  static const int NOTIFICATION_QUEUE_SIZE = 16;
  QueueHandle_t notificationQueue;

public:
  // ========== Constructor ==========
  WateringSystem()
      : pumpState(PUMP_OFF), activeValveCount(0), lastStatePublish(0),
        lastStateJson(""), sequentialMode(false), currentSequenceIndex(0),
        sequenceLength(0), telegramSessionActive(false), sessionTriggerType(""),
        autoWateringValveIndex(-1), haltMode(false), overflowDetected(false),
        lastOverflowCheck(0), lastOverflowResetTime(0), overflowDetectionStreak(0),
        waterLevelLow(false),
        lastWaterLevelCheck(0), waterLevelLowNotificationSent(false),
        waterLevelLowFirstDetectedTime(0), waterLevelLowWaitingLogged(false),
        lastPlantLightScheduleCheck(0),
        notificationQueue(nullptr) {
    for (int i = 0; i < NUM_VALVES; i++) {
      valves[i] = new ValveController(i);
      sequenceValves[i] = 0;
    }
  }

  // ========== Destructor ==========
  ~WateringSystem() {
    for (int i = 0; i < NUM_VALVES; i++) {
      delete valves[i];
    }
  }

  // ========== Public Interface ==========

  void init();
  void processWateringLoop();
  String getLastState() { return lastStateJson; }

  // Watering control
  void startWatering(int valveIndex, bool forceWatering = false);
  void stopWatering(int valveIndex);
  void startSequentialWatering(const String &triggerType = "Manual");
  void startSequentialWateringCustom(int *valveIndices, int count,
                                     const String &triggerType = "");
  void stopSequentialWatering();

  // Time-based learning algorithm
  void resetCalibration(int valveIndex);
  void resetAllCalibrations();
  void printLearningStatus();
  void setAutoWatering(int valveIndex, bool enabled);
  void setAllAutoWatering(bool enabled);

  // Persistence
  bool saveLearningData();
  bool loadLearningData();

  // State management
  void publishCurrentState();
  void queueTelegramNotification(const String& message);  // Queue notification from Core 1 (non-blocking)
  void processPendingNotifications();  // Called from Core 0 (networkTask) to send queued Telegram messages
  void clearTimeoutFlag(int valveIndex);

  // Telegram session tracking
  void startTelegramSession(const String &triggerType);
  void recordSessionStart(int valveIndex);
  void recordSessionEnd(int valveIndex, const String &status);
  void endTelegramSession();

  // Watering schedule notification
  void sendWateringSchedule(const String &title);

  // Boot watering decision helpers
  bool isFirstBoot(); // Check if device has no calibration data (needs initial
                      // watering)
  bool
  hasOverdueValves(); // Check if any valve's next watering time is in the past
  int getOverdueValveIndices(int *valveIndices, int maxCount);

  // Sensor diagnostics
  void testSensor(int valveIndex);  // Test a specific sensor and log results
  void testAllSensors();  // Test all sensors and report
  int getMasterOverflowRawReading();
  int getMasterOverflowLowReadings();
  int getOverflowDetectionStreak() { return overflowDetectionStreak; }
  String getOverflowStatusMessage();

  // Halt mode control (for emergency firmware updates)
  void setHaltMode(bool enabled);
  bool isHaltMode() { return haltMode; }

  // Master overflow sensor control
  bool isOverflowDetected() { return overflowDetected; }
  void resetOverflowFlag(); // Reset overflow flag after fixing issue

  // GPIO hardware reinitialization (for stuck relay recovery)
  void reinitializeGPIOHardware(); // Force GPIO reinitialization to unstick relays

  // Water level sensor control
  bool isWaterLevelLow() { return waterLevelLow; }
  void checkWaterLevel(); // Manual water level check (for testing)
  bool setPlantLightManualOn();
  bool setPlantLightManualOff();
  bool setPlantLightAuto();
  String getPlantLightStatusMessage();

  // Metrics accessors (for MetricsPusher)
  PumpState getPumpState() { return pumpState; }
  ValveController* getValve(int i) { return (i >= 0 && i < NUM_VALVES) ? valves[i] : nullptr; }
  bool isPlantLightOn() { return plantLight.isOn(); }

private:
  // ========== Core Logic ==========
  void processValve(int valveIndex, unsigned long currentTime);
  bool isValveComplete(int valveIndex);
  void startNextInSequence();
  void checkAutoWatering(unsigned long currentTime);
  void globalSafetyWatchdog(unsigned long currentTime);  // EMERGENCY SAFETY CHECK
  void checkMasterOverflowSensor(unsigned long currentTime);  // Master overflow sensor check
  void checkWaterLevelSensor(unsigned long currentTime);  // Water level sensor check
  void updatePlantLightSchedule(unsigned long currentTime);
  void emergencyStopAll(const String &reason);  // Emergency stop all watering

  // ========== Hardware Control ==========
  bool readRainSensor(int valveIndex);
  void openValve(int valveIndex);
  void closeValve(int valveIndex);
  void updatePumpState();

  // ========== Time-Based Learning Algorithm ==========
  void processLearningData(ValveController *valve, unsigned long currentTime);
  void logLearningData(ValveController *valve, float waterLevelBefore,
                       unsigned long emptyDuration);
  void sendScheduleUpdateIfNeeded(); // Helper: sends schedule update unless in
                                     // sequential mode

  // ========== Utilities ==========
  void publishStateChange(const String &component, const String &state);
};

// ============================================
// Implementation
// ============================================

// ========== Initialization ==========
inline void WateringSystem::init() {
  // Create FreeRTOS queue for thread-safe Telegram notifications between cores.
  // Stores String* pointers; actual Strings are heap-allocated by producer (Core 1)
  // and freed by consumer (Core 0) after successful send.
  notificationQueue = xQueueCreate(NOTIFICATION_QUEUE_SIZE, sizeof(String*));

  // Initialize hardware pins
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(RAIN_SENSOR_POWER_PIN, OUTPUT);

  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);

  // Initialize NeoPixel LED
  statusLED.begin();
  statusLED.clear();
  statusLED.show();  // Initialize to OFF

  // Initialize valve pins
  String valvePinsInfo = "Valve GPIOs: ";
  for (int i = 0; i < NUM_VALVES; i++) {
    valvePinsInfo += String(i) + "→" + String(VALVE_PINS[i]);
    if (i < NUM_VALVES - 1)
      valvePinsInfo += ", ";
    pinMode(VALVE_PINS[i], OUTPUT);
    digitalWrite(VALVE_PINS[i], LOW);
  }
  DebugHelper::debugImportant(valvePinsInfo);

  // Initialize rain sensor pins with internal pull-up
  for (int i = 0; i < NUM_VALVES; i++) {
    pinMode(RAIN_SENSOR_PINS[i], INPUT_PULLUP);
  }

  // Initialize master overflow sensor pin
  pinMode(MASTER_OVERFLOW_SENSOR_PIN, INPUT_PULLUP);
  DebugHelper::debugImportant("Master overflow sensor: GPIO " + String(MASTER_OVERFLOW_SENSOR_PIN));

  // Initialize water level sensor pin
  pinMode(WATER_LEVEL_SENSOR_PIN, INPUT_PULLUP);
  DebugHelper::debugImportant("Water level sensor: GPIO " + String(WATER_LEVEL_SENSOR_PIN));

  plantLight.init();
  time_t now;
  time(&now);
  plantLight.syncAutoStateSilently(now);
  DebugHelper::debugImportant("Plant light relay: GPIO " + String(PLANT_LIGHT_RELAY_PIN) +
                              " (auto 22:00-07:00)");

  DebugHelper::debugImportant("✓ WateringSystem initialized");
  publishStateChange("system", "initialized");

  // Note: loadLearningData() is called from main.cpp after DS3231 RTC init
  // This ensures real time is available for proper timestamp conversion
}

// ========== GPIO Hardware Reinitialization ==========
// Force GPIO reinitialization to unstick relay modules
// Call this after events that may leave relays in stuck state:
// - Overflow sensor recovery
// - Water level sensor recovery
// - WiFi reconnection after long disconnection
// - Any situation where physical power cycle would normally be needed
inline void WateringSystem::reinitializeGPIOHardware() {
  DebugHelper::debugImportant("🔧 Reinitializing GPIO hardware to unstick relay modules...");

  // Reinitialize pump pin
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  // Reinitialize sensor power pin
  pinMode(RAIN_SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);

  // Reinitialize all valve pins
  for (int i = 0; i < NUM_VALVES; i++) {
    pinMode(VALVE_PINS[i], OUTPUT);
    digitalWrite(VALVE_PINS[i], LOW);
  }

  // Reinitialize plant light relay without changing logical mode/state.
  pinMode(PLANT_LIGHT_RELAY_PIN, OUTPUT);
  digitalWrite(PLANT_LIGHT_RELAY_PIN,
               plantLight.isOn() == PLANT_LIGHT_ACTIVE_HIGH ? HIGH : LOW);

  // Small delay to allow GPIO hardware to stabilize
  delay(100);

  DebugHelper::debugImportant("✓ GPIO hardware reinitialized - all pins set to OUTPUT/LOW");
  DebugHelper::debugImportant("✓ Relay modules should now be in known good state");
}

// ========== Persistence Functions ==========
inline bool WateringSystem::saveLearningData() {
  DebugHelper::debug("💾 Saving learning data to flash...");

  // Create JSON document
  StaticJsonDocument<2048> doc;
  JsonArray valvesArray = doc.createNestedArray("valves");
  unsigned long currentMillis = millis();

  for (int i = 0; i < NUM_VALVES; i++) {
    ValveController *valve = valves[i];
    JsonObject valveObj = valvesArray.createNestedObject();

    valveObj["index"] = valve->valveIndex;
    valveObj["lastWateringCompleteTime"] =
        (unsigned long)valve->lastWateringCompleteTime;
    valveObj["lastWateringAttemptTime"] =
        (unsigned long)valve->lastWateringAttemptTime;
    valveObj["lastWateringCompleteAgeMs"] =
        hasLastWateringReference(valve)
            ? getTimeSinceLastWatering(valve, currentMillis)
            : 0;
    valveObj["lastWateringAttemptAgeMs"] =
        hasLastWateringAttemptReference(valve)
            ? getTimeSinceLastWateringAttempt(valve, currentMillis)
            : 0;
    valveObj["emptyToFullDuration"] = (unsigned long)valve->emptyToFullDuration;
    valveObj["baselineFillDuration"] =
        (unsigned long)valve->baselineFillDuration;
    valveObj["lastFillDuration"] = (unsigned long)valve->lastFillDuration;
    valveObj["previousFillDuration"] =
        (unsigned long)valve->previousFillDuration;
    valveObj["lastWaterLevelPercent"] = valve->lastWaterLevelPercent;
    valveObj["isCalibrated"] = valve->isCalibrated;
    valveObj["totalWateringCycles"] = valve->totalWateringCycles;
    valveObj["autoWateringEnabled"] = valve->autoWateringEnabled;
    valveObj["intervalMultiplier"] = valve->intervalMultiplier;
  }

  // Save current millis() and DS3231 RTC time as reference points
  doc["savedAtMillis"] = currentMillis;
  time_t now;
  time(&now);
  doc["savedAtRealTime"] = (unsigned long)now;

  // Open file for writing
  File file = LittleFS.open(LEARNING_DATA_FILE, "w");
  if (!file) {
    DebugHelper::debugImportant("❌ Failed to open file for writing");
    return false;
  }

  // Serialize JSON to file
  if (serializeJson(doc, file) == 0) {
    DebugHelper::debugImportant("❌ Failed to write JSON to file");
    file.close();
    return false;
  }

  file.close();
  DebugHelper::debug("✓ Learning data saved successfully");
  return true;
}

inline bool WateringSystem::loadLearningData() {
  DebugHelper::debug("📂 Loading learning data from flash...");

  // Check if file exists
  if (!LittleFS.exists(LEARNING_DATA_FILE)) {
    DebugHelper::debug("  No learning data file found");
    return false;
  }

  // Open file for reading
  File file = LittleFS.open(LEARNING_DATA_FILE, "r");
  if (!file) {
    DebugHelper::debugImportant("❌ Failed to open file for reading");
    return false;
  }

  // Parse JSON
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    DebugHelper::debugImportant("❌ Failed to parse JSON: " +
                                String(error.c_str()));
    return false;
  }

  // Get saved timestamps
  unsigned long savedAtMillis = doc["savedAtMillis"] | 0;
  unsigned long savedAtRealTime = doc["savedAtRealTime"] | 0;
  unsigned long currentMillis = millis();
  time_t currentRealTime;
  time(&currentRealTime);

  // Calculate time offset using DS3231 RTC time (always available)
  unsigned long timeOffsetMs = 0;

  if (savedAtRealTime > 0 && currentRealTime > 1000000000) {
    // Use real time - this handles reboots correctly
    if (currentRealTime >= savedAtRealTime) {
      unsigned long elapsedSeconds = currentRealTime - savedAtRealTime;
      timeOffsetMs = elapsedSeconds * 1000UL;
      DebugHelper::debug("  Using real time for offset calculation");
      DebugHelper::debug("  Time since save: " +
                         LearningAlgorithm::formatDuration(timeOffsetMs));
    } else {
      // Clock went backwards - use current millis as base
      DebugHelper::debugImportant(
          "⚠️  Clock went backwards - resetting to current time");
      timeOffsetMs = currentMillis;
    }
  } else if (currentMillis >= savedAtMillis) {
    // Fall back to millis() if no real time available
    timeOffsetMs = currentMillis - savedAtMillis;
    DebugHelper::debug("  Using millis() for offset (no real time available)");
  } else {
    // Reboot detected without real time - use current millis as base
    DebugHelper::debugImportant(
        "⚠️  Reboot detected without real time - using current millis");
    timeOffsetMs = currentMillis;
  }

  // Load valve data
  JsonArray valvesArray = doc["valves"];
  int loadedCount = 0;

  for (JsonObject valveObj : valvesArray) {
    int index = valveObj["index"] | -1;
    if (index < 0 || index >= NUM_VALVES)
      continue;

    ValveController *valve = valves[index];
    unsigned long savedCompleteTime = valveObj["lastWateringCompleteTime"] | 0;
    unsigned long savedAttemptTime = valveObj["lastWateringAttemptTime"] | 0;
    unsigned long savedCompleteAgeMs = valveObj["lastWateringCompleteAgeMs"] | 0;
    unsigned long savedAttemptAgeMs = valveObj["lastWateringAttemptAgeMs"] | 0;

    valve->emptyToFullDuration = valveObj["emptyToFullDuration"] | 0;
    valve->baselineFillDuration = valveObj["baselineFillDuration"] | 0;
    valve->lastFillDuration = valveObj["lastFillDuration"] | 0;
    valve->previousFillDuration = valveObj["previousFillDuration"] | 0;
    valve->lastWaterLevelPercent = valveObj["lastWaterLevelPercent"] | 0.0;
    valve->isCalibrated = valveObj["isCalibrated"] | false;
    valve->totalWateringCycles = valveObj["totalWateringCycles"] | 0;
    valve->autoWateringEnabled = valveObj["autoWateringEnabled"] | true;
    valve->intervalMultiplier = valveObj["intervalMultiplier"] | 1.0;
    valve->lastWateringCompleteTime = 0;
    valve->lastWateringAttemptTime = 0;
    valve->realTimeSinceLastWatering = 0;
    valve->realTimeSinceLastWateringAttempt = 0;

    // Convert saved timestamps to current millis() epoch
    // New timestamp = current_millis - (time_elapsed_since_save -
    // time_from_save_to_watering) Simplified: New timestamp = current_millis -
    // time_since_watering
    unsigned long timeSinceWatering = 0;
    bool hasSavedWateringReference = false;
    if (savedCompleteAgeMs > 0) {
      timeSinceWatering = savedCompleteAgeMs + timeOffsetMs;
      hasSavedWateringReference = true;
    } else if (savedCompleteTime > 0 && savedAtMillis > 0) {
      // Backward-compatible path for older files that only stored millis()
      // timestamps.
      unsigned long timeFromWateringToSave = savedAtMillis - savedCompleteTime;
      timeSinceWatering = timeFromWateringToSave + timeOffsetMs;
      hasSavedWateringReference = true;
    }

    if (hasSavedWateringReference) {
      // New timestamp in current epoch
      if (currentMillis >= timeSinceWatering) {
        valve->lastWateringCompleteTime = currentMillis - timeSinceWatering;
        valve->realTimeSinceLastWatering = 0; // Can use normal timestamp
      } else {
        // Watering was longer ago than millis can represent
        // Store the real duration for overdue detection
        valve->lastWateringCompleteTime = 0;
        valve->realTimeSinceLastWatering = timeSinceWatering;
      }
    }

    unsigned long timeSinceAttempt = 0;
    bool hasSavedAttemptReference = false;
    if (savedAttemptAgeMs > 0) {
      timeSinceAttempt = savedAttemptAgeMs + timeOffsetMs;
      hasSavedAttemptReference = true;
    } else if (savedAttemptTime > 0 && savedAtMillis > 0) {
      unsigned long timeFromAttemptToSave = savedAtMillis - savedAttemptTime;
      timeSinceAttempt = timeFromAttemptToSave + timeOffsetMs;
      hasSavedAttemptReference = true;
    }

    if (hasSavedAttemptReference) {
      if (currentMillis >= timeSinceAttempt) {
        valve->lastWateringAttemptTime = currentMillis - timeSinceAttempt;
        valve->realTimeSinceLastWateringAttempt = 0;
      } else {
        valve->lastWateringAttemptTime = 0;
        valve->realTimeSinceLastWateringAttempt = timeSinceAttempt;
      }
    }

    loadedCount++;
  }

  DebugHelper::debug("✓ Loaded data for " + String(loadedCount) + " valves");
  DebugHelper::debug("  Time offset applied: " +
                     LearningAlgorithm::formatDuration(timeOffsetMs));

  return true;
}

// ========== Main Processing Loop ==========
inline void WateringSystem::processWateringLoop() {
  unsigned long currentTime = millis();

  // 🚨 MASTER OVERFLOW SENSOR - HIGHEST PRIORITY CHECK
  checkMasterOverflowSensor(currentTime);

  // 🚨 WATER LEVEL SENSOR - CHECK TANK WATER LEVEL
  checkWaterLevelSensor(currentTime);

  // 🚨 GLOBAL SAFETY WATCHDOG - ALWAYS RUN FIRST
  globalSafetyWatchdog(currentTime);

  // Plant light schedule runs independently of watering safety logic.
  updatePlantLightSchedule(currentTime);

  // Check for automatic watering (time-based)
  if (!sequentialMode) {
    checkAutoWatering(currentTime);
  }

  // Process each valve independently
  for (int i = 0; i < NUM_VALVES; i++) {
    processValve(i, currentTime);
  }

  // Handle sequential watering
  if (sequentialMode && currentSequenceIndex > 0) {
    int lastValveIndex = sequenceValves[currentSequenceIndex - 1];
    if (isValveComplete(lastValveIndex)) {
      delay(1000); // Small delay between valves
      startNextInSequence();
    }
  }

  // Publish state periodically
  if (currentTime - lastStatePublish >= STATE_PUBLISH_INTERVAL) {
    publishCurrentState();
    lastStatePublish = currentTime;
  }
}

// ========== GLOBAL SAFETY WATCHDOG ==========
// This runs INDEPENDENTLY of the state machine to catch any failures
inline void WateringSystem::globalSafetyWatchdog(unsigned long currentTime) {
  for (int i = 0; i < NUM_VALVES; i++) {
    ValveController* valve = valves[i];

    // Check if any valve has been watering too long
    if (valve->phase == PHASE_WATERING && valve->wateringStartTime > 0) {
      unsigned long wateringDuration = currentTime - valve->wateringStartTime;

      // CRITICAL: If exceeded absolute timeout, FORCE STOP EVERYTHING
      if (wateringDuration >= getValveEmergencyTimeout(i)) {
        DebugHelper::debugImportant("🚨🚨🚨 GLOBAL SAFETY WATCHDOG TRIGGERED! 🚨🚨🚨");
        DebugHelper::debugImportant("Valve " + String(i) + " exceeded " + String(getValveEmergencyTimeout(i) / 1000) + "s!");
        DebugHelper::debugImportant("Duration: " + String(wateringDuration / 1000) + "s");
        DebugHelper::debugImportant("FORCING EMERGENCY SHUTDOWN!");
        #ifdef METRICS_PUSHER_H
        MetricsPusher::logError("Safety watchdog: valve " + String(i) + " exceeded " + String(getValveEmergencyTimeout(i) / 1000) + "s, forcing shutdown");
        #endif

        // FORCE DIRECT GPIO CONTROL - BYPASS ALL STATE MACHINES
        digitalWrite(VALVE_PINS[i], LOW);
        valve->state = VALVE_CLOSED;

        // Force pump off if no other valves active
        bool anyWatering = false;
        for (int j = 0; j < NUM_VALVES; j++) {
          if (j != i && valves[j]->phase == PHASE_WATERING) {
            anyWatering = true;
          }
        }
        if (!anyWatering) {
          digitalWrite(PUMP_PIN, LOW);
          pumpState = PUMP_OFF;
          statusLED.clear();
          statusLED.show();
        }

        // Mark as timeout and move to cleanup
        valve->timeoutOccurred = true;
        valve->phase = PHASE_CLOSING_VALVE;

        DebugHelper::debugImportant("Emergency shutdown complete for valve " + String(i));
      }
    }
  }
}

// ========== MASTER OVERFLOW SENSOR WATCHDOG ==========
// Monitors master overflow sensor and triggers emergency stop if water overflow detected
inline void WateringSystem::checkMasterOverflowSensor(unsigned long currentTime) {
  // Check sensor every 100ms to ensure fast response
  if (currentTime - lastOverflowCheck < 100) {
    return;
  }
  lastOverflowCheck = currentTime;

  // Software debouncing: Take multiple readings to filter out electrical noise
  // This prevents false triggers from pump/valve switching and EMI
  int lowReadings = 0;

  for (int i = 0; i < OVERFLOW_DEBOUNCE_SAMPLES; i++) {
    int reading = digitalRead(MASTER_OVERFLOW_SENSOR_PIN);
    if (reading == LOW) {
      lowReadings++;
    }

    // Small delay between readings to avoid catching the same noise spike
    if (i < OVERFLOW_DEBOUNCE_SAMPLES - 1) {
      delay(OVERFLOW_DEBOUNCE_DELAY_MS);
    }
  }

  bool debouncedDetected = (lowReadings >= OVERFLOW_DEBOUNCE_THRESHOLD);

  // Require multiple consecutive debounced detections to avoid latching on a
  // single noisy burst. This makes the line effectively need to stay bad for
  // several 100ms checks rather than one 35ms sampling window.
  if (debouncedDetected) {
    if (overflowDetectionStreak < OVERFLOW_CONFIRMATION_CHECKS) {
      overflowDetectionStreak++;
    }

    if (!overflowDetected &&
        overflowDetectionStreak >= OVERFLOW_CONFIRMATION_CHECKS) {
      // Confirmed sustained detection - trigger emergency stop
      DebugHelper::debugImportant("🚨🚨🚨 MASTER OVERFLOW SENSOR TRIGGERED! 🚨🚨🚨");
      DebugHelper::debugImportant("Water overflow detected on GPIO " + String(MASTER_OVERFLOW_SENSOR_PIN) +
                                  " (" + String(lowReadings) + "/" + String(OVERFLOW_DEBOUNCE_SAMPLES) +
                                  " LOW readings, streak " + String(overflowDetectionStreak) +
                                  "/" + String(OVERFLOW_CONFIRMATION_CHECKS) + ")");
      #ifdef METRICS_PUSHER_H
      MetricsPusher::logError("Overflow detected! GPIO " + String(MASTER_OVERFLOW_SENSOR_PIN) + " streak=" + String(overflowDetectionStreak));
      #endif
      overflowDetected = true;

      // Emergency stop everything
      emergencyStopAll("OVERFLOW DETECTED");

      // Queue Telegram notification (non-blocking, sent from Core 0)
      String message = "🚨🚨🚨 <b>WATER OVERFLOW DETECTED</b> 🚨🚨🚨\n\n";
      message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
      message += "🔧 Master overflow sensor triggered\n";
      message += "💧 Water is overflowing from tray!\n\n";
      message += "✅ Emergency actions taken:\n";
      message += "  • All valves CLOSED\n";
      message += "  • Pump STOPPED\n";
      message += "  • System LOCKED\n\n";
      message += "⚠️  Manual intervention required!\n";
      message += "Send /reset_overflow to resume operations";

      queueTelegramNotification(message);
      DebugHelper::debugImportant("📱 Overflow notification queued for Telegram");
    }
  } else {
    overflowDetectionStreak = 0;
  }
}

inline int WateringSystem::getMasterOverflowRawReading() {
  return digitalRead(MASTER_OVERFLOW_SENSOR_PIN);
}

inline int WateringSystem::getMasterOverflowLowReadings() {
  int lowReadings = 0;

  for (int i = 0; i < OVERFLOW_DEBOUNCE_SAMPLES; i++) {
    if (digitalRead(MASTER_OVERFLOW_SENSOR_PIN) == LOW) {
      lowReadings++;
    }

    if (i < OVERFLOW_DEBOUNCE_SAMPLES - 1) {
      delay(OVERFLOW_DEBOUNCE_DELAY_MS);
    }
  }

  return lowReadings;
}

inline String WateringSystem::getOverflowStatusMessage() {
  int rawReading = getMasterOverflowRawReading();
  int lowReadings = getMasterOverflowLowReadings();
  bool debouncedDetected = (lowReadings >= OVERFLOW_DEBOUNCE_THRESHOLD);

  String message = "🚨 <b>OVERFLOW SENSOR STATUS</b>\n\n";
  message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
  message += "🔌 GPIO: " + String(MASTER_OVERFLOW_SENSOR_PIN) + "\n";
  message += "🔒 Latched overflow flag: " + String(overflowDetected ? "ON" : "OFF") + "\n";
  message += "📍 Raw reading: " + String(rawReading) + " (" +
             String(rawReading == LOW ? "LOW / triggered" : "HIGH / dry") + ")\n";
  message += "🧪 Debounced reading: " + String(lowReadings) + "/" +
             String(OVERFLOW_DEBOUNCE_SAMPLES) + " LOW samples\n";
  message += "📈 Trigger streak: " + String(overflowDetectionStreak) + "/" +
             String(OVERFLOW_CONFIRMATION_CHECKS) + "\n";
  message += "🚦 Debounced result: " +
             String(debouncedDetected ? "OVERFLOW DETECTED" : "NORMAL") + "\n\n";
  message += "Threshold: " + String(OVERFLOW_DEBOUNCE_THRESHOLD) + "/" +
             String(OVERFLOW_DEBOUNCE_SAMPLES) + " LOW samples required\n";
  message += "Latch rule: " + String(OVERFLOW_CONFIRMATION_CHECKS) +
             " consecutive debounced detections required";

  return message;
}

// ========== EMERGENCY STOP ALL ==========
// Force stop all watering operations immediately
inline void WateringSystem::emergencyStopAll(const String &reason) {
  DebugHelper::debugImportant("🚨 EMERGENCY STOP: " + reason);

  // Force close all valves via direct GPIO control
  for (int i = 0; i < NUM_VALVES; i++) {
    digitalWrite(VALVE_PINS[i], LOW);
    valves[i]->state = VALVE_CLOSED;
    valves[i]->phase = PHASE_IDLE;
  }

  // Force pump off via direct GPIO control
  digitalWrite(PUMP_PIN, LOW);
  pumpState = PUMP_OFF;

  // CRITICAL: Turn off sensor power (GPIO 18) - no valves watering
  digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);

  // Turn off LED
  statusLED.clear();
  statusLED.show();

  // Stop sequential mode if active
  sequentialMode = false;

  DebugHelper::debugImportant("✓ All valves closed, pump stopped, sensors off, system halted");
}

// ========== RESET OVERFLOW FLAG ==========
inline void WateringSystem::resetOverflowFlag() {
  overflowDetected = false;
  overflowDetectionStreak = 0;
  lastOverflowResetTime = millis(); // Track when overflow was reset (for learning algorithm)

  // Reinitialize GPIO hardware to unstick relay modules
  // This is critical because relay modules can get stuck after emergency stop
  // and need GPIO reinitialization (not just flag clearing) to recover
  reinitializeGPIOHardware();

  DebugHelper::debugImportant("✓ Overflow flag reset - system ready to resume");
}

// ========== WATER LEVEL SENSOR WATCHDOG ==========
// Monitors water level sensor and blocks watering if tank is empty
inline void WateringSystem::checkWaterLevelSensor(unsigned long currentTime) {
  // Check sensor every 100ms to ensure fast response
  if (currentTime - lastWaterLevelCheck < WATER_LEVEL_CHECK_INTERVAL) {
    return;
  }
  lastWaterLevelCheck = currentTime;

  // Read water level sensor (HIGH = water detected, LOW = no water/empty)
  int sensorValue = digitalRead(WATER_LEVEL_SENSOR_PIN);

  // If water level is low (sensor reads LOW)
  if (sensorValue == LOW) {
    // Start or continue tracking LOW detection time
    if (waterLevelLowFirstDetectedTime == 0) {
      // First LOW detection - start timer
      waterLevelLowFirstDetectedTime = currentTime;
      waterLevelLowWaitingLogged = false; // Reset waiting log flag
      DebugHelper::debug("Water level LOW detected - allowing " + String(WATER_LEVEL_LOW_DELAY / 1000) + "s continuation time...");
      return; // Don't block yet, wait for delay
    }

    // Check if we've been LOW for at least WATER_LEVEL_LOW_DELAY seconds
    unsigned long lowDuration = currentTime - waterLevelLowFirstDetectedTime;
    if (lowDuration >= WATER_LEVEL_LOW_DELAY) {
      // Been LOW for full delay period - now block watering
      if (!waterLevelLow) {
        // First time blocking - trigger emergency stop and send notification
        DebugHelper::debugImportant("⚠️⚠️⚠️ WATER LEVEL LOW CONFIRMED (" + String(WATER_LEVEL_LOW_DELAY / 1000) + "s delay expired) ⚠️⚠️⚠️");
        DebugHelper::debugImportant("Water tank is empty - GPIO " + String(WATER_LEVEL_SENSOR_PIN));
        #ifdef METRICS_PUSHER_H
        MetricsPusher::logWarn("Water level low confirmed, GPIO " + String(WATER_LEVEL_SENSOR_PIN));
        #endif
        waterLevelLow = true;
        waterLevelLowNotificationSent = false;
        waterLevelLowWaitingLogged = false; // Reset for next event

        // Emergency stop everything if currently watering
        bool anyWatering = false;
        for (int i = 0; i < NUM_VALVES; i++) {
          if (valves[i]->phase != PHASE_IDLE) {
            anyWatering = true;
            break;
          }
        }
        if (anyWatering) {
          emergencyStopAll("WATER LEVEL LOW");
        }

        // Queue Telegram notification (non-blocking, sent from Core 0)
        if (!waterLevelLowNotificationSent) {
          String message = "⚠️⚠️⚠️ <b>WATER LEVEL LOW</b> ⚠️⚠️⚠️\n\n";
          message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
          message += "💧 Water tank is empty or low\n";
          message += "🔧 Sensor GPIO " + String(WATER_LEVEL_SENSOR_PIN) + "\n";
          message += "⏱️ Confirmed after " + String(WATER_LEVEL_LOW_DELAY / 1000) + "s delay\n\n";
          message += "✅ Actions taken:\n";
          if (anyWatering) {
            message += "  • All valves CLOSED\n";
            message += "  • Pump STOPPED\n";
          }
          message += "  • Watering BLOCKED\n\n";
          message += "🔄 System will resume automatically when water is refilled";

          queueTelegramNotification(message);
          waterLevelLowNotificationSent = true;
          DebugHelper::debugImportant("📱 Water level low notification queued for Telegram");
        }
      }
    } else {
      // Still within delay period - don't block yet, allow watering to continue
      if (!waterLevelLowWaitingLogged) {
        unsigned long remainingMs = WATER_LEVEL_LOW_DELAY - lowDuration;
        DebugHelper::debug("Water level LOW - allowing " + String(remainingMs / 1000) + "s continuation time (won't spam)");
        waterLevelLowWaitingLogged = true; // Only log once during the delay period
      }
    }
  } else {
    // Water level is OK (sensor reads HIGH)

    // Reset the LOW detection timer since we detected HIGH
    if (waterLevelLowFirstDetectedTime != 0) {
      waterLevelLowFirstDetectedTime = 0;
      waterLevelLowWaitingLogged = false; // Reset waiting log flag
      if (!waterLevelLow) {
        // Water came back before the delay period - no blocking needed
        DebugHelper::debug("Water level restored before delay - pipe drainage detected");
      }
    }

    if (waterLevelLow) {
      // Water was low and blocked, but now restored
      DebugHelper::debugImportant("✅ WATER LEVEL RESTORED!");
      DebugHelper::debugImportant("Water tank refilled - normal operation resumed");
      waterLevelLow = false;

      // Reinitialize GPIO hardware to unstick relay modules
      // Same issue as overflow: relay modules can get stuck after emergency stop
      reinitializeGPIOHardware();

      // Queue Telegram notification (non-blocking, sent from Core 0)
      String message = "✅ <b>WATER LEVEL RESTORED</b> ✅\n\n";
      message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
      message += "💧 Water tank refilled\n";
      message += "🔄 System resuming normal operation\n\n";
      message += "✓ Watering operations enabled";

      queueTelegramNotification(message);
      DebugHelper::debugImportant("📱 Water level restored notification queued for Telegram");

      // Reset notification flag for next low water event
      waterLevelLowNotificationSent = false;
    }
  }
}

// ========== MANUAL WATER LEVEL CHECK ==========
// For testing and diagnostics
inline void WateringSystem::checkWaterLevel() {
  int sensorValue = digitalRead(WATER_LEVEL_SENSOR_PIN);
  String status = (sensorValue == HIGH) ? "OK (Water detected)" : "LOW (No water)";
  DebugHelper::debugImportant("Water Level Sensor (GPIO " + String(WATER_LEVEL_SENSOR_PIN) + "): " + status);
  DebugHelper::debugImportant("Current state: " + String(waterLevelLow ? "BLOCKED" : "NORMAL"));
}

inline void WateringSystem::updatePlantLightSchedule(unsigned long currentTime) {
  if (currentTime - lastPlantLightScheduleCheck <
      PLANT_LIGHT_SCHEDULE_CHECK_INTERVAL_MS) {
    return;
  }
  lastPlantLightScheduleCheck = currentTime;

  time_t now;
  time(&now);
  bool changed = plantLight.applyAutomaticSchedule(now);

  if (!changed) {
    return;
  }

  String message = String(plantLight.isOn() ? "💡 <b>PLANT LIGHT ON</b>\n\n"
                                            : "🌙 <b>PLANT LIGHT OFF</b>\n\n");
  message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
  message += "🤖 Mode: automatic schedule\n";
  message += "📅 Schedule: 22:00 -> 07:00";

  queueTelegramNotification(message);
  publishStateChange("plant_light", plantLight.isOn() ? "on" : "off");
}

inline bool WateringSystem::setPlantLightManualOn() {
  bool changed = plantLight.setManualOn();
  publishStateChange("plant_light", "manual_on");
  publishCurrentState();
  return changed;
}

inline bool WateringSystem::setPlantLightManualOff() {
  bool changed = plantLight.setManualOff();
  publishStateChange("plant_light", "manual_off");
  publishCurrentState();
  return changed;
}

inline bool WateringSystem::setPlantLightAuto() {
  time_t now;
  time(&now);
  bool changed = plantLight.setAuto(now);
  publishStateChange("plant_light", "auto");
  publishCurrentState();
  return changed;
}

inline String WateringSystem::getPlantLightStatusMessage() {
  time_t now;
  time(&now);

  String message = "💡 <b>Plant Light Status</b>\n\n";
  message += "State: " + String(plantLight.isOn() ? "ON" : "OFF") + "\n";
  message += "Mode: " + String(plantLight.getModeName()) + "\n";
  message += "Relay GPIO: " + String(PLANT_LIGHT_RELAY_PIN) + "\n";
  message += "Schedule: 22:00 -> 07:00\n";
  message += "Auto wants: " +
             String(plantLight.shouldBeOnNow(now) ? "ON now" : "OFF now");

  return message;
}

// ========== Automatic Watering Check ==========
inline void WateringSystem::checkAutoWatering(unsigned long currentTime) {
  // OVERFLOW CHECK: Block all watering if overflow detected
  if (overflowDetected) {
    return;
  }

  // WATER LEVEL CHECK: Block all watering if water tank is empty
  if (waterLevelLow) {
    return;
  }

  // HALT MODE CHECK: Block auto-watering
  if (haltMode) {
    return;
  }

  // Check each valve to see if it's time to water automatically
  for (int i = 0; i < NUM_VALVES; i++) {
    ValveController *valve = valves[i];

    // Only check valves that are idle and have auto-watering enabled
    if (valve->phase != PHASE_IDLE || !valve->autoWateringEnabled) {
      continue;
    }

    // Check if tray is empty and should be watered
    if (shouldWaterNow(valve, currentTime)) {
      DebugHelper::debugImportant("⏰ AUTO-WATERING TRIGGERED: Valve " +
                                  String(i));
      DebugHelper::debug("  Tray is empty - starting automatic watering");
      #ifdef METRICS_PUSHER_H
      MetricsPusher::logInfo("Auto-watering triggered: valve " + String(i));
      #endif

      // Start Telegram session for auto-watering
      startTelegramSession("Auto (Tray " + String(i + 1) + ")");
      autoWateringValveIndex = i;

      // Queue start notification (non-blocking, sent from Core 0)
      String trayNumber = String(i + 1);
      queueTelegramNotification(TelegramNotifier::formatWateringStarted("Auto", trayNumber));

      startWatering(i);
    }
  }
}

// ========== Watering Control ==========
inline void WateringSystem::startWatering(int valveIndex, bool forceWatering) {
  // OVERFLOW CHECK: Block all watering if overflow detected
  if (overflowDetected) {
    DebugHelper::debug("🚨 Watering blocked - OVERFLOW DETECTED");
    return;
  }

  // WATER LEVEL CHECK: Block all watering if water tank is empty
  if (waterLevelLow) {
    DebugHelper::debug("💧 Watering blocked - WATER LEVEL LOW");
    return;
  }

  // HALT MODE CHECK: Block all watering operations
  if (haltMode) {
    DebugHelper::debug("🛑 Watering blocked - system in HALT MODE");
    return;
  }

  if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
    DebugHelper::debugImportant("Invalid valve index: " + String(valveIndex));
    publishStateChange("error", "invalid_valve_index");
    return;
  }

  ValveController *valve = valves[valveIndex];

  if (valve->phase != PHASE_IDLE) {
    DebugHelper::debug("Valve " + String(valveIndex) + " is already active");
    return;
  }

  unsigned long currentTime = millis();

  // Check if we should skip this watering (time-based learning algorithm)
  // ONLY skip for auto-watering, NOT for manual/Telegram/API commands
  if (!forceWatering && valve->isCalibrated && valve->emptyToFullDuration > 0 &&
      valve->lastWateringCompleteTime > 0) {
    // Safety check for future timestamps (e.g. clock drift)
    if (valve->lastWateringCompleteTime > currentTime) {
      DebugHelper::debugImportant("⚠️ FUTURE TIMESTAMP DETECTED: Valve " +
                                  String(valveIndex));
      DebugHelper::debug("  Last watering: " +
                         String(valve->lastWateringCompleteTime));
      DebugHelper::debug("  Current time:  " + String(currentTime));
      DebugHelper::debug("  Skipping watering for safety");

      publishStateChange("valve" + String(valveIndex),
                         "skipped_future_timestamp");
      return;
    }

    unsigned long timeSinceLastWatering =
        currentTime - valve->lastWateringCompleteTime;

    if (timeSinceLastWatering < valve->emptyToFullDuration) {
      // Tray not empty yet - skip watering
      float currentWaterLevel = calculateCurrentWaterLevel(valve, currentTime);
      unsigned long timeRemaining =
          valve->emptyToFullDuration - timeSinceLastWatering;

      DebugHelper::debug("═══════════════════════════════════════");
      DebugHelper::debug("🧠 SMART SKIP: Valve " + String(valveIndex));
      DebugHelper::debug("  Tray not empty yet (water level: ~" +
                         String((int)currentWaterLevel) + "%)");
      DebugHelper::debug(
          "  Time since last watering: " +
          LearningAlgorithm::formatDuration(timeSinceLastWatering));
      DebugHelper::debug("  Time until empty: " +
                         LearningAlgorithm::formatDuration(timeRemaining));
      DebugHelper::debug("═══════════════════════════════════════");

      publishStateChange("valve" + String(valveIndex),
                         "cycle_skipped_learning");
      return;
    } else {
      DebugHelper::debug("═══════════════════════════════════════");
      DebugHelper::debug("⏰ TIME TO WATER: Valve " + String(valveIndex));
      DebugHelper::debug(
          "  Tray should be empty now (time elapsed: " +
          LearningAlgorithm::formatDuration(timeSinceLastWatering) + ")");
    }
  }

  // Start watering cycle
  DebugHelper::debug("═══════════════════════════════════════");
  DebugHelper::debug("Starting watering cycle for valve " + String(valveIndex));

  if (valve->isCalibrated) {
    DebugHelper::debug(
        "🧠 Calibrated - Baseline: " +
        LearningAlgorithm::formatDuration(valve->baselineFillDuration));
    if (valve->emptyToFullDuration > 0) {
      DebugHelper::debug("  Empty time: " + LearningAlgorithm::formatDuration(
                                                valve->emptyToFullDuration));
    }
  } else {
    DebugHelper::debug("🎯 First watering - Establishing baseline");
  }

  DebugHelper::debug("Step 1: Opening valve (sensor needs water flow)...");
  valve->wateringRequested = true;
  valve->rainDetected = false;
  valve->lastRainCheck = 0;
  valve->phase = PHASE_OPENING_VALVE;

  // CRITICAL: Record watering attempt time (prevents auto-watering retry loops)
  valve->lastWateringAttemptTime = currentTime;
  valve->realTimeSinceLastWateringAttempt = 0;

  publishStateChange("valve" + String(valveIndex), "cycle_started");

  // Record session start if Telegram tracking is active
  if (telegramSessionActive) {
    recordSessionStart(valveIndex);
  }
}

inline void WateringSystem::stopWatering(int valveIndex) {
  if (valveIndex < 0 || valveIndex >= NUM_VALVES)
    return;

  ValveController *valve = valves[valveIndex];
  valve->wateringRequested = false;

  if (valve->phase == PHASE_WATERING) {
    updatePumpState();
  }

  closeValve(valveIndex);
  valve->phase = PHASE_IDLE;
  publishStateChange("valve" + String(valveIndex), "cycle_stopped");
  updatePumpState();

  // CRITICAL: Turn off sensor power (GPIO 18) if no other valves are watering
  bool anyWatering = false;
  for (int i = 0; i < NUM_VALVES; i++) {
    if (valves[i]->phase == PHASE_WATERING) {
      anyWatering = true;
      break;
    }
  }
  if (!anyWatering) {
    digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
    DebugHelper::debug("Sensor power (GPIO 18) turned OFF - no valves watering");
  }
}

inline void WateringSystem::startSequentialWatering(const String &triggerType) {
  // OVERFLOW CHECK: Block all watering if overflow detected
  if (overflowDetected) {
    DebugHelper::debug("🚨 Sequential watering blocked - OVERFLOW DETECTED");
    return;
  }

  // WATER LEVEL CHECK: Block all watering if water tank is empty
  if (waterLevelLow) {
    DebugHelper::debug("💧 Sequential watering blocked - WATER LEVEL LOW");
    return;
  }

  // HALT MODE CHECK: Block all watering operations
  if (haltMode) {
    DebugHelper::debug("🛑 Sequential watering blocked - system in HALT MODE");
    return;
  }

  if (sequentialMode) {
    DebugHelper::debug("Sequential watering already in progress");
    return;
  }

  DebugHelper::debug("\n╔═══════════════════════════════════════════╗");
  DebugHelper::debug("║  SEQUENTIAL WATERING STARTED (ALL VALVES) ║");
  DebugHelper::debug("╚═══════════════════════════════════════════╝");

  // Prepare sequence: all valves in reverse order (5-0)
  sequenceLength = NUM_VALVES;
  for (int i = 0; i < NUM_VALVES; i++) {
    sequenceValves[i] = NUM_VALVES - 1 - i;
  }

  sequentialMode = true;
  currentSequenceIndex = 0;
  publishStateChange("system", "sequential_started");

  // Start Telegram session and send start notification
  startTelegramSession(triggerType);

  // Build tray numbers list
  String trayNumbers;
  if (sequenceLength == NUM_VALVES) {
    trayNumbers = "All";
  } else {
    trayNumbers = "";
    for (int i = 0; i < sequenceLength; i++) {
      trayNumbers += String(sequenceValves[i] + 1); // Convert to 1-indexed
      if (i < sequenceLength - 1)
        trayNumbers += ", ";
    }
  }

  // Queue start notification (non-blocking, sent from Core 0)
  queueTelegramNotification(TelegramNotifier::formatWateringStarted(sessionTriggerType, trayNumbers));

  startNextInSequence();
}

inline void WateringSystem::startSequentialWateringCustom(int *valveIndices,
                                                          int count,
                                                          const String &triggerType) {
  // OVERFLOW CHECK: Block all watering if overflow detected
  if (overflowDetected) {
    DebugHelper::debug("🚨 Sequential watering blocked - OVERFLOW DETECTED");
    return;
  }

  // WATER LEVEL CHECK: Block all watering if water tank is empty
  if (waterLevelLow) {
    DebugHelper::debug("💧 Sequential watering blocked - WATER LEVEL LOW");
    return;
  }

  // HALT MODE CHECK: Block all watering operations
  if (haltMode) {
    DebugHelper::debug("🛑 Sequential watering blocked - system in HALT MODE");
    return;
  }

  if (sequentialMode) {
    DebugHelper::debug("Sequential watering already in progress");
    return;
  }

  if (count == 0 || count > NUM_VALVES) {
    DebugHelper::debug("Invalid valve count for sequential watering");
    return;
  }

  DebugHelper::debug("╔═══════════════════════════════════════════╗");
  DebugHelper::debug("║  SEQUENTIAL WATERING STARTED              ║");
  DebugHelper::debug("╚═══════════════════════════════════════════╝");
  String valveSeq = "Valve sequence: ";
  for (int i = 0; i < count; i++) {
    valveSeq += String(valveIndices[i]);
    if (i < count - 1)
      valveSeq += ", ";
    sequenceValves[i] = valveIndices[i];
  }
  DebugHelper::debug(valveSeq);

  sequenceLength = count;
  sequentialMode = true;
  currentSequenceIndex = 0;
  publishStateChange("system", "sequential_started");

  if (triggerType.length() > 0) {
    startTelegramSession(triggerType);

    String trayNumbers;
    for (int i = 0; i < sequenceLength; i++) {
      trayNumbers += String(sequenceValves[i] + 1); // Convert to 1-indexed
      if (i < sequenceLength - 1) {
        trayNumbers += ", ";
      }
    }

    queueTelegramNotification(
        TelegramNotifier::formatWateringStarted(sessionTriggerType, trayNumbers));
  }

  startNextInSequence();
}

inline void WateringSystem::stopSequentialWatering() {
  if (!sequentialMode)
    return;

  DebugHelper::debug("\n⚠️  SEQUENTIAL WATERING STOPPED");
  sequentialMode = false;

  for (int i = 0; i < NUM_VALVES; i++) {
    if (valves[i]->phase != PHASE_IDLE) {
      stopWatering(i);
    }
  }

  publishStateChange("system", "sequential_stopped");
}

inline bool WateringSystem::isValveComplete(int valveIndex) {
  return valves[valveIndex]->phase == PHASE_IDLE &&
         !valves[valveIndex]->wateringRequested;
}

inline void WateringSystem::startNextInSequence() {
  if (!sequentialMode)
    return;

  if (currentSequenceIndex >= sequenceLength) {
    DebugHelper::debug("\n╔═══════════════════════════════════════════╗");
    DebugHelper::debug("║  SEQUENTIAL WATERING COMPLETE ✓           ║");
    DebugHelper::debug("╚═══════════════════════════════════════════╝");
    sequentialMode = false;
    publishStateChange("system", "sequential_complete");

    // Send Telegram completion notification
    if (telegramSessionActive) {
      // Build results array for Telegram
      String results[NUM_VALVES][3];
      int resultCount = 0;

      for (int i = 0; i < NUM_VALVES; i++) {
        if (sessionData[i].active) {
          results[resultCount][0] = String(sessionData[i].trayNumber);
          results[resultCount][1] = String(sessionData[i].duration, 1);
          results[resultCount][2] = sessionData[i].status;
          resultCount++;
        }
      }

      // Queue completion notification (non-blocking, sent from Core 0)
      queueTelegramNotification(TelegramNotifier::formatWateringComplete(results, resultCount));
      endTelegramSession();

      // Queue updated watering schedule after sequential watering completes
      sendWateringSchedule("Updated Schedule");
    }

    return;
  }

  int valveIndex = sequenceValves[currentSequenceIndex];
  DebugHelper::debug("\n→ [Sequence " + String(currentSequenceIndex + 1) + "/" +
                     String(sequenceLength) + "] Starting Valve " +
                     String(valveIndex));

  startWatering(valveIndex, true); // Force watering - ignore learning algorithm
  currentSequenceIndex++;
}

// ========== Hardware Control ==========
inline bool WateringSystem::readRainSensor(int valveIndex) {
  // CRITICAL: Rain sensors need TWO power signals (per CLAUDE.md):
  // 1. Valve pin HIGH (specific sensor power)
  // 2. GPIO 18 HIGH (common rail enable)
  //
  // IMPORTANT: During PHASE_WATERING, GPIO 18 must stay HIGH continuously
  // to allow sensors to detect water as soon as it arrives. Turning it off
  // between reads causes sensor blindness (sensor only powered 10% of time).
  //
  // Power management strategy:
  // - PHASE_WATERING: Keep GPIO 18 HIGH (managed by state machine)
  // - Other phases: Turn on briefly for reading, then off

  // Ensure pins are configured correctly
  pinMode(VALVE_PINS[valveIndex], OUTPUT);
  pinMode(RAIN_SENSOR_POWER_PIN, OUTPUT);

  // Check if any valve is currently in PHASE_WATERING
  bool anyWatering = false;
  for (int i = 0; i < NUM_VALVES; i++) {
    if (valves[i]->phase == PHASE_WATERING) {
      anyWatering = true;
      break;
    }
  }

  // Power on sensor: valve pin + common rail
  digitalWrite(VALVE_PINS[valveIndex], HIGH);
  digitalWrite(RAIN_SENSOR_POWER_PIN, HIGH);

  // Only delay if we're turning on GPIO 18 for the first time
  // (if already watering, GPIO 18 is already HIGH and stable)
  if (!anyWatering) {
    delay(SENSOR_POWER_STABILIZATION);
  }

  // Read sensor: LOW = wet, HIGH = dry (with pull-up)
  int rawValue = digitalRead(RAIN_SENSOR_PINS[valveIndex]);

  // Power management: Only turn off GPIO 18 if NOT in watering phase
  // During watering, GPIO 18 stays HIGH for continuous sensor monitoring
  if (!anyWatering) {
    digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
  }

  // ENHANCED LOGGING: Log actual GPIO values for debugging
  static unsigned long lastDetailedLog = 0;
  if (millis() - lastDetailedLog > 5000) {  // Detailed log every 5s
    DebugHelper::debug("Sensor " + String(valveIndex) + " GPIO " + String(RAIN_SENSOR_PINS[valveIndex]) +
                      ": raw=" + String(rawValue) + " (" + String(rawValue == LOW ? "WET" : "DRY") +
                      "), GPIO18=" + String(anyWatering ? "CONTINUOUS" : "PULSED"));
    lastDetailedLog = millis();
  }

  return (rawValue == LOW); // LOW = wet/rain detected
}

inline void WateringSystem::openValve(int valveIndex) {
  // NOTE: No GPIO verification is performed after digitalWrite() because the relay
  // module has automatic sensor-based control. When the rain sensor is WET, the relay
  // module's hardware automatically opens the relay (disables power) regardless of
  // GPIO state. This is a hardware-level safety feature. GPIO read-back would show
  // LOW even when GPIO is set HIGH (expected behavior, not a failure).

  DebugHelper::debugImportant("🔧 OPENING VALVE " + String(valveIndex));
  DebugHelper::debug("  GPIO Pin: " + String(VALVE_PINS[valveIndex]));

  digitalWrite(VALVE_PINS[valveIndex], HIGH);

  valves[valveIndex]->state = VALVE_OPEN;
  activeValveCount++;

  DebugHelper::debug("✓ Valve " + String(valveIndex) + " marked as OPEN");
}

inline void WateringSystem::closeValve(int valveIndex) {
  // Check if already closed (idempotent - safe to call multiple times)
  if (valves[valveIndex]->state == VALVE_CLOSED) {
    return; // Already closed, nothing to do
  }

  // Close valve hardware
  digitalWrite(VALVE_PINS[valveIndex], LOW);
  valves[valveIndex]->state = VALVE_CLOSED;
  if (activeValveCount > 0)
    activeValveCount--;

  DebugHelper::debugImportant("🔧 CLOSING VALVE " + String(valveIndex) +
                              " (GPIO " + String(VALVE_PINS[valveIndex]) + ")");
}

inline void WateringSystem::updatePumpState() {
  // Count valves that are actually watering (not just open)
  int wateringCount = 0;
  for (int i = 0; i < NUM_VALVES; i++) {
    if (valves[i]->phase == PHASE_WATERING) {
      wateringCount++;
    }
  }

  // Turn pump on if any valve is watering
  if (wateringCount > 0 && pumpState == PUMP_OFF) {
    digitalWrite(PUMP_PIN, HIGH);
    // Turn LED blue when pump is on
    statusLED.setPixelColor(0, statusLED.Color(0, 0, 255));
    statusLED.show();
    pumpState = PUMP_ON;
    DebugHelper::debugImportant("💧 Pump ON (GPIO " + String(PUMP_PIN) + ")");
    publishStateChange("pump", "on");
  } else if (wateringCount == 0 && pumpState == PUMP_ON) {
    digitalWrite(PUMP_PIN, LOW);
    // Turn LED off when pump is off
    statusLED.clear();
    statusLED.show();
    pumpState = PUMP_OFF;
    DebugHelper::debugImportant("💧 Pump OFF (GPIO " + String(PUMP_PIN) + ")");
    publishStateChange("pump", "off");
  }
}

// ========== Helper: Schedule Update ==========
inline void WateringSystem::sendScheduleUpdateIfNeeded() {
  if (!sequentialMode) {
    DebugHelper::debug("📅 Queuing updated watering schedule...");
    sendWateringSchedule("Updated Schedule");
  }
}

// ========== Time-Based Learning Algorithm ==========
inline void WateringSystem::processLearningData(ValveController *valve,
                                                unsigned long currentTime) {
  // Algorithm constants (extracted for easy tuning)
  const unsigned long BASE_INTERVAL_MS = 86400000; // 24 hours
  const float BASELINE_TOLERANCE =
      0.65; // 65% - threshold for "tray not fully empty" (lowered from 70% in v1.18.2 to reduce interval growth)
  const long FILL_STABLE_TOLERANCE_MS =
      500; // ±0.5s - threshold for "same fill time"
  const float MIN_INTERVAL_MULTIPLIER = 1.0; // Never go below 24h
  const float INTERVAL_DOUBLE = 2.0;         // Double when tray already full
  const float INTERVAL_INCREMENT_LARGE =
      1.0; // Large adjustment for coarse search
  const float INTERVAL_DECREMENT_BINARY =
      0.5; // Binary search refinement (decrease)
  const float INTERVAL_INCREMENT_FINE = 0.25; // Fine-tuning adjustment

  // Handle timeout scenarios
  if (valve->timeoutOccurred) {
    // SPECIAL CASE: First watering timeout on uncalibrated valve
    // Set up 24-hour auto-retry to allow the system to try again
    if (!valve->isCalibrated) {
      DebugHelper::debugImportant("⏰ TIMEOUT on first watering - scheduling 24h retry");

      // Set up minimal learning data to enable auto-watering retry
      valve->lastWateringAttemptTime = currentTime;
      valve->realTimeSinceLastWateringAttempt = 0;
      valve->emptyToFullDuration = BASE_INTERVAL_MS; // 24 hours
      valve->intervalMultiplier = MIN_INTERVAL_MULTIPLIER; // 1.0x
      // Keep isCalibrated = false so next successful watering establishes baseline

      DebugHelper::debugImportant("  Next attempt in: 24 hours");
      DebugHelper::debug("  Valve remains uncalibrated until successful watering");

      saveLearningData();
      sendScheduleUpdateIfNeeded();
      return;
    }

    // For calibrated valves, skip learning on timeout
    DebugHelper::debug("🧠 Skipping learning - timeout occurred (calibrated valve)");
    return;
  }

  // CASE 1: Tray already full (sensor wet before pump started)
  // wateringStartTime == 0 means pump never started, rainDetected == true means
  // sensor is wet
  if (valve->wateringStartTime == 0 && valve->rainDetected) {
    // CRITICAL: Detect restart/power-outage scenarios
    // If tray was recently watered (< 2 hours ago), this is likely a restart
    // Don't punish with interval doubling - just skip this cycle
    if (valve->lastWateringCompleteTime > 0) {
      unsigned long timeSinceLastWatering =
          currentTime - valve->lastWateringCompleteTime;

      // Handle millis() overflow (rare but possible)
      if (timeSinceLastWatering > 4000000000UL) {
        // Overflow detected, assume long time passed
        timeSinceLastWatering = RECENT_WATERING_THRESHOLD_MS + 1000;
      }

      if (timeSinceLastWatering < RECENT_WATERING_THRESHOLD_MS) {
        // Recently watered - likely restart/power outage scenario
        DebugHelper::debugImportant(
            "🧠 RESTART DETECTION: Tray wet from recent watering");
        DebugHelper::debugImportant(
            "  Time since last watering: " +
            LearningAlgorithm::formatDuration(timeSinceLastWatering));
        DebugHelper::debugImportant(
            "  Skipping cycle (no interval change) - likely power outage/restart");

        // Don't modify interval - just skip this cycle
        // Update attempt time to prevent auto-watering retry loops
        valve->lastWateringAttemptTime = currentTime;
        valve->realTimeSinceLastWateringAttempt = 0;
        saveLearningData();
        sendScheduleUpdateIfNeeded();
        return;
      }
    }

    // CRITICAL: Detect overflow recovery scenarios
    // If overflow was recently reset (< 2 hours ago), tray may have been
    // refilled during overflow period (rain, manual watering, etc.)
    // Don't punish with interval doubling - just skip this cycle
    if (lastOverflowResetTime > 0) {
      unsigned long timeSinceOverflowReset = currentTime - lastOverflowResetTime;

      // Handle millis() overflow (rare but possible)
      if (timeSinceOverflowReset > 4000000000UL) {
        // Overflow detected, assume long time passed
        timeSinceOverflowReset = OVERFLOW_RECOVERY_THRESHOLD_MS + 1000;
      }

      if (timeSinceOverflowReset < OVERFLOW_RECOVERY_THRESHOLD_MS) {
        // Overflow was recently reset - tray may have been refilled during overflow period
        DebugHelper::debugImportant(
            "🧠 OVERFLOW RECOVERY DETECTION: Tray wet after overflow reset");
        DebugHelper::debugImportant(
            "  Time since overflow reset: " +
            LearningAlgorithm::formatDuration(timeSinceOverflowReset));
        DebugHelper::debugImportant(
            "  Skipping cycle (no interval change) - watering was blocked by overflow");

        // Don't modify interval - just skip this cycle
        // Update attempt time to prevent auto-watering retry loops
        valve->lastWateringAttemptTime = currentTime;
        valve->realTimeSinceLastWateringAttempt = 0;
        saveLearningData();
        sendScheduleUpdateIfNeeded();
        return;
      }
    }

    // Tray has been wet for a LONG time - genuinely slow consumption
    DebugHelper::debugImportant("🧠 ADAPTIVE LEARNING: Tray still full after long time");

    // Double the interval (exponential backoff) - tray is consuming water
    // slower than expected
    float oldMultiplier = valve->intervalMultiplier;
    valve->intervalMultiplier *= INTERVAL_DOUBLE;
    valve->emptyToFullDuration =
        (unsigned long)(BASE_INTERVAL_MS * valve->intervalMultiplier);
    valve->totalWateringCycles++;

    DebugHelper::debugImportant("  Interval: " + String(oldMultiplier, 2) +
                                "x → " + String(valve->intervalMultiplier, 2) +
                                "x (doubled)");
    DebugHelper::debugImportant(
        "  Next attempt in: " +
        LearningAlgorithm::formatDuration(valve->emptyToFullDuration));

    #ifdef METRICS_PUSHER_H
    MetricsPusher::logInfo("Valve " + String(valve->valveIndex) + ": learning: tray full, interval " + String(oldMultiplier, 2) + "x->" + String(valve->intervalMultiplier, 2) + "x");
    #endif
    saveLearningData();
    sendScheduleUpdateIfNeeded();
    return;
  }

  // CASE 2: Successful watering (pump ran and sensor became wet)
  if (!valve->rainDetected || valve->wateringStartTime == 0) {
    return; // Not a successful watering
  }

  unsigned long fillDuration = currentTime - valve->wateringStartTime;
  valve->lastFillDuration = fillDuration;
  valve->totalWateringCycles++;

  DebugHelper::debug("🧠 ADAPTIVE LEARNING:");
  DebugHelper::debug("  Fill duration: " + String(fillDuration / 1000.0, 1) +
                     "s");

  // CASE 2A: First successful watering - establish baseline
  if (!valve->isCalibrated) {
    valve->isCalibrated = true;
    valve->baselineFillDuration = fillDuration;
    valve->previousFillDuration = fillDuration;
    valve->intervalMultiplier = MIN_INTERVAL_MULTIPLIER;
    valve->emptyToFullDuration = BASE_INTERVAL_MS; // Start with 24h interval
    valve->lastWateringCompleteTime = currentTime;
    valve->realTimeSinceLastWatering = 0; // Clear outage duration
    valve->realTimeSinceLastWateringAttempt = 0;
    valve->lastWaterLevelPercent = 0.0;

    DebugHelper::debugImportant(
        "  ✨ INITIAL CALIBRATION: " + String(fillDuration / 1000.0, 1) + "s");
    DebugHelper::debug("  Baseline will auto-update when tray is emptier");
    DebugHelper::debug("  Starting interval: 1.0x (24 hours)");

    publishStateChange("valve" + String(valve->valveIndex),
                       "initial_calibration");
    saveLearningData();
    sendScheduleUpdateIfNeeded();
    return;
  }

  // CASE 2B: Subsequent waterings - adaptive interval adjustment
  float fillSeconds = fillDuration / 1000.0;
  float baselineSeconds = valve->baselineFillDuration / 1000.0;
  float previousSeconds = valve->previousFillDuration / 1000.0;
  float oldMultiplier = valve->intervalMultiplier;

  DebugHelper::debug("  Baseline: " + String(baselineSeconds, 1) + "s");
  DebugHelper::debug("  Previous: " + String(previousSeconds, 1) + "s");
  DebugHelper::debug("  Current multiplier: " + String(oldMultiplier, 2) + "x");

  // Adaptive interval adjustment algorithm
  if (fillDuration < valve->baselineFillDuration * BASELINE_TOLERANCE) {
    // Fill < 85% of baseline - tray not fully empty, need longer interval
    valve->intervalMultiplier += INTERVAL_INCREMENT_LARGE;
    DebugHelper::debugImportant(
        "  ⬆️  Fill < baseline → Interval: " + String(oldMultiplier, 2) +
        "x → " + String(valve->intervalMultiplier, 2) + "x (+" +
        String(INTERVAL_INCREMENT_LARGE, 1) + ")");
  } else if (fillDuration > valve->baselineFillDuration) {
    // Fill > baseline - tray was emptier than ever! Update baseline AND
    // increase interval
    valve->baselineFillDuration = fillDuration;
    valve->intervalMultiplier += INTERVAL_INCREMENT_LARGE;
    DebugHelper::debugImportant("  ✨ NEW BASELINE: " + String(fillSeconds, 1) +
                                "s");
    DebugHelper::debugImportant("  ⬆️  Interval: " + String(oldMultiplier, 2) +
                                "x → " + String(valve->intervalMultiplier, 2) +
                                "x (+" + String(INTERVAL_INCREMENT_LARGE, 1) +
                                ")");
  } else {
    // Fill ≈ baseline (within 15%) - fine-tuning phase
    long fillDiff = (long)(fillDuration - valve->previousFillDuration);

    if (abs(fillDiff) < FILL_STABLE_TOLERANCE_MS) {
      // Same as last time (±0.5s) - interval might be too long, decrease for
      // binary search
      valve->intervalMultiplier -= INTERVAL_DECREMENT_BINARY;
      if (valve->intervalMultiplier < MIN_INTERVAL_MULTIPLIER) {
        valve->intervalMultiplier =
            MIN_INTERVAL_MULTIPLIER; // Never go below 24h
      }
      DebugHelper::debugImportant(
          "  🎯 Fill stable → Interval: " + String(oldMultiplier, 2) + "x → " +
          String(valve->intervalMultiplier, 2) + "x (-" +
          String(INTERVAL_DECREMENT_BINARY, 1) + ")");

      // Check if we've found optimal (stable baseline for 2+ cycles)
      if (valve->totalWateringCycles > 2 && fillDiff == 0) {
        DebugHelper::debugImportant("  ✅ OPTIMAL INTERVAL FOUND: " +
                                    String(valve->intervalMultiplier, 2) + "x");
      }
    } else if (fillDuration < valve->previousFillDuration) {
      // Fill decreased from last time - tray was less empty, interval too short
      valve->intervalMultiplier += INTERVAL_INCREMENT_FINE;
      DebugHelper::debugImportant(
          "  ⬆️  Fill decreased → Interval: " + String(oldMultiplier, 2) +
          "x → " + String(valve->intervalMultiplier, 2) + "x (+" +
          String(INTERVAL_INCREMENT_FINE, 2) + ")");
    } else {
      // Fill increased from last time - tray was emptier, interval too long
      valve->intervalMultiplier -= INTERVAL_INCREMENT_FINE;
      if (valve->intervalMultiplier < MIN_INTERVAL_MULTIPLIER) {
        valve->intervalMultiplier = MIN_INTERVAL_MULTIPLIER;
      }
      DebugHelper::debugImportant(
          "  ⬇️  Fill increased → Interval: " + String(oldMultiplier, 2) +
          "x → " + String(valve->intervalMultiplier, 2) + "x (-" +
          String(INTERVAL_INCREMENT_FINE, 2) + ")");
    }
  }

  // Update state
  valve->previousFillDuration = fillDuration;
  valve->emptyToFullDuration =
      (unsigned long)(BASE_INTERVAL_MS * valve->intervalMultiplier);
  valve->lastWateringCompleteTime = currentTime;
  valve->realTimeSinceLastWatering = 0; // Clear outage duration
  valve->realTimeSinceLastWateringAttempt = 0;

  // Calculate water level before this watering (for logging)
  float waterLevelBefore = LearningAlgorithm::calculateWaterLevelBefore(
      fillDuration, valve->baselineFillDuration);
  valve->lastWaterLevelPercent = waterLevelBefore;

  DebugHelper::debugImportant(
      "  ⏰ Next watering in: " +
      LearningAlgorithm::formatDuration(valve->emptyToFullDuration) + " (" +
      String(valve->intervalMultiplier, 2) + "x)");
  DebugHelper::debug("  Water level before: " + String((int)waterLevelBefore) +
                     "% (" + String(getTrayState(waterLevelBefore)) + ")");
  DebugHelper::debug("  Total cycles: " + String(valve->totalWateringCycles));

  #ifdef METRICS_PUSHER_H
  MetricsPusher::logInfo("Valve " + String(valve->valveIndex) + ": learning: fill=" + String(fillDuration / 1000.0, 1) + "s interval " + String(oldMultiplier, 2) + "x->" + String(valve->intervalMultiplier, 2) + "x");
  #endif
  saveLearningData();
  sendScheduleUpdateIfNeeded();
}

inline void WateringSystem::logLearningData(ValveController *valve,
                                            float waterLevelBefore,
                                            unsigned long emptyDuration) {
  DebugHelper::debug("  Baseline fill: " + LearningAlgorithm::formatDuration(
                                               valve->baselineFillDuration));
  DebugHelper::debug("  Current fill: " + LearningAlgorithm::formatDuration(
                                              valve->lastFillDuration));

  // Check if baseline was just updated
  if (valve->lastFillDuration >= valve->baselineFillDuration &&
      valve->totalWateringCycles > 1) {
    DebugHelper::debug("  ✨ Baseline updated - tray was emptier than before");
  }

  DebugHelper::debug("  Water level before: " + String((int)waterLevelBefore) +
                     "%");

  const char *state = getTrayState(waterLevelBefore);
  DebugHelper::debug("  Tray state was: " + String(state));

  if (emptyDuration > 0) {
    DebugHelper::debug("  Estimated empty time: " +
                       LearningAlgorithm::formatDuration(emptyDuration));
    DebugHelper::debug("  Learning cycles: " +
                       String(valve->totalWateringCycles));

    if (valve->autoWateringEnabled) {
      DebugHelper::debug("  ⏰ Auto-watering enabled - will water when empty");
    } else {
      DebugHelper::debug("  ⚠️  Auto-watering disabled - manual watering only");
    }
  } else {
    DebugHelper::debug("  ⚠️  Not enough data for consumption estimate yet");
  }

  String learningMsg =
      "{\"valve\":" + String(valve->valveIndex) +
      ",\"fillDuration\":" + String(valve->lastFillDuration) +
      ",\"baseline\":" + String(valve->baselineFillDuration) +
      ",\"waterLevelBefore\":" + String((int)waterLevelBefore) +
      ",\"emptyDuration\":" + String(emptyDuration) + "}";
  publishStateChange("learning", learningMsg);
}

inline void WateringSystem::resetCalibration(int valveIndex) {
  if (valveIndex < 0 || valveIndex >= NUM_VALVES)
    return;

  ValveController *valve = valves[valveIndex];

  // Reset all calibration data (idempotent - safe to call multiple times)
  valve->isCalibrated = false;
  valve->baselineFillDuration = 0;
  valve->lastFillDuration = 0;
  valve->previousFillDuration = 0;
  valve->emptyToFullDuration = 0;
  valve->lastWateringCompleteTime = 0;
  valve->lastWateringAttemptTime = 0;
  valve->realTimeSinceLastWatering = 0;
  valve->realTimeSinceLastWateringAttempt = 0;
  valve->lastWaterLevelPercent = 0.0;
  valve->totalWateringCycles = 0;
  valve->intervalMultiplier = 1.0; // CRITICAL: Reset interval multiplier

  DebugHelper::debug("═══════════════════════════════════════");
  DebugHelper::debug("🔄 CALIBRATION RESET: Valve " + String(valveIndex));
  DebugHelper::debug("  All learning data cleared (interval: 1.0x)");
  DebugHelper::debug("  Next watering will establish new baseline");
  DebugHelper::debug("═══════════════════════════════════════");
  publishStateChange("valve" + String(valveIndex), "calibration_reset");

  // Save to flash
  saveLearningData();
}

inline void WateringSystem::resetAllCalibrations() {
  DebugHelper::debug("═══════════════════════════════════════");
  DebugHelper::debug("🔄 RESET ALL CALIBRATIONS");
  for (int i = 0; i < NUM_VALVES; i++) {
    valves[i]->isCalibrated = false;
    valves[i]->baselineFillDuration = 0;
    valves[i]->lastFillDuration = 0;
    valves[i]->previousFillDuration = 0;
    valves[i]->emptyToFullDuration = 0;
    valves[i]->lastWateringCompleteTime = 0;
    valves[i]->lastWateringAttemptTime = 0;
    valves[i]->realTimeSinceLastWatering = 0;
    valves[i]->realTimeSinceLastWateringAttempt = 0;
    valves[i]->lastWaterLevelPercent = 0.0;
    valves[i]->totalWateringCycles = 0;
    valves[i]->intervalMultiplier = 1.0; // CRITICAL: Reset interval multiplier
  }
  DebugHelper::debug("  All valves reset to uncalibrated state (intervals: 1.0x)");
  DebugHelper::debug("═══════════════════════════════════════");
  publishStateChange("system", "all_calibrations_reset");

  // Save to flash
  saveLearningData();
}

inline void WateringSystem::printLearningStatus() {
  DebugHelper::debug("\n╔═══════════════════════════════════════════╗");
  DebugHelper::debug("║    TIME-BASED LEARNING SYSTEM STATUS      ║");
  DebugHelper::debug("╚═══════════════════════════════════════════╝");

  unsigned long currentTime = millis();

  for (int i = 0; i < NUM_VALVES; i++) {
    ValveController *valve = valves[i];
    DebugHelper::debug("\n📊 Valve " + String(i) + ":");

    if (valve->isCalibrated) {
      DebugHelper::debug("  Status: ✓ Calibrated");
      DebugHelper::debug(
          "  Baseline fill: " +
          LearningAlgorithm::formatDuration(valve->baselineFillDuration));
      DebugHelper::debug("  Last fill: " + LearningAlgorithm::formatDuration(
                                               valve->lastFillDuration));
      DebugHelper::debug("  Total cycles: " +
                         String(valve->totalWateringCycles));

      if (valve->emptyToFullDuration > 0) {
        DebugHelper::debug(
            "  Empty-to-full time: " +
            LearningAlgorithm::formatDuration(valve->emptyToFullDuration));

        // Calculate current water level
        float currentWaterLevel =
            calculateCurrentWaterLevel(valve, currentTime);
        DebugHelper::debug("  Current water level: ~" +
                           String((int)currentWaterLevel) + "% (" +
                           String(getTrayState(currentWaterLevel)) + ")");

        // Time until empty
        if (currentWaterLevel > 0) {
          unsigned long timeSinceWatering =
              currentTime - valve->lastWateringCompleteTime;
          unsigned long timeRemaining =
              valve->emptyToFullDuration - timeSinceWatering;
          DebugHelper::debug("  Time until empty: ~" +
                             LearningAlgorithm::formatDuration(timeRemaining));
        } else {
          DebugHelper::debug("  Time until empty: Now (should water!)");
        }
      } else {
        DebugHelper::debug("  Empty-to-full time: Unknown (need more data)");
      }

      DebugHelper::debug("  Auto-watering: " + String(valve->autoWateringEnabled
                                                          ? "Enabled ✓"
                                                          : "Disabled ✗"));
    } else {
      DebugHelper::debug("  Status: ⚠️  Not calibrated");
      DebugHelper::debug("  Action: Run first watering to establish baseline");
      DebugHelper::debug("  Auto-watering: " + String(valve->autoWateringEnabled
                                                          ? "Enabled ✓"
                                                          : "Disabled ✗"));
    }
  }
  DebugHelper::debug("\n═══════════════════════════════════════");
}

inline void WateringSystem::setAutoWatering(int valveIndex, bool enabled) {
  if (valveIndex < 0 || valveIndex >= NUM_VALVES)
    return;

  valves[valveIndex]->autoWateringEnabled = enabled;
  DebugHelper::debug("⏰ Valve " + String(valveIndex) + " auto-watering: " +
                     String(enabled ? "ENABLED" : "DISABLED"));
  publishStateChange("valve" + String(valveIndex),
                     enabled ? "auto_enabled" : "auto_disabled");

  // Save to flash
  saveLearningData();
}

inline void WateringSystem::setAllAutoWatering(bool enabled) {
  DebugHelper::debug("═══════════════════════════════════════");
  DebugHelper::debug("⏰ SET ALL AUTO-WATERING: " +
                     String(enabled ? "ENABLED" : "DISABLED"));
  for (int i = 0; i < NUM_VALVES; i++) {
    valves[i]->autoWateringEnabled = enabled;
  }
  DebugHelper::debug("═══════════════════════════════════════");
  publishStateChange("system",
                     enabled ? "all_auto_enabled" : "all_auto_disabled");

  // Save to flash
  saveLearningData();
}

inline void WateringSystem::clearTimeoutFlag(int valveIndex) {
  if (valveIndex >= 0 && valveIndex < NUM_VALVES) {
    valves[valveIndex]->timeoutOccurred = false;
    DebugHelper::debug("Timeout flag cleared for valve " + String(valveIndex));
    publishStateChange("valve" + String(valveIndex), "timeout_cleared");
  }
}

// ========== Telegram Session Tracking ==========
inline void WateringSystem::startTelegramSession(const String &triggerType) {
  telegramSessionActive = true;
  sessionTriggerType = triggerType;

  // Reset all session data
  for (int i = 0; i < NUM_VALVES; i++) {
    sessionData[i].active = false;
    sessionData[i].status = "";
    sessionData[i].startTime = 0;
    sessionData[i].endTime = 0;
    sessionData[i].duration = 0.0;
  }

  DebugHelper::debug("📱 Telegram session started - Trigger: " + triggerType);
}

inline void WateringSystem::recordSessionStart(int valveIndex) {
  if (!telegramSessionActive || valveIndex < 0 || valveIndex >= NUM_VALVES)
    return;

  sessionData[valveIndex].active = true;
  sessionData[valveIndex].trayNumber = valveIndex + 1; // Convert to 1-indexed
  sessionData[valveIndex].startTime = millis();
  sessionData[valveIndex].status = "IN_PROGRESS";

  DebugHelper::debug("📱 Session tracking: Tray " + String(valveIndex + 1) +
                     " started");
}

inline void WateringSystem::recordSessionEnd(int valveIndex,
                                             const String &status) {
  if (!telegramSessionActive || valveIndex < 0 || valveIndex >= NUM_VALVES)
    return;
  if (!sessionData[valveIndex].active)
    return;

  sessionData[valveIndex].endTime = millis();

  // CORRECT: Calculate duration from valve OPEN to valve CLOSE (full cycle
  // time) Use valveOpenTime instead of sessionData startTime for accurate
  // measurement
  unsigned long startTime = valves[valveIndex]->valveOpenTime;
  if (startTime == 0) {
    // Fallback to session start if valve time not set
    startTime = sessionData[valveIndex].startTime;
  }
  sessionData[valveIndex].duration =
      (sessionData[valveIndex].endTime - startTime) / 1000.0;
  sessionData[valveIndex].status = status;

  DebugHelper::debug("📱 Session tracking: Tray " + String(valveIndex + 1) +
                     " ended - Status: " + status + ", Duration: " +
                     String(sessionData[valveIndex].duration, 1) +
                     "s (valve open to close)");
}

inline void WateringSystem::endTelegramSession() {
  if (!telegramSessionActive)
    return;

  DebugHelper::debug("📱 Telegram session ended - preparing completion report");
  telegramSessionActive = false;
}

// ========== Watering Schedule Notification ==========
inline void WateringSystem::sendWateringSchedule(const String &title) {
  // Get current time from system time (already set from RTC at boot)
  time_t now;
  time(&now);
  struct tm *timeinfo = localtime(&now);

  unsigned long currentTime = millis();

  // Build schedule data for all valves (4 columns: tray, planned, duration,
  // cycle)
  String scheduleData[NUM_VALVES][4];

  for (int i = 0; i < NUM_VALVES; i++) {
    ValveController *valve = valves[i];

    // Column 0: Tray number (1-indexed)
    scheduleData[i][0] = String(i + 1);

    // Column 2: Duration (expected watering runtime in seconds)
    if (valve->baselineFillDuration > 0) {
      float durationSec = valve->baselineFillDuration / 1000.0;
      scheduleData[i][2] = String(durationSec, 1);
    } else if (valve->emptyToFullDuration > 0 || valve->isCalibrated) {
      // Retry/calibration-in-progress path: baseline is unknown yet, but valve can
      // still water using per-valve timeout safeguards.
      float fallbackDurationSec = getValveNormalTimeout(i) / 1000.0;
      scheduleData[i][2] = String(fallbackDurationSec, 1);
    } else {
      scheduleData[i][2] = "-";
    }

    // Column 3: Cycle (watering interval in hours)
    float cycleHours = valve->intervalMultiplier * 24.0;
    scheduleData[i][3] = String((int)cycleHours);

    // Column 1: Planned time
    if (!valve->isCalibrated && valve->emptyToFullDuration == 0) {
      // Not calibrated and no temporary duration set
      scheduleData[i][1] = "Not calibrtd";
    } else if (!valve->isCalibrated && valve->emptyToFullDuration > 0) {
      // Not calibrated but has temporary 24h retry duration (tray was found
      // full)
      unsigned long timeSinceWatering = hasLastWateringAttemptReference(valve)
                                            ? getTimeSinceLastWateringAttempt(
                                                  valve, currentTime)
                                            : getTimeSinceLastWatering(
                                                  valve, currentTime);

      if (timeSinceWatering >= valve->emptyToFullDuration) {
        scheduleData[i][1] = "Now (retry)";
      } else {
        unsigned long timeUntilRetry =
            valve->emptyToFullDuration - timeSinceWatering;
        time_t plannedTime = now + (timeUntilRetry / 1000);
        struct tm plannedTm;
        localtime_r(&plannedTime, &plannedTm);
        char buffer[20];
        strftime(buffer, sizeof(buffer), "%d/%m %H:%M", &plannedTm);
        scheduleData[i][1] = String(buffer);
      }
    } else if (!valve->autoWateringEnabled) {
      scheduleData[i][1] = "Auto disbld";
    } else if (valve->emptyToFullDuration == 0) {
      // Learning mode - use minimum interval (24h) from last attempt
      if (hasLastWateringAttemptReference(valve) ||
          hasLastWateringReference(valve)) {
        unsigned long timeSinceAttempt = hasLastWateringAttemptReference(valve)
                                             ? getTimeSinceLastWateringAttempt(
                                                   valve, currentTime)
                                             : getTimeSinceLastWatering(
                                                   valve, currentTime);
        if (timeSinceAttempt >= AUTO_WATERING_MIN_INTERVAL_MS) {
          scheduleData[i][1] = "Now (learn)";
        } else {
          unsigned long timeUntilNext =
              AUTO_WATERING_MIN_INTERVAL_MS - timeSinceAttempt;
          time_t plannedTime = now + (timeUntilNext / 1000);
          struct tm plannedTm;
          localtime_r(&plannedTime, &plannedTm);
          char buffer[20];
          strftime(buffer, sizeof(buffer), "%d/%m %H:%M", &plannedTm);
          scheduleData[i][1] = String(buffer);
        }
      } else {
        scheduleData[i][1] = "Now (learn)";
      }
    } else {
      // Calculate planned watering time based on learned consumption
      unsigned long timeSinceWatering =
          getTimeSinceLastWatering(valve, currentTime);

      // Calculate effective watering interval considering 24h minimum safety check
      // This matches the logic in shouldWaterNow() (ValveController.h:160-192)
      // Apply 24h minimum interval if we have lastWateringAttemptTime
      if (hasLastWateringAttemptReference(valve)) {
        unsigned long timeSinceAttempt =
            getTimeSinceLastWateringAttempt(valve, currentTime);
        unsigned long timeUntilMinInterval = 0;

        if (timeSinceAttempt < AUTO_WATERING_MIN_INTERVAL_MS) {
          timeUntilMinInterval = AUTO_WATERING_MIN_INTERVAL_MS - timeSinceAttempt;
        }

        // Use the maximum of learned interval and minimum safety interval
        unsigned long timeUntilLearnedInterval =
            (timeSinceWatering < valve->emptyToFullDuration)
            ? (valve->emptyToFullDuration - timeSinceWatering)
            : 0;

        if (timeUntilMinInterval > timeUntilLearnedInterval) {
          // 24h minimum not reached yet - this is the actual blocking condition
          time_t plannedTime = now + (timeUntilMinInterval / 1000);
          struct tm plannedTm;
          localtime_r(&plannedTime, &plannedTm);
          char buffer[20];
          strftime(buffer, sizeof(buffer), "%d/%m %H:%M", &plannedTm);
          scheduleData[i][1] = String(buffer);
          continue; // Skip to next valve
        }
      }

      if (timeSinceWatering >= valve->emptyToFullDuration) {
        // Already due for watering
        scheduleData[i][1] = "Now";
      } else {
        // Calculate future watering time
        unsigned long timeUntilWatering =
            valve->emptyToFullDuration - timeSinceWatering;
        time_t plannedTime = now + (timeUntilWatering / 1000);

        // Safety check: if planned time is in the past, show "Now"
        if (plannedTime <= now) {
          scheduleData[i][1] = "Now";
        } else {
          // Format as date/time - always show full date for clarity
          struct tm plannedTm;
          localtime_r(&plannedTime, &plannedTm);

          char buffer[20];
          strftime(buffer, sizeof(buffer), "%d/%m %H:%M", &plannedTm);

          scheduleData[i][1] = String(buffer);
        }
      }
    }
  }

  // Queue schedule notification (non-blocking, sent from Core 0)
  queueTelegramNotification(TelegramNotifier::formatWateringSchedule(scheduleData, NUM_VALVES, title));
}

// ========== Boot Watering Decision Helpers ==========
inline bool WateringSystem::isFirstBoot() {
  // Check if all valves are uncalibrated (fresh device, no learning data)
  for (int i = 0; i < NUM_VALVES; i++) {
    if (valves[i]->isCalibrated) {
      return false; // At least one valve has data
    }
  }
  return true; // All valves uncalibrated = first boot
}

inline bool WateringSystem::hasOverdueValves() {
  return getOverdueValveIndices(nullptr, 0) > 0;
}

inline int WateringSystem::getOverdueValveIndices(int *valveIndices, int maxCount) {
  unsigned long currentTime = millis();
  int overdueCount = 0;

  for (int i = 0; i < NUM_VALVES; i++) {
    ValveController *valve = valves[i];

    // Skip if auto-watering disabled
    if (!valve->autoWateringEnabled) {
      continue;
    }

    // Skip if not calibrated
    if (!valve->isCalibrated) {
      continue;
    }

    // Check for overdue watering using either millis timestamp or real time duration
    if (valve->emptyToFullDuration > 0) {
      bool isOverdue = false;

      if (hasLastWateringReference(valve)) {
        unsigned long totalTimeSinceWatering =
            getTimeSinceLastWatering(valve, currentTime);
        isOverdue = (totalTimeSinceWatering >= valve->emptyToFullDuration);
      }

      if (isOverdue) {
        DebugHelper::debug(
            "Valve " + String(i) + " is overdue (interval: " +
            LearningAlgorithm::formatDuration(valve->emptyToFullDuration) + ")");
        if (valveIndices && overdueCount < maxCount) {
          valveIndices[overdueCount] = i;
        }
        overdueCount++;
      }
    }
  }

  return overdueCount;
}

// ========== Halt Mode Control ==========
inline void WateringSystem::setHaltMode(bool enabled) {
  haltMode = enabled;

  if (enabled) {
    DebugHelper::debugImportant("🛑 HALT MODE ACTIVATED");
    DebugHelper::debugImportant("  All watering operations BLOCKED");
    DebugHelper::debugImportant("  System ready for firmware update");
    DebugHelper::debugImportant("  Send /resume to exit halt mode");

    // Stop any ongoing watering
    if (sequentialMode) {
      stopSequentialWatering();
    }
    for (int i = 0; i < NUM_VALVES; i++) {
      if (valves[i]->phase != PHASE_IDLE) {
        stopWatering(i);
      }
    }

    // Turn off LED
    statusLED.clear();
    statusLED.show();
  } else {
    DebugHelper::debugImportant("▶️ HALT MODE DEACTIVATED");
    DebugHelper::debugImportant("  Normal operations resumed");
  }
}

// ========== Sensor Diagnostic Functions ==========
inline void WateringSystem::testSensor(int valveIndex) {
  if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
    DebugHelper::debugImportant("❌ Invalid valve index: " + String(valveIndex));
    return;
  }

  DebugHelper::debugImportant("🔍 TESTING SENSOR " + String(valveIndex) + ":");

  // Test 1: Check power pin configuration
  DebugHelper::debug("  1️⃣ Checking power pin (GPIO " + String(RAIN_SENSOR_POWER_PIN) + ")");
  pinMode(RAIN_SENSOR_POWER_PIN, OUTPUT);
  DebugHelper::debug("     ✓ Power pin configured as OUTPUT");

  // Test 2: Check sensor pin configuration
  DebugHelper::debug("  2️⃣ Checking sensor pin (GPIO " + String(RAIN_SENSOR_PINS[valveIndex]) + ")");
  pinMode(RAIN_SENSOR_PINS[valveIndex], INPUT_PULLUP);
  DebugHelper::debug("     ✓ Sensor pin configured as INPUT_PULLUP");

  // Test 3: Read sensor with power OFF (should be HIGH due to pullup)
  digitalWrite(VALVE_PINS[valveIndex], LOW);
  digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
  delay(100);
  int valueOff = digitalRead(RAIN_SENSOR_PINS[valveIndex]);
  DebugHelper::debug("  3️⃣ Sensor reading (power OFF): " + String(valueOff) +
                     " (" + String(valueOff == HIGH ? "HIGH - DRY ✓" : "LOW - UNEXPECTED ⚠️") + ")");

  // Test 4: Read sensor with power ON (actual reading)
  // CRITICAL: Sensor needs valve pin HIGH + GPIO 18 HIGH
  digitalWrite(VALVE_PINS[valveIndex], HIGH);
  digitalWrite(RAIN_SENSOR_POWER_PIN, HIGH);
  delay(SENSOR_POWER_STABILIZATION);
  int valueOn = digitalRead(RAIN_SENSOR_PINS[valveIndex]);
  digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
  digitalWrite(VALVE_PINS[valveIndex], LOW);

  DebugHelper::debug("  4️⃣ Sensor reading (power ON): " + String(valueOn) +
                     " (" + String(valueOn == LOW ? "LOW - WET 💧" : "HIGH - DRY ☀️") + ")");

  // Test 5: Voltage check (if available)
  DebugHelper::debug("  5️⃣ Final result: Sensor is " +
                     String(valueOn == LOW ? "WET (tray is FULL)" : "DRY (tray is EMPTY)"));

  // Summary
  if (valueOff != HIGH) {
    DebugHelper::debugImportant("  ⚠️ WARNING: Sensor reads LOW when power is OFF - check pullup resistor!");
  }

  DebugHelper::debug("  ✓ Test complete for sensor " + String(valveIndex));
}

inline void WateringSystem::testAllSensors() {
  DebugHelper::debugImportant("═══════════════════════════════════════");
  DebugHelper::debugImportant("🔍 TESTING ALL " + String(NUM_VALVES) + " SENSORS");
  DebugHelper::debugImportant("═══════════════════════════════════════");

  // Test power pin first
  DebugHelper::debug("Power pin: GPIO " + String(RAIN_SENSOR_POWER_PIN));
  pinMode(RAIN_SENSOR_POWER_PIN, OUTPUT);

  // Create summary table
  String summary = "\n📊 SENSOR TEST SUMMARY:\n";
  summary += "Tray | GPIO | Power OFF | Power ON | Status\n";
  summary += "-----|------|-----------|----------|-------\n";

  for (int i = 0; i < NUM_VALVES; i++) {
    // Configure sensor pin
    pinMode(RAIN_SENSOR_PINS[i], INPUT_PULLUP);

    // Read with power OFF
    digitalWrite(VALVE_PINS[i], LOW);
    digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
    delay(50);
    int valueOff = digitalRead(RAIN_SENSOR_PINS[i]);

    // Read with power ON
    // CRITICAL: Sensor needs valve pin HIGH + GPIO 18 HIGH
    digitalWrite(VALVE_PINS[i], HIGH);
    digitalWrite(RAIN_SENSOR_POWER_PIN, HIGH);
    delay(SENSOR_POWER_STABILIZATION);
    int valueOn = digitalRead(RAIN_SENSOR_PINS[i]);
    digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
    digitalWrite(VALVE_PINS[i], LOW);

    // Add to summary
    String tray = String(i + 1);
    while (tray.length() < 4) tray = " " + tray;

    String gpio = String(RAIN_SENSOR_PINS[i]);
    while (gpio.length() < 4) gpio = " " + gpio;

    String off = valueOff == HIGH ? "HIGH(DRY)" : "LOW(WET) ";
    String on = valueOn == LOW ? "LOW(WET)" : "HIGH(DRY)";
    String status = valueOn == LOW ? "💧 WET" : "☀️ DRY";

    // Add warning if power-off reading is wrong
    if (valueOff != HIGH) {
      status += " ⚠️";
    }

    summary += tray + " | " + gpio + " | " + off + " | " + on + "  | " + status + "\n";
  }

  DebugHelper::debug(summary);
  DebugHelper::debugImportant("═══════════════════════════════════════");
  DebugHelper::debugImportant("✓ ALL SENSORS TESTED");
  DebugHelper::debugImportant("═══════════════════════════════════════");
}

// ========== Telegram Notification Queue ==========
// Queue a pre-formatted notification from Core 1 (non-blocking, no network calls)
inline void WateringSystem::queueTelegramNotification(const String& message) {
  String* msg = new String(message);
  if (xQueueSend(notificationQueue, &msg, 0) != pdTRUE) {
    DebugHelper::debug("⚠️ Telegram notification queue full - dropping message");
    delete msg;
  }
}

// Process pending notifications from Core 0 (networkTask) - sends via Telegram
inline void WateringSystem::processPendingNotifications() {
  // Process only one message per network task iteration.
  // This prevents long Telegram outages from starving local web/API handling.
  if (!WiFi.isConnected()) {
    return;
  }

  String* msg = nullptr;
  if (xQueuePeek(notificationQueue, &msg, 0) != pdTRUE) {
    return; // Queue empty
  }

  // Send using dedicated notification path that bypasses DebugHelper cooldown.
  if (!TelegramNotifier::sendNotificationMessage(*msg)) {
    return; // Send failed - leave message at head for retry
  }

  // Remove from queue only after confirmed send
  xQueueReceive(notificationQueue, &msg, 0);
  delete msg;
}

// ============================================
// Include State Machine Implementation
// ============================================
#include "WateringSystemStateMachine.h"

#endif // WATERING_SYSTEM_H

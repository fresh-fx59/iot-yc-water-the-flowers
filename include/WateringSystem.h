#ifndef WATERING_SYSTEM_H
#define WATERING_SYSTEM_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "ValveController.h"
#include "TelegramNotifier.h"
#include "DebugHelper.h"

// External MQTT client (defined in main.cpp)
extern PubSubClient mqttClient;

// Learning data file paths (simplified two-file system)
// To reset learning data: swap the filenames below, old file auto-deletes on boot
const char* LEARNING_DATA_FILE = "/learning_data_v1.8.5.json";      // ACTIVE: Current learning data
const char* LEARNING_DATA_FILE_OLD = "/learning_data.json"; // OLD: Will be deleted on boot

// ============================================
// Time-Based Learning Algorithm Helper Functions
// ============================================
namespace LearningAlgorithm {

    // Calculate water level before watering based on fill duration
    inline float calculateWaterLevelBefore(unsigned long fillDuration, unsigned long baselineFillDuration) {
        if (baselineFillDuration == 0) return 0.0;

        float fillRatio = (float)fillDuration / (float)baselineFillDuration;

        // Water level before = 100% - (fill ratio * 100%)
        // If fillRatio = 1.0 (full fill) → was 0% (empty)
        // If fillRatio = 0.5 (half fill) → was 50% (half full)
        float waterLevelBefore = 100.0f - (fillRatio * 100.0f);
        return (waterLevelBefore < 0.0f) ? 0.0f : waterLevelBefore;
    }

    // Calculate estimated time to empty based on fill ratio and time since last watering
    inline unsigned long calculateEmptyDuration(unsigned long fillDuration, unsigned long baselineFillDuration,
                                                 unsigned long timeSinceLastWatering) {
        if (fillDuration == 0 || baselineFillDuration == 0) return 0;

        float fillRatio = (float)fillDuration / (float)baselineFillDuration;

        // If tray was empty (fillRatio ≈ 1.0)
        if (fillRatio >= LEARNING_EMPTY_THRESHOLD) {
            return timeSinceLastWatering;
        }

        // If tray had water remaining, calculate consumption rate
        // emptyToFullDuration = timeSinceLastWatering / fillRatio
        unsigned long emptyDuration = (unsigned long)((float)timeSinceLastWatering / fillRatio);

        return emptyDuration;
    }

    // Format time duration for display (ms to human-readable)
    inline String formatDuration(unsigned long milliseconds) {
        unsigned long seconds = milliseconds / 1000;
        unsigned long minutes = seconds / 60;
        unsigned long hours = minutes / 60;
        unsigned long days = hours / 24;

        if (days > 0) {
            return String(days) + "d " + String(hours % 24) + "h";
        } else if (hours > 0) {
            return String(hours) + "h " + String(minutes % 60) + "m";
        } else if (minutes > 0) {
            return String(minutes) + "m " + String(seconds % 60) + "s";
        } else {
            return String(seconds) + "." + String((milliseconds % 1000) / 100) + "s";
        }
    }
}

// ============================================
// Session Tracking Struct (for Telegram notifications)
// ============================================
struct WateringSessionData {
    int trayNumber;                   // 1-6 (display format)
    unsigned long startTime;          // millis() when valve opened
    unsigned long endTime;            // millis() when completed
    float duration;                   // duration in seconds
    String status;                    // "OK", "TIMEOUT", "ALREADY_WET", "MANUAL_STOP"
    bool active;                      // is this session being tracked?

    WateringSessionData() : trayNumber(0), startTime(0), endTime(0), duration(0.0), status(""), active(false) {}
};

// ============================================
// WateringSystem Class
// ============================================
class WateringSystem {
private:
    // ========== State Variables ==========
    PumpState pumpState;
    ValveController* valves[NUM_VALVES];
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
    int autoWateringValveIndex;  // Currently auto-watering valve (-1 if none)

public:
    // ========== Constructor ==========
    WateringSystem() :
        pumpState(PUMP_OFF),
        activeValveCount(0),
        lastStatePublish(0),
        lastStateJson(""),
        sequentialMode(false),
        currentSequenceIndex(0),
        sequenceLength(0),
        telegramSessionActive(false),
        sessionTriggerType(""),
        autoWateringValveIndex(-1)
    {
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
    void startSequentialWatering();
    void startSequentialWateringCustom(int* valveIndices, int count);
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
    void clearTimeoutFlag(int valveIndex);

    // Telegram session tracking
    void startTelegramSession(const String& triggerType);
    void recordSessionStart(int valveIndex);
    void recordSessionEnd(int valveIndex, const String& status);
    void endTelegramSession();

    // Watering schedule notification
    void sendWateringSchedule(const String& title);

    // Boot watering decision helpers
    bool isFirstBoot();         // Check if device has no calibration data (needs initial watering)
    bool hasOverdueValves();    // Check if any valve's next watering time is in the past

private:
    // ========== Core Logic ==========
    void processValve(int valveIndex, unsigned long currentTime);
    bool isValveComplete(int valveIndex);
    void startNextInSequence();
    void checkAutoWatering(unsigned long currentTime);

    // ========== Hardware Control ==========
    bool readRainSensor(int valveIndex);
    void openValve(int valveIndex);
    void closeValve(int valveIndex);
    void updatePumpState();

    // ========== Time-Based Learning Algorithm ==========
    void processLearningData(ValveController* valve, unsigned long currentTime);
    void logLearningData(ValveController* valve, float waterLevelBefore, unsigned long emptyDuration);
    void sendScheduleUpdateIfNeeded();  // Helper: sends schedule update unless in sequential mode

    // ========== Utilities ==========
    void publishStateChange(const String& component, const String& state);
};

// ============================================
// Implementation
// ============================================

// ========== Initialization ==========
inline void WateringSystem::init() {
    // Initialize hardware pins
    pinMode(PUMP_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(RAIN_SENSOR_POWER_PIN, OUTPUT);

    digitalWrite(PUMP_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);

    // Initialize valve pins
    String valvePinsInfo = "Valve GPIOs: ";
    for (int i = 0; i < NUM_VALVES; i++) {
        valvePinsInfo += String(i) + "→" + String(VALVE_PINS[i]);
        if (i < NUM_VALVES - 1) valvePinsInfo += ", ";
        pinMode(VALVE_PINS[i], OUTPUT);
        digitalWrite(VALVE_PINS[i], LOW);
    }
    DebugHelper::debugImportant(valvePinsInfo);

    // Initialize rain sensor pins with internal pull-up
    for (int i = 0; i < NUM_VALVES; i++) {
        pinMode(RAIN_SENSOR_PINS[i], INPUT_PULLUP);
    }

    DebugHelper::debugImportant("✓ WateringSystem initialized");
    publishStateChange("system", "initialized");

    // Note: loadLearningData() is called from main.cpp after NTP sync
    // This ensures real time is available for proper timestamp conversion
}

// ========== Persistence Functions ==========
inline bool WateringSystem::saveLearningData() {
    DebugHelper::debug("💾 Saving learning data to flash...");

    // Create JSON document
    StaticJsonDocument<2048> doc;
    JsonArray valvesArray = doc.createNestedArray("valves");

    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* valve = valves[i];
        JsonObject valveObj = valvesArray.createNestedObject();

        valveObj["index"] = valve->valveIndex;
        valveObj["lastWateringCompleteTime"] = (unsigned long)valve->lastWateringCompleteTime;
        valveObj["lastWateringAttemptTime"] = (unsigned long)valve->lastWateringAttemptTime;
        valveObj["emptyToFullDuration"] = (unsigned long)valve->emptyToFullDuration;
        valveObj["baselineFillDuration"] = (unsigned long)valve->baselineFillDuration;
        valveObj["lastFillDuration"] = (unsigned long)valve->lastFillDuration;
        valveObj["previousFillDuration"] = (unsigned long)valve->previousFillDuration;
        valveObj["lastWaterLevelPercent"] = valve->lastWaterLevelPercent;
        valveObj["isCalibrated"] = valve->isCalibrated;
        valveObj["totalWateringCycles"] = valve->totalWateringCycles;
        valveObj["autoWateringEnabled"] = valve->autoWateringEnabled;
        valveObj["intervalMultiplier"] = valve->intervalMultiplier;
    }

    // Save current millis() and real time as reference points
    doc["savedAtMillis"] = millis();
    doc["savedAtRealTime"] = (unsigned long)time(nullptr);

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
        DebugHelper::debugImportant("❌ Failed to parse JSON: " + String(error.c_str()));
        return false;
    }

    // Get saved timestamps
    unsigned long savedAtMillis = doc["savedAtMillis"] | 0;
    unsigned long savedAtRealTime = doc["savedAtRealTime"] | 0;
    unsigned long currentMillis = millis();
    time_t currentRealTime = time(nullptr);

    // Calculate time offset using real time (if available) or millis()
    unsigned long timeOffsetMs = 0;

    if (savedAtRealTime > 0 && currentRealTime > 1000000000) {
        // Use real time - this handles reboots correctly
        if (currentRealTime >= savedAtRealTime) {
            unsigned long elapsedSeconds = currentRealTime - savedAtRealTime;
            timeOffsetMs = elapsedSeconds * 1000UL;
            DebugHelper::debug("  Using real time for offset calculation");
            DebugHelper::debug("  Time since save: " + LearningAlgorithm::formatDuration(timeOffsetMs));
        } else {
            // Clock went backwards - use current millis as base
            DebugHelper::debugImportant("⚠️  Clock went backwards - resetting to current time");
            timeOffsetMs = currentMillis;
        }
    } else if (currentMillis >= savedAtMillis) {
        // Fall back to millis() if no real time available
        timeOffsetMs = currentMillis - savedAtMillis;
        DebugHelper::debug("  Using millis() for offset (no real time available)");
    } else {
        // Reboot detected without real time - use current millis as base
        DebugHelper::debugImportant("⚠️  Reboot detected without real time - using current millis");
        timeOffsetMs = currentMillis;
    }

    // Load valve data
    JsonArray valvesArray = doc["valves"];
    int loadedCount = 0;

    for (JsonObject valveObj : valvesArray) {
        int index = valveObj["index"] | -1;
        if (index < 0 || index >= NUM_VALVES) continue;

        ValveController* valve = valves[index];
        unsigned long savedCompleteTime = valveObj["lastWateringCompleteTime"] | 0;
        unsigned long savedAttemptTime = valveObj["lastWateringAttemptTime"] | 0;

        valve->emptyToFullDuration = valveObj["emptyToFullDuration"] | 0;
        valve->baselineFillDuration = valveObj["baselineFillDuration"] | 0;
        valve->lastFillDuration = valveObj["lastFillDuration"] | 0;
        valve->previousFillDuration = valveObj["previousFillDuration"] | 0;
        valve->lastWaterLevelPercent = valveObj["lastWaterLevelPercent"] | 0.0;
        valve->isCalibrated = valveObj["isCalibrated"] | false;
        valve->totalWateringCycles = valveObj["totalWateringCycles"] | 0;
        valve->autoWateringEnabled = valveObj["autoWateringEnabled"] | true;
        valve->intervalMultiplier = valveObj["intervalMultiplier"] | 1.0;

        // Convert saved timestamps to current millis() epoch
        // New timestamp = current_millis - (time_elapsed_since_save - time_from_save_to_watering)
        // Simplified: New timestamp = current_millis - time_since_watering
        if (savedCompleteTime > 0 && savedAtMillis > 0) {
            // Time from watering to save
            unsigned long timeFromWateringToSave = savedAtMillis - savedCompleteTime;
            // Time from watering to now = time_from_watering_to_save + time_since_save
            unsigned long timeSinceWatering = timeFromWateringToSave + timeOffsetMs;
            // New timestamp in current epoch
            if (currentMillis >= timeSinceWatering) {
                valve->lastWateringCompleteTime = currentMillis - timeSinceWatering;
            } else {
                valve->lastWateringCompleteTime = 0;  // Watering was longer ago than millis can represent
            }
        }

        if (savedAttemptTime > 0 && savedAtMillis > 0) {
            unsigned long timeFromAttemptToSave = savedAtMillis - savedAttemptTime;
            unsigned long timeSinceAttempt = timeFromAttemptToSave + timeOffsetMs;
            if (currentMillis >= timeSinceAttempt) {
                valve->lastWateringAttemptTime = currentMillis - timeSinceAttempt;
            } else {
                valve->lastWateringAttemptTime = 0;
            }
        }

        loadedCount++;
    }

    DebugHelper::debug("✓ Loaded data for " + String(loadedCount) + " valves");
    DebugHelper::debug("  Time offset applied: " + LearningAlgorithm::formatDuration(timeOffsetMs));

    return true;
}

// ========== Main Processing Loop ==========
inline void WateringSystem::processWateringLoop() {
    unsigned long currentTime = millis();

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

// ========== Automatic Watering Check ==========
inline void WateringSystem::checkAutoWatering(unsigned long currentTime) {
    // Check each valve to see if it's time to water automatically
    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* valve = valves[i];

        // Only check valves that are idle and have auto-watering enabled
        if (valve->phase != PHASE_IDLE || !valve->autoWateringEnabled) {
            continue;
        }

        // Check if tray is empty and should be watered
        if (shouldWaterNow(valve, currentTime)) {
            DebugHelper::debugImportant("⏰ AUTO-WATERING TRIGGERED: Valve " + String(i));
            DebugHelper::debug("  Tray is empty - starting automatic watering");

            // Start Telegram session for auto-watering
            startTelegramSession("Auto (Tray " + String(i + 1) + ")");
            autoWateringValveIndex = i;

            // Flush debug buffer before sending notification
            DebugHelper::flushBuffer();

            // Send start notification
            String trayNumber = String(i + 1);
            TelegramNotifier::sendWateringStarted("Auto", trayNumber);

            startWatering(i);
        }
    }
}

// ========== Watering Control ==========
inline void WateringSystem::startWatering(int valveIndex, bool forceWatering) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
        DebugHelper::debugImportant("Invalid valve index: " + String(valveIndex));
        publishStateChange("error", "invalid_valve_index");
        return;
    }

    ValveController* valve = valves[valveIndex];

    if (valve->phase != PHASE_IDLE) {
        DebugHelper::debug("Valve " + String(valveIndex) + " is already active");
        return;
    }

    unsigned long currentTime = millis();

    // Check if we should skip this watering (time-based learning algorithm)
    // ONLY skip for auto-watering, NOT for manual/MQTT commands
    if (!forceWatering && valve->isCalibrated && valve->emptyToFullDuration > 0 && valve->lastWateringCompleteTime > 0) {
        unsigned long timeSinceLastWatering = currentTime - valve->lastWateringCompleteTime;

        if (timeSinceLastWatering < valve->emptyToFullDuration) {
            // Tray not empty yet - skip watering
            float currentWaterLevel = calculateCurrentWaterLevel(valve, currentTime);
            unsigned long timeRemaining = valve->emptyToFullDuration - timeSinceLastWatering;

            DebugHelper::debug("═══════════════════════════════════════");
            DebugHelper::debug("🧠 SMART SKIP: Valve " + String(valveIndex));
            DebugHelper::debug("  Tray not empty yet (water level: ~" + String((int)currentWaterLevel) + "%)");
            DebugHelper::debug("  Time since last watering: " + LearningAlgorithm::formatDuration(timeSinceLastWatering));
            DebugHelper::debug("  Time until empty: " + LearningAlgorithm::formatDuration(timeRemaining));
            DebugHelper::debug("═══════════════════════════════════════");

            publishStateChange("valve" + String(valveIndex), "cycle_skipped_learning");
            return;
        } else {
            DebugHelper::debug("═══════════════════════════════════════");
            DebugHelper::debug("⏰ TIME TO WATER: Valve " + String(valveIndex));
            DebugHelper::debug("  Tray should be empty now (time elapsed: " + LearningAlgorithm::formatDuration(timeSinceLastWatering) + ")");
        }
    }

    // Start watering cycle
    DebugHelper::debug("═══════════════════════════════════════");
    DebugHelper::debug("Starting watering cycle for valve " + String(valveIndex));

    if (valve->isCalibrated) {
        DebugHelper::debug("🧠 Calibrated - Baseline: " + LearningAlgorithm::formatDuration(valve->baselineFillDuration));
        if (valve->emptyToFullDuration > 0) {
            DebugHelper::debug("  Empty time: " + LearningAlgorithm::formatDuration(valve->emptyToFullDuration));
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

    publishStateChange("valve" + String(valveIndex), "cycle_started");

    // Record session start if Telegram tracking is active
    if (telegramSessionActive) {
        recordSessionStart(valveIndex);
    }
}

inline void WateringSystem::stopWatering(int valveIndex) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) return;

    ValveController* valve = valves[valveIndex];
    valve->wateringRequested = false;

    if (valve->phase == PHASE_WATERING) {
        updatePumpState();
    }

    closeValve(valveIndex);
    valve->phase = PHASE_IDLE;
    publishStateChange("valve" + String(valveIndex), "cycle_stopped");
    updatePumpState();
}

inline void WateringSystem::startSequentialWatering() {
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
    startTelegramSession("MQTT");

    // Build tray numbers list
    String trayNumbers;
    if (sequenceLength == NUM_VALVES) {
        trayNumbers = "All";
    } else {
        trayNumbers = "";
        for (int i = 0; i < sequenceLength; i++) {
            trayNumbers += String(sequenceValves[i] + 1);  // Convert to 1-indexed
            if (i < sequenceLength - 1) trayNumbers += ", ";
        }
    }

    // Flush debug buffer before sending notification
    DebugHelper::flushBuffer();

    TelegramNotifier::sendWateringStarted(sessionTriggerType, trayNumbers);

    startNextInSequence();
}

inline void WateringSystem::startSequentialWateringCustom(int* valveIndices, int count) {
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
        if (i < count - 1) valveSeq += ", ";
        sequenceValves[i] = valveIndices[i];
    }
    DebugHelper::debug(valveSeq);

    sequenceLength = count;
    sequentialMode = true;
    currentSequenceIndex = 0;
    publishStateChange("system", "sequential_started");
    startNextInSequence();
}

inline void WateringSystem::stopSequentialWatering() {
    if (!sequentialMode) return;

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
    return valves[valveIndex]->phase == PHASE_IDLE && !valves[valveIndex]->wateringRequested;
}

inline void WateringSystem::startNextInSequence() {
    if (!sequentialMode) return;

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

            // Flush all buffered debug messages before sending completion notification
            DebugHelper::flushBuffer();

            TelegramNotifier::sendWateringComplete(results, resultCount);
            endTelegramSession();

            // Send updated watering schedule after sequential watering completes
            sendWateringSchedule("Updated Schedule");
        }

        return;
    }

    int valveIndex = sequenceValves[currentSequenceIndex];
    DebugHelper::debug("\n→ [Sequence " + String(currentSequenceIndex + 1) + "/" + String(sequenceLength) +
                       "] Starting Valve " + String(valveIndex));

    startWatering(valveIndex, true);  // Force watering - ignore learning algorithm
    currentSequenceIndex++;
}

// ========== Hardware Control ==========
inline bool WateringSystem::readRainSensor(int valveIndex) {
    // Power on sensor
    digitalWrite(RAIN_SENSOR_POWER_PIN, HIGH);
    delay(SENSOR_POWER_STABILIZATION);

    // Read sensor: LOW = wet, HIGH = dry (with pull-up)
    bool sensorValue = digitalRead(RAIN_SENSOR_PINS[valveIndex]);

    // Power off immediately
    digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);

    return (sensorValue == LOW); // LOW = wet/rain detected
}

inline void WateringSystem::openValve(int valveIndex) {
    DebugHelper::debugImportant("🔧 OPENING VALVE " + String(valveIndex));
    DebugHelper::debug("  GPIO Pin: " + String(VALVE_PINS[valveIndex]));
    DebugHelper::debug("  Setting GPIO to HIGH...");

    digitalWrite(VALVE_PINS[valveIndex], HIGH);

    // Read back the pin state to verify
    int pinState = digitalRead(VALVE_PINS[valveIndex]);
    DebugHelper::debugImportant("  GPIO After: " + String(pinState == HIGH ? "HIGH" : "LOW"));

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
    if (activeValveCount > 0) activeValveCount--;

    DebugHelper::debugImportant("🔧 CLOSING VALVE " + String(valveIndex) + " (GPIO " + String(VALVE_PINS[valveIndex]) + ")");
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
        digitalWrite(LED_PIN, HIGH);
        pumpState = PUMP_ON;
        DebugHelper::debugImportant("💧 Pump ON (GPIO " + String(PUMP_PIN) + ")");
        publishStateChange("pump", "on");
    } else if (wateringCount == 0 && pumpState == PUMP_ON) {
        digitalWrite(PUMP_PIN, LOW);
        digitalWrite(LED_PIN, LOW);
        pumpState = PUMP_OFF;
        DebugHelper::debugImportant("💧 Pump OFF (GPIO " + String(PUMP_PIN) + ")");
        publishStateChange("pump", "off");
    }
}

// ========== Helper: Schedule Update ==========
inline void WateringSystem::sendScheduleUpdateIfNeeded() {
    if (!sequentialMode) {
        DebugHelper::debug("📅 Sending updated watering schedule...");
        delay(500);  // Brief delay to ensure WiFi is stable
        sendWateringSchedule("Updated Schedule");
    }
}

// ========== Time-Based Learning Algorithm ==========
inline void WateringSystem::processLearningData(ValveController* valve, unsigned long currentTime) {
    // Algorithm constants (extracted for easy tuning)
    const unsigned long BASE_INTERVAL_MS = 86400000;  // 24 hours
    const float BASELINE_TOLERANCE = 0.95;            // 95% - threshold for "tray not fully empty"
    const long FILL_STABLE_TOLERANCE_MS = 500;        // ±0.5s - threshold for "same fill time"
    const float MIN_INTERVAL_MULTIPLIER = 1.0;        // Never go below 24h
    const float INTERVAL_DOUBLE = 2.0;                // Double when tray already full
    const float INTERVAL_INCREMENT_LARGE = 1.0;       // Large adjustment for coarse search
    const float INTERVAL_DECREMENT_BINARY = 0.5;      // Binary search refinement (decrease)
    const float INTERVAL_INCREMENT_FINE = 0.25;       // Fine-tuning adjustment

    // Skip if timeout occurred
    if (valve->timeoutOccurred) {
        DebugHelper::debug("🧠 Skipping learning - timeout occurred");
        return;
    }

    // CASE 1: Tray already full (sensor wet before pump started)
    // wateringStartTime == 0 means pump never started, rainDetected == true means sensor is wet
    if (valve->wateringStartTime == 0 && valve->rainDetected) {
        DebugHelper::debugImportant("🧠 ADAPTIVE LEARNING: Tray already full");

        // Double the interval (exponential backoff) - tray is consuming water slower than expected
        float oldMultiplier = valve->intervalMultiplier;
        valve->intervalMultiplier *= INTERVAL_DOUBLE;
        valve->emptyToFullDuration = (unsigned long)(BASE_INTERVAL_MS * valve->intervalMultiplier);
        valve->totalWateringCycles++;

        DebugHelper::debugImportant("  Interval: " + String(oldMultiplier, 2) + "x → " +
            String(valve->intervalMultiplier, 2) + "x (doubled)");
        DebugHelper::debugImportant("  Next attempt in: " + LearningAlgorithm::formatDuration(valve->emptyToFullDuration));

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
    DebugHelper::debug("  Fill duration: " + String(fillDuration / 1000.0, 1) + "s");

    // CASE 2A: First successful watering - establish baseline
    if (!valve->isCalibrated) {
        valve->isCalibrated = true;
        valve->baselineFillDuration = fillDuration;
        valve->previousFillDuration = fillDuration;
        valve->intervalMultiplier = MIN_INTERVAL_MULTIPLIER;
        valve->emptyToFullDuration = BASE_INTERVAL_MS; // Start with 24h interval
        valve->lastWateringCompleteTime = currentTime;
        valve->lastWaterLevelPercent = 0.0;

        DebugHelper::debugImportant("  ✨ INITIAL CALIBRATION: " + String(fillDuration / 1000.0, 1) + "s");
        DebugHelper::debug("  Baseline will auto-update when tray is emptier");
        DebugHelper::debug("  Starting interval: 1.0x (24 hours)");

        publishStateChange("valve" + String(valve->valveIndex), "initial_calibration");
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
        // Fill < 95% of baseline - tray not fully empty, need longer interval
        valve->intervalMultiplier += INTERVAL_INCREMENT_LARGE;
        DebugHelper::debugImportant("  ⬆️  Fill < baseline → Interval: " + String(oldMultiplier, 2) +
            "x → " + String(valve->intervalMultiplier, 2) + "x (+" + String(INTERVAL_INCREMENT_LARGE, 1) + ")");
    }
    else if (fillDuration > valve->baselineFillDuration) {
        // Fill > baseline - tray was emptier than ever! Update baseline AND increase interval
        valve->baselineFillDuration = fillDuration;
        valve->intervalMultiplier += INTERVAL_INCREMENT_LARGE;
        DebugHelper::debugImportant("  ✨ NEW BASELINE: " + String(fillSeconds, 1) + "s");
        DebugHelper::debugImportant("  ⬆️  Interval: " + String(oldMultiplier, 2) +
            "x → " + String(valve->intervalMultiplier, 2) + "x (+" + String(INTERVAL_INCREMENT_LARGE, 1) + ")");
    }
    else {
        // Fill ≈ baseline (within 5%) - fine-tuning phase
        long fillDiff = (long)(fillDuration - valve->previousFillDuration);

        if (abs(fillDiff) < FILL_STABLE_TOLERANCE_MS) {
            // Same as last time (±0.5s) - interval might be too long, decrease for binary search
            valve->intervalMultiplier -= INTERVAL_DECREMENT_BINARY;
            if (valve->intervalMultiplier < MIN_INTERVAL_MULTIPLIER) {
                valve->intervalMultiplier = MIN_INTERVAL_MULTIPLIER; // Never go below 24h
            }
            DebugHelper::debugImportant("  🎯 Fill stable → Interval: " + String(oldMultiplier, 2) +
                "x → " + String(valve->intervalMultiplier, 2) + "x (-" + String(INTERVAL_DECREMENT_BINARY, 1) + ")");

            // Check if we've found optimal (stable baseline for 2+ cycles)
            if (valve->totalWateringCycles > 2 && fillDiff == 0) {
                DebugHelper::debugImportant("  ✅ OPTIMAL INTERVAL FOUND: " +
                    String(valve->intervalMultiplier, 2) + "x");
            }
        }
        else if (fillDuration < valve->previousFillDuration) {
            // Fill decreased from last time - interval was too long, fine-tune upward
            valve->intervalMultiplier += INTERVAL_INCREMENT_FINE;
            DebugHelper::debugImportant("  ⬆️  Fill decreased → Interval: " + String(oldMultiplier, 2) +
                "x → " + String(valve->intervalMultiplier, 2) + "x (+" + String(INTERVAL_INCREMENT_FINE, 2) + ")");
        }
        else {
            // Fill increased from last time - can try longer interval
            valve->intervalMultiplier += INTERVAL_INCREMENT_FINE;
            DebugHelper::debugImportant("  ⬆️  Fill increased → Interval: " + String(oldMultiplier, 2) +
                "x → " + String(valve->intervalMultiplier, 2) + "x (+" + String(INTERVAL_INCREMENT_FINE, 2) + ")");
        }
    }

    // Update state
    valve->previousFillDuration = fillDuration;
    valve->emptyToFullDuration = (unsigned long)(BASE_INTERVAL_MS * valve->intervalMultiplier);
    valve->lastWateringCompleteTime = currentTime;

    // Calculate water level before this watering (for logging)
    float waterLevelBefore = LearningAlgorithm::calculateWaterLevelBefore(fillDuration, valve->baselineFillDuration);
    valve->lastWaterLevelPercent = waterLevelBefore;

    DebugHelper::debugImportant("  ⏰ Next watering in: " +
        LearningAlgorithm::formatDuration(valve->emptyToFullDuration) +
        " (" + String(valve->intervalMultiplier, 2) + "x)");
    DebugHelper::debug("  Water level before: " + String((int)waterLevelBefore) + "% (" +
        String(getTrayState(waterLevelBefore)) + ")");
    DebugHelper::debug("  Total cycles: " + String(valve->totalWateringCycles));

    saveLearningData();
    sendScheduleUpdateIfNeeded();
}

inline void WateringSystem::logLearningData(ValveController* valve, float waterLevelBefore, unsigned long emptyDuration) {
    DebugHelper::debug("  Baseline fill: " + LearningAlgorithm::formatDuration(valve->baselineFillDuration));
    DebugHelper::debug("  Current fill: " + LearningAlgorithm::formatDuration(valve->lastFillDuration));

    // Check if baseline was just updated
    if (valve->lastFillDuration >= valve->baselineFillDuration && valve->totalWateringCycles > 1) {
        DebugHelper::debug("  ✨ Baseline updated - tray was emptier than before");
    }

    DebugHelper::debug("  Water level before: " + String((int)waterLevelBefore) + "%");

    const char* state = getTrayState(waterLevelBefore);
    DebugHelper::debug("  Tray state was: " + String(state));

    if (emptyDuration > 0) {
        DebugHelper::debug("  Estimated empty time: " + LearningAlgorithm::formatDuration(emptyDuration));
        DebugHelper::debug("  Learning cycles: " + String(valve->totalWateringCycles));

        if (valve->autoWateringEnabled) {
            DebugHelper::debug("  ⏰ Auto-watering enabled - will water when empty");
        } else {
            DebugHelper::debug("  ⚠️  Auto-watering disabled - manual watering only");
        }
    } else {
        DebugHelper::debug("  ⚠️  Not enough data for consumption estimate yet");
    }

    String learningMsg = "{\"valve\":" + String(valve->valveIndex) +
                       ",\"fillDuration\":" + String(valve->lastFillDuration) +
                       ",\"baseline\":" + String(valve->baselineFillDuration) +
                       ",\"waterLevelBefore\":" + String((int)waterLevelBefore) +
                       ",\"emptyDuration\":" + String(emptyDuration) + "}";
    publishStateChange("learning", learningMsg);
}

inline void WateringSystem::resetCalibration(int valveIndex) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) return;

    ValveController* valve = valves[valveIndex];
    valve->isCalibrated = false;
    valve->baselineFillDuration = 0;
    valve->lastFillDuration = 0;
    valve->emptyToFullDuration = 0;
    valve->lastWateringCompleteTime = 0;
    valve->lastWaterLevelPercent = 0.0;
    valve->totalWateringCycles = 0;

    DebugHelper::debug("═══════════════════════════════════════");
    DebugHelper::debug("🔄 CALIBRATION RESET: Valve " + String(valveIndex));
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
        valves[i]->emptyToFullDuration = 0;
        valves[i]->lastWateringCompleteTime = 0;
        valves[i]->lastWaterLevelPercent = 0.0;
        valves[i]->totalWateringCycles = 0;
    }
    DebugHelper::debug("  All valves reset to uncalibrated state");
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
        ValveController* valve = valves[i];
        DebugHelper::debug("\n📊 Valve " + String(i) + ":");

        if (valve->isCalibrated) {
            DebugHelper::debug("  Status: ✓ Calibrated");
            DebugHelper::debug("  Baseline fill: " + LearningAlgorithm::formatDuration(valve->baselineFillDuration));
            DebugHelper::debug("  Last fill: " + LearningAlgorithm::formatDuration(valve->lastFillDuration));
            DebugHelper::debug("  Total cycles: " + String(valve->totalWateringCycles));

            if (valve->emptyToFullDuration > 0) {
                DebugHelper::debug("  Empty-to-full time: " + LearningAlgorithm::formatDuration(valve->emptyToFullDuration));

                // Calculate current water level
                float currentWaterLevel = calculateCurrentWaterLevel(valve, currentTime);
                DebugHelper::debug("  Current water level: ~" + String((int)currentWaterLevel) + "% (" + String(getTrayState(currentWaterLevel)) + ")");

                // Time until empty
                if (currentWaterLevel > 0) {
                    unsigned long timeSinceWatering = currentTime - valve->lastWateringCompleteTime;
                    unsigned long timeRemaining = valve->emptyToFullDuration - timeSinceWatering;
                    DebugHelper::debug("  Time until empty: ~" + LearningAlgorithm::formatDuration(timeRemaining));
                } else {
                    DebugHelper::debug("  Time until empty: Now (should water!)");
                }
            } else {
                DebugHelper::debug("  Empty-to-full time: Unknown (need more data)");
            }

            DebugHelper::debug("  Auto-watering: " + String(valve->autoWateringEnabled ? "Enabled ✓" : "Disabled ✗"));
        } else {
            DebugHelper::debug("  Status: ⚠️  Not calibrated");
            DebugHelper::debug("  Action: Run first watering to establish baseline");
            DebugHelper::debug("  Auto-watering: " + String(valve->autoWateringEnabled ? "Enabled ✓" : "Disabled ✗"));
        }
    }
    DebugHelper::debug("\n═══════════════════════════════════════");
}

inline void WateringSystem::setAutoWatering(int valveIndex, bool enabled) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) return;

    valves[valveIndex]->autoWateringEnabled = enabled;
    DebugHelper::debug("⏰ Valve " + String(valveIndex) + " auto-watering: " + String(enabled ? "ENABLED" : "DISABLED"));
    publishStateChange("valve" + String(valveIndex), enabled ? "auto_enabled" : "auto_disabled");

    // Save to flash
    saveLearningData();
}

inline void WateringSystem::setAllAutoWatering(bool enabled) {
    DebugHelper::debug("═══════════════════════════════════════");
    DebugHelper::debug("⏰ SET ALL AUTO-WATERING: " + String(enabled ? "ENABLED" : "DISABLED"));
    for (int i = 0; i < NUM_VALVES; i++) {
        valves[i]->autoWateringEnabled = enabled;
    }
    DebugHelper::debug("═══════════════════════════════════════");
    publishStateChange("system", enabled ? "all_auto_enabled" : "all_auto_disabled");

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
inline void WateringSystem::startTelegramSession(const String& triggerType) {
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
    if (!telegramSessionActive || valveIndex < 0 || valveIndex >= NUM_VALVES) return;

    sessionData[valveIndex].active = true;
    sessionData[valveIndex].trayNumber = valveIndex + 1;  // Convert to 1-indexed
    sessionData[valveIndex].startTime = millis();
    sessionData[valveIndex].status = "IN_PROGRESS";

    DebugHelper::debug("📱 Session tracking: Tray " + String(valveIndex + 1) + " started");
}

inline void WateringSystem::recordSessionEnd(int valveIndex, const String& status) {
    if (!telegramSessionActive || valveIndex < 0 || valveIndex >= NUM_VALVES) return;
    if (!sessionData[valveIndex].active) return;

    sessionData[valveIndex].endTime = millis();

    // CORRECT: Calculate duration from valve OPEN to valve CLOSE (full cycle time)
    // Use valveOpenTime instead of sessionData startTime for accurate measurement
    unsigned long startTime = valves[valveIndex]->valveOpenTime;
    if (startTime == 0) {
        // Fallback to session start if valve time not set
        startTime = sessionData[valveIndex].startTime;
    }
    sessionData[valveIndex].duration = (sessionData[valveIndex].endTime - startTime) / 1000.0;
    sessionData[valveIndex].status = status;

    DebugHelper::debug("📱 Session tracking: Tray " + String(valveIndex + 1) +
                        " ended - Status: " + status +
                        ", Duration: " + String(sessionData[valveIndex].duration, 1) + "s (valve open to close)");
}

inline void WateringSystem::endTelegramSession() {
    if (!telegramSessionActive) return;

    DebugHelper::debug("📱 Telegram session ended - preparing completion report");
    telegramSessionActive = false;
}

// ========== Watering Schedule Notification ==========
inline void WateringSystem::sendWateringSchedule(const String& title) {
    if (!WiFi.isConnected()) {
        DebugHelper::debugImportant("❌ Cannot send schedule: WiFi not connected - will retry on next watering");
        return;
    }

    // Get current time
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        DebugHelper::debugImportant("❌ Cannot send schedule: Time not synced - will retry on next watering");
        return;
    }

    // Flush debug buffer before sending schedule notification
    DebugHelper::flushBuffer();

    unsigned long currentTime = millis();
    time_t now = time(nullptr);

    // Build schedule data for all valves (4 columns: tray, planned, duration, cycle)
    String scheduleData[NUM_VALVES][4];

    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* valve = valves[i];

        // Column 0: Tray number (1-indexed)
        scheduleData[i][0] = String(i + 1);

        // Column 2: Duration (baseline fill time in seconds)
        if (valve->baselineFillDuration > 0) {
            float durationSec = valve->baselineFillDuration / 1000.0;
            scheduleData[i][2] = String(durationSec, 1);
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
            // Not calibrated but has temporary 24h retry duration (tray was found full)
            unsigned long timeSinceWatering = currentTime - valve->lastWateringCompleteTime;

            if (timeSinceWatering >= valve->emptyToFullDuration) {
                scheduleData[i][1] = "Now (retry)";
            } else {
                unsigned long timeUntilRetry = valve->emptyToFullDuration - timeSinceWatering;
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
            unsigned long referenceTime = valve->lastWateringAttemptTime;
            if (referenceTime == 0) referenceTime = valve->lastWateringCompleteTime;

            if (referenceTime > 0) {
                unsigned long timeSinceAttempt = currentTime - referenceTime;
                if (timeSinceAttempt >= AUTO_WATERING_MIN_INTERVAL_MS) {
                    scheduleData[i][1] = "Now (learn)";
                } else {
                    unsigned long timeUntilNext = AUTO_WATERING_MIN_INTERVAL_MS - timeSinceAttempt;
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
            unsigned long timeSinceWatering = currentTime - valve->lastWateringCompleteTime;

            if (timeSinceWatering >= valve->emptyToFullDuration) {
                // Already due for watering
                scheduleData[i][1] = "Now";
            } else {
                // Calculate future watering time
                unsigned long timeUntilWatering = valve->emptyToFullDuration - timeSinceWatering;
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

    TelegramNotifier::sendWateringSchedule(scheduleData, NUM_VALVES, title);
}

// ========== Boot Watering Decision Helpers ==========
inline bool WateringSystem::isFirstBoot() {
    // Check if all valves are uncalibrated (fresh device, no learning data)
    for (int i = 0; i < NUM_VALVES; i++) {
        if (valves[i]->isCalibrated) {
            return false;  // At least one valve has data
        }
    }
    return true;  // All valves uncalibrated = first boot
}

inline bool WateringSystem::hasOverdueValves() {
    unsigned long currentTime = millis();

    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* valve = valves[i];

        // Skip if auto-watering disabled
        if (!valve->autoWateringEnabled) {
            continue;
        }

        // Skip if not calibrated
        if (!valve->isCalibrated) {
            continue;
        }

        // CASE 1: Valve has learning data but lastWateringCompleteTime is 0
        // This happens when outage was longer than millis() can represent
        // If calibrated with emptyToFullDuration but no timestamp, watering was DEFINITELY overdue
        if (valve->emptyToFullDuration > 0 && valve->lastWateringCompleteTime == 0) {
            DebugHelper::debugImportant("Valve " + String(i) + " is overdue - timestamp too old to represent (outage > " +
                LearningAlgorithm::formatDuration(currentTime) + ")");
            return true;
        }

        // CASE 2: Normal case - calculate next watering time from timestamp
        if (valve->emptyToFullDuration > 0 && valve->lastWateringCompleteTime > 0) {
            unsigned long nextWateringTime = valve->lastWateringCompleteTime + valve->emptyToFullDuration;

            // Check if next watering time is in the past (overdue)
            if (currentTime >= nextWateringTime) {
                DebugHelper::debug("Valve " + String(i) + " is overdue - scheduled for " +
                    LearningAlgorithm::formatDuration(currentTime - nextWateringTime) + " ago");
                return true;
            }
        }
    }

    return false;  // No overdue valves
}

// ============================================
// Include State Machine Implementation
// ============================================
#include "WateringSystemStateMachine.h"

#endif // WATERING_SYSTEM_H

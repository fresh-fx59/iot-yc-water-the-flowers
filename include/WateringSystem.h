#ifndef WATERING_SYSTEM_H
#define WATERING_SYSTEM_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "ValveController.h"
#include "TelegramNotifier.h"
#include "DebugHelper.h"

// External MQTT client (defined in main.cpp)
extern PubSubClient mqttClient;

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
        sessionTriggerType("")
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

    // Load learning data from flash
    if (!loadLearningData()) {
        DEBUG_SERIAL.println("⚠️  No saved learning data found or load failed");
    }
}

// ========== Persistence Functions ==========
inline bool WateringSystem::saveLearningData() {
    DEBUG_SERIAL.println("💾 Saving learning data to flash...");

    // Create JSON document
    StaticJsonDocument<2048> doc;
    JsonArray valvesArray = doc.createNestedArray("valves");

    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* valve = valves[i];
        JsonObject valveObj = valvesArray.createNestedObject();

        valveObj["index"] = valve->valveIndex;
        valveObj["lastWateringCompleteTime"] = (unsigned long)valve->lastWateringCompleteTime;
        valveObj["emptyToFullDuration"] = (unsigned long)valve->emptyToFullDuration;
        valveObj["baselineFillDuration"] = (unsigned long)valve->baselineFillDuration;
        valveObj["lastFillDuration"] = (unsigned long)valve->lastFillDuration;
        valveObj["lastWaterLevelPercent"] = valve->lastWaterLevelPercent;
        valveObj["isCalibrated"] = valve->isCalibrated;
        valveObj["totalWateringCycles"] = valve->totalWateringCycles;
        valveObj["autoWateringEnabled"] = valve->autoWateringEnabled;
    }

    // Save current millis() as reference point
    doc["savedAtMillis"] = millis();

    // Open file for writing
    File file = LittleFS.open("/learning_data.json", "w");
    if (!file) {
        DEBUG_SERIAL.println("❌ Failed to open file for writing");
        return false;
    }

    // Serialize JSON to file
    if (serializeJson(doc, file) == 0) {
        DEBUG_SERIAL.println("❌ Failed to write JSON to file");
        file.close();
        return false;
    }

    file.close();
    DEBUG_SERIAL.println("✓ Learning data saved successfully");
    return true;
}

inline bool WateringSystem::loadLearningData() {
    DEBUG_SERIAL.println("📂 Loading learning data from flash...");

    // Check if file exists
    if (!LittleFS.exists("/learning_data.json")) {
        DEBUG_SERIAL.println("  No learning data file found");
        return false;
    }

    // Open file for reading
    File file = LittleFS.open("/learning_data.json", "r");
    if (!file) {
        DEBUG_SERIAL.println("❌ Failed to open file for reading");
        return false;
    }

    // Parse JSON
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_SERIAL.println("❌ Failed to parse JSON: " + String(error.c_str()));
        return false;
    }

    // Get time offset (how long since data was saved)
    unsigned long savedAtMillis = doc["savedAtMillis"] | 0;
    unsigned long currentMillis = millis();
    unsigned long timeOffset = (currentMillis >= savedAtMillis) ? (currentMillis - savedAtMillis) : 0;

    // Handle millis() overflow: if current < saved, we've overflowed (happens every ~49 days)
    if (currentMillis < savedAtMillis) {
        DEBUG_SERIAL.println("⚠️  millis() overflow detected - resetting timestamps");
        timeOffset = 0;  // Reset time offset
    }

    // Load valve data
    JsonArray valvesArray = doc["valves"];
    int loadedCount = 0;

    for (JsonObject valveObj : valvesArray) {
        int index = valveObj["index"] | -1;
        if (index < 0 || index >= NUM_VALVES) continue;

        ValveController* valve = valves[index];
        valve->lastWateringCompleteTime = valveObj["lastWateringCompleteTime"] | 0;
        valve->emptyToFullDuration = valveObj["emptyToFullDuration"] | 0;
        valve->baselineFillDuration = valveObj["baselineFillDuration"] | 0;
        valve->lastFillDuration = valveObj["lastFillDuration"] | 0;
        valve->lastWaterLevelPercent = valveObj["lastWaterLevelPercent"] | 0.0;
        valve->isCalibrated = valveObj["isCalibrated"] | false;
        valve->totalWateringCycles = valveObj["totalWateringCycles"] | 0;
        valve->autoWateringEnabled = valveObj["autoWateringEnabled"] | true;

        // Adjust lastWateringCompleteTime for time offset
        if (valve->lastWateringCompleteTime > 0 && timeOffset > 0) {
            valve->lastWateringCompleteTime += timeOffset;
        }

        loadedCount++;
    }

    DEBUG_SERIAL.println("✓ Loaded data for " + String(loadedCount) + " valves");
    DEBUG_SERIAL.println("  Time offset applied: " + LearningAlgorithm::formatDuration(timeOffset));

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
            DEBUG_SERIAL.println("\n⏰ AUTO-WATERING TRIGGERED: Valve " + String(i));
            DEBUG_SERIAL.println("  Tray is empty - starting automatic watering");
            startWatering(i);
        }
    }
}

// ========== Watering Control ==========
inline void WateringSystem::startWatering(int valveIndex, bool forceWatering) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
        DEBUG_SERIAL.println("Invalid valve index: " + String(valveIndex));
        publishStateChange("error", "invalid_valve_index");
        return;
    }

    ValveController* valve = valves[valveIndex];

    if (valve->phase != PHASE_IDLE) {
        DEBUG_SERIAL.println("Valve " + String(valveIndex) + " is already active");
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

            DEBUG_SERIAL.println("═══════════════════════════════════════");
            DEBUG_SERIAL.println("🧠 SMART SKIP: Valve " + String(valveIndex));
            DEBUG_SERIAL.println("  Tray not empty yet (water level: ~" + String((int)currentWaterLevel) + "%)");
            DEBUG_SERIAL.println("  Time since last watering: " + LearningAlgorithm::formatDuration(timeSinceLastWatering));
            DEBUG_SERIAL.println("  Time until empty: " + LearningAlgorithm::formatDuration(timeRemaining));
            DEBUG_SERIAL.println("═══════════════════════════════════════");

            publishStateChange("valve" + String(valveIndex), "cycle_skipped_learning");
            return;
        } else {
            DEBUG_SERIAL.println("═══════════════════════════════════════");
            DEBUG_SERIAL.println("⏰ TIME TO WATER: Valve " + String(valveIndex));
            DEBUG_SERIAL.println("  Tray should be empty now (time elapsed: " + LearningAlgorithm::formatDuration(timeSinceLastWatering) + ")");
        }
    }

    // Start watering cycle
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    DEBUG_SERIAL.println("Starting watering cycle for valve " + String(valveIndex));

    if (valve->isCalibrated) {
        DEBUG_SERIAL.println("🧠 Calibrated - Baseline: " + LearningAlgorithm::formatDuration(valve->baselineFillDuration));
        if (valve->emptyToFullDuration > 0) {
            DEBUG_SERIAL.println("  Empty time: " + LearningAlgorithm::formatDuration(valve->emptyToFullDuration));
        }
    } else {
        DEBUG_SERIAL.println("🎯 First watering - Establishing baseline");
    }

    DEBUG_SERIAL.println("Step 1: Opening valve (sensor needs water flow)...");
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
        DEBUG_SERIAL.println("Sequential watering already in progress");
        return;
    }

    DEBUG_SERIAL.println("\n╔═══════════════════════════════════════════╗");
    DEBUG_SERIAL.println("║  SEQUENTIAL WATERING STARTED (ALL VALVES) ║");
    DEBUG_SERIAL.println("╚═══════════════════════════════════════════╝");

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

    TelegramNotifier::sendWateringStarted(sessionTriggerType, trayNumbers);

    startNextInSequence();
}

inline void WateringSystem::startSequentialWateringCustom(int* valveIndices, int count) {
    if (sequentialMode) {
        DEBUG_SERIAL.println("Sequential watering already in progress");
        return;
    }

    if (count == 0 || count > NUM_VALVES) {
        DEBUG_SERIAL.println("Invalid valve count for sequential watering");
        return;
    }

    DEBUG_SERIAL.println("\n╔═══════════════════════════════════════════╗");
    DEBUG_SERIAL.println("║  SEQUENTIAL WATERING STARTED              ║");
    DEBUG_SERIAL.println("╚═══════════════════════════════════════════╝");
    DEBUG_SERIAL.print("Valve sequence: ");
    for (int i = 0; i < count; i++) {
        DEBUG_SERIAL.print(valveIndices[i]);
        if (i < count - 1) DEBUG_SERIAL.print(", ");
        sequenceValves[i] = valveIndices[i];
    }
    DEBUG_SERIAL.println();

    sequenceLength = count;
    sequentialMode = true;
    currentSequenceIndex = 0;
    publishStateChange("system", "sequential_started");
    startNextInSequence();
}

inline void WateringSystem::stopSequentialWatering() {
    if (!sequentialMode) return;

    DEBUG_SERIAL.println("\n⚠️  SEQUENTIAL WATERING STOPPED");
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
        DEBUG_SERIAL.println("\n╔═══════════════════════════════════════════╗");
        DEBUG_SERIAL.println("║  SEQUENTIAL WATERING COMPLETE ✓           ║");
        DEBUG_SERIAL.println("╚═══════════════════════════════════════════╝");
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

            TelegramNotifier::sendWateringComplete(results, resultCount);
            endTelegramSession();
        }

        return;
    }

    int valveIndex = sequenceValves[currentSequenceIndex];
    DEBUG_SERIAL.println("\n→ [Sequence " + String(currentSequenceIndex + 1) + "/" + String(sequenceLength) +
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

// ========== Time-Based Learning Algorithm ==========
inline void WateringSystem::processLearningData(ValveController* valve, unsigned long currentTime) {
    if (valve->wateringStartTime == 0 || valve->timeoutOccurred || !valve->rainDetected) {
        return; // Only process successful waterings
    }

    unsigned long fillDuration = currentTime - valve->wateringStartTime;
    unsigned long timeSinceLastWatering = 0;

    if (valve->lastWateringCompleteTime > 0) {
        timeSinceLastWatering = valve->wateringStartTime - valve->lastWateringCompleteTime;
    }

    valve->lastFillDuration = fillDuration;
    valve->totalWateringCycles++;

    DEBUG_SERIAL.println("🧠 TIME-BASED LEARNING:");
    DEBUG_SERIAL.println("  Fill duration: " + LearningAlgorithm::formatDuration(fillDuration));

    // Update baseline if this fill was longer (tray was emptier)
    bool baselineUpdated = false;
    if (fillDuration >= valve->baselineFillDuration || valve->baselineFillDuration == 0) {
        valve->baselineFillDuration = fillDuration;
        baselineUpdated = true;
        DEBUG_SERIAL.println("  📏 Baseline updated: " + LearningAlgorithm::formatDuration(fillDuration));
    }

    if (!valve->isCalibrated) {
        // First successful watering - record initial data
        valve->isCalibrated = true;
        valve->lastWateringCompleteTime = currentTime;
        valve->emptyToFullDuration = 0;  // Unknown until we have consumption data
        valve->lastWaterLevelPercent = 0.0;

        DEBUG_SERIAL.println("  🎯 INITIAL CALIBRATION: " + LearningAlgorithm::formatDuration(fillDuration));
        DEBUG_SERIAL.println("  Note: Tray may not have been empty");
        DEBUG_SERIAL.println("  Baseline will auto-update when tray is emptier");
        DEBUG_SERIAL.println("  Next watering will start measuring consumption");
        publishStateChange("valve" + String(valve->valveIndex), "initial_calibration");

        // Save to flash
        saveLearningData();
    } else {
        // Calculate water level before this watering
        float waterLevelBefore = LearningAlgorithm::calculateWaterLevelBefore(fillDuration, valve->baselineFillDuration);
        valve->lastWaterLevelPercent = waterLevelBefore;

        // Only calculate consumption if we have valid time data
        unsigned long emptyDuration = 0;
        if (timeSinceLastWatering > 0 && fillDuration > 0) {
            emptyDuration = LearningAlgorithm::calculateEmptyDuration(
                fillDuration, valve->baselineFillDuration, timeSinceLastWatering);

            // Update emptyToFullDuration with running average for stability
            if (valve->emptyToFullDuration == 0) {
                valve->emptyToFullDuration = emptyDuration;
            } else {
                // Weighted average: 70% old value, 30% new value (smoothing)
                valve->emptyToFullDuration = (valve->emptyToFullDuration * 7 + emptyDuration * 3) / 10;
            }
        }

        valve->lastWateringCompleteTime = currentTime;

        logLearningData(valve, waterLevelBefore, valve->emptyToFullDuration);

        // Save to flash
        saveLearningData();
    }
}

inline void WateringSystem::logLearningData(ValveController* valve, float waterLevelBefore, unsigned long emptyDuration) {
    DEBUG_SERIAL.println("  Baseline fill: " + LearningAlgorithm::formatDuration(valve->baselineFillDuration));
    DEBUG_SERIAL.println("  Current fill: " + LearningAlgorithm::formatDuration(valve->lastFillDuration));

    // Check if baseline was just updated
    if (valve->lastFillDuration >= valve->baselineFillDuration && valve->totalWateringCycles > 1) {
        DEBUG_SERIAL.println("  ✨ Baseline updated - tray was emptier than before");
    }

    DEBUG_SERIAL.println("  Water level before: " + String((int)waterLevelBefore) + "%");

    const char* state = getTrayState(waterLevelBefore);
    DEBUG_SERIAL.println("  Tray state was: " + String(state));

    if (emptyDuration > 0) {
        DEBUG_SERIAL.println("  Estimated empty time: " + LearningAlgorithm::formatDuration(emptyDuration));
        DEBUG_SERIAL.println("  Learning cycles: " + String(valve->totalWateringCycles));

        if (valve->autoWateringEnabled) {
            DEBUG_SERIAL.println("  ⏰ Auto-watering enabled - will water when empty");
        } else {
            DEBUG_SERIAL.println("  ⚠️  Auto-watering disabled - manual watering only");
        }
    } else {
        DEBUG_SERIAL.println("  ⚠️  Not enough data for consumption estimate yet");
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

    DEBUG_SERIAL.println("═══════════════════════════════════════");
    DEBUG_SERIAL.println("🔄 CALIBRATION RESET: Valve " + String(valveIndex));
    DEBUG_SERIAL.println("  Next watering will establish new baseline");
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    publishStateChange("valve" + String(valveIndex), "calibration_reset");

    // Save to flash
    saveLearningData();
}

inline void WateringSystem::resetAllCalibrations() {
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    DEBUG_SERIAL.println("🔄 RESET ALL CALIBRATIONS");
    for (int i = 0; i < NUM_VALVES; i++) {
        valves[i]->isCalibrated = false;
        valves[i]->baselineFillDuration = 0;
        valves[i]->lastFillDuration = 0;
        valves[i]->emptyToFullDuration = 0;
        valves[i]->lastWateringCompleteTime = 0;
        valves[i]->lastWaterLevelPercent = 0.0;
        valves[i]->totalWateringCycles = 0;
    }
    DEBUG_SERIAL.println("  All valves reset to uncalibrated state");
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    publishStateChange("system", "all_calibrations_reset");

    // Save to flash
    saveLearningData();
}

inline void WateringSystem::printLearningStatus() {
    DEBUG_SERIAL.println("\n╔═══════════════════════════════════════════╗");
    DEBUG_SERIAL.println("║    TIME-BASED LEARNING SYSTEM STATUS      ║");
    DEBUG_SERIAL.println("╚═══════════════════════════════════════════╝");

    unsigned long currentTime = millis();

    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* valve = valves[i];
        DEBUG_SERIAL.println("\n📊 Valve " + String(i) + ":");

        if (valve->isCalibrated) {
            DEBUG_SERIAL.println("  Status: ✓ Calibrated");
            DEBUG_SERIAL.println("  Baseline fill: " + LearningAlgorithm::formatDuration(valve->baselineFillDuration));
            DEBUG_SERIAL.println("  Last fill: " + LearningAlgorithm::formatDuration(valve->lastFillDuration));
            DEBUG_SERIAL.println("  Total cycles: " + String(valve->totalWateringCycles));

            if (valve->emptyToFullDuration > 0) {
                DEBUG_SERIAL.println("  Empty-to-full time: " + LearningAlgorithm::formatDuration(valve->emptyToFullDuration));

                // Calculate current water level
                float currentWaterLevel = calculateCurrentWaterLevel(valve, currentTime);
                DEBUG_SERIAL.println("  Current water level: ~" + String((int)currentWaterLevel) + "% (" + String(getTrayState(currentWaterLevel)) + ")");

                // Time until empty
                if (currentWaterLevel > 0) {
                    unsigned long timeSinceWatering = currentTime - valve->lastWateringCompleteTime;
                    unsigned long timeRemaining = valve->emptyToFullDuration - timeSinceWatering;
                    DEBUG_SERIAL.println("  Time until empty: ~" + LearningAlgorithm::formatDuration(timeRemaining));
                } else {
                    DEBUG_SERIAL.println("  Time until empty: Now (should water!)");
                }
            } else {
                DEBUG_SERIAL.println("  Empty-to-full time: Unknown (need more data)");
            }

            DEBUG_SERIAL.println("  Auto-watering: " + String(valve->autoWateringEnabled ? "Enabled ✓" : "Disabled ✗"));
        } else {
            DEBUG_SERIAL.println("  Status: ⚠️  Not calibrated");
            DEBUG_SERIAL.println("  Action: Run first watering to establish baseline");
            DEBUG_SERIAL.println("  Auto-watering: " + String(valve->autoWateringEnabled ? "Enabled ✓" : "Disabled ✗"));
        }
    }
    DEBUG_SERIAL.println("\n═══════════════════════════════════════");
}

inline void WateringSystem::setAutoWatering(int valveIndex, bool enabled) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) return;

    valves[valveIndex]->autoWateringEnabled = enabled;
    DEBUG_SERIAL.println("⏰ Valve " + String(valveIndex) + " auto-watering: " + String(enabled ? "ENABLED" : "DISABLED"));
    publishStateChange("valve" + String(valveIndex), enabled ? "auto_enabled" : "auto_disabled");

    // Save to flash
    saveLearningData();
}

inline void WateringSystem::setAllAutoWatering(bool enabled) {
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    DEBUG_SERIAL.println("⏰ SET ALL AUTO-WATERING: " + String(enabled ? "ENABLED" : "DISABLED"));
    for (int i = 0; i < NUM_VALVES; i++) {
        valves[i]->autoWateringEnabled = enabled;
    }
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    publishStateChange("system", enabled ? "all_auto_enabled" : "all_auto_disabled");

    // Save to flash
    saveLearningData();
}

inline void WateringSystem::clearTimeoutFlag(int valveIndex) {
    if (valveIndex >= 0 && valveIndex < NUM_VALVES) {
        valves[valveIndex]->timeoutOccurred = false;
        DEBUG_SERIAL.println("Timeout flag cleared for valve " + String(valveIndex));
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

    DEBUG_SERIAL.println("📱 Telegram session started - Trigger: " + triggerType);
}

inline void WateringSystem::recordSessionStart(int valveIndex) {
    if (!telegramSessionActive || valveIndex < 0 || valveIndex >= NUM_VALVES) return;

    sessionData[valveIndex].active = true;
    sessionData[valveIndex].trayNumber = valveIndex + 1;  // Convert to 1-indexed
    sessionData[valveIndex].startTime = millis();
    sessionData[valveIndex].status = "IN_PROGRESS";

    DEBUG_SERIAL.println("📱 Session tracking: Tray " + String(valveIndex + 1) + " started");
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

    DEBUG_SERIAL.println("📱 Session tracking: Tray " + String(valveIndex + 1) +
                        " ended - Status: " + status +
                        ", Duration: " + String(sessionData[valveIndex].duration, 1) + "s (valve open to close)");
}

inline void WateringSystem::endTelegramSession() {
    if (!telegramSessionActive) return;

    DEBUG_SERIAL.println("📱 Telegram session ended - preparing completion report");
    telegramSessionActive = false;
}

// ============================================
// Include State Machine Implementation
// ============================================
#include "WateringSystemStateMachine.h"

#endif // WATERING_SYSTEM_H

#ifndef WATERING_SYSTEM_H
#define WATERING_SYSTEM_H

#include <Arduino.h>
#include <PubSubClient.h>
#include "config.h"
#include "ValveController.h"

// External MQTT client (defined in main.cpp)
extern PubSubClient mqttClient;

// ============================================
// Learning Algorithm Helper Functions
// ============================================
namespace LearningAlgorithm {

    // Calculate how many cycles to skip based on fill ratio
    inline int calculateSkipCycles(float fillRatio, unsigned long baselineTime, unsigned long currentTime) {
        if (fillRatio >= LEARNING_EMPTY_THRESHOLD) {
            // Tray was empty - water every cycle
            return 0;
        }

        if (fillRatio < LEARNING_FULL_THRESHOLD) {
            // Tray was almost full - skip many cycles
            return LEARNING_FULL_SKIP_CYCLES;
        }

        // Calculate skip cycles: baseline / (baseline - current) - 1
        unsigned long consumption = baselineTime - currentTime;
        float cyclesToEmpty = (float)baselineTime / (float)consumption;
        int skipCycles = (int)cyclesToEmpty - 1;

        // Apply safety limits
        if (skipCycles < 0) skipCycles = 0;
        if (skipCycles > LEARNING_MAX_SKIP_CYCLES) skipCycles = LEARNING_MAX_SKIP_CYCLES;

        return skipCycles;
    }

    // Calculate water remaining percentage
    inline int calculateWaterRemaining(float fillRatio) {
        return (int)((1.0 - fillRatio) * 100);
    }

    // Get consumption percentage
    inline int calculateConsumptionPercent(float fillRatio) {
        return (int)(fillRatio * 100);
    }
}

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

public:
    // ========== Constructor ==========
    WateringSystem() :
        pumpState(PUMP_OFF),
        activeValveCount(0),
        lastStatePublish(0),
        lastStateJson(""),
        sequentialMode(false),
        currentSequenceIndex(0),
        sequenceLength(0)
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
    void startWatering(int valveIndex);
    void stopWatering(int valveIndex);
    void startSequentialWatering();
    void startSequentialWateringCustom(int* valveIndices, int count);
    void stopSequentialWatering();

    // Learning algorithm
    void resetCalibration(int valveIndex);
    void resetAllCalibrations();
    void printLearningStatus();
    void setSkipCycles(int valveIndex, int cycles);

    // State management
    void publishCurrentState();
    void clearTimeoutFlag(int valveIndex);

private:
    // ========== Core Logic ==========
    void processValve(int valveIndex, unsigned long currentTime);
    bool isValveComplete(int valveIndex);
    void startNextInSequence();

    // ========== Hardware Control ==========
    bool readRainSensor(int valveIndex);
    void openValve(int valveIndex);
    void closeValve(int valveIndex);
    void updatePumpState();

    // ========== Learning Algorithm ==========
    void processLearningData(ValveController* valve, unsigned long currentTime);
    void logSkipCycle(ValveController* valve);
    void logLearningData(ValveController* valve, unsigned long fillTime, float fillRatio, int skipCycles);

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
    for (int i = 0; i < NUM_VALVES; i++) {
        pinMode(VALVE_PINS[i], OUTPUT);
        digitalWrite(VALVE_PINS[i], LOW);
    }

    // Initialize rain sensor pins with internal pull-up
    for (int i = 0; i < NUM_VALVES; i++) {
        pinMode(RAIN_SENSOR_PINS[i], INPUT_PULLUP);
    }

    DEBUG_SERIAL.println("WateringSystem initialized with " + String(NUM_VALVES) + " valves");
    publishStateChange("system", "initialized");
}

// ========== Main Processing Loop ==========
inline void WateringSystem::processWateringLoop() {
    unsigned long currentTime = millis();

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

// ========== Watering Control ==========
inline void WateringSystem::startWatering(int valveIndex) {
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

    // Check if we should skip this watering cycle (learning algorithm)
    if (valve->skipCyclesRemaining > 0) {
        valve->skipCyclesRemaining--;
        logSkipCycle(valve);
        publishStateChange("valve" + String(valveIndex), "cycle_skipped_learning");
        return;
    }

    // Start watering cycle
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    DEBUG_SERIAL.println("Starting watering cycle for valve " + String(valveIndex));

    if (valve->isCalibrated) {
        DEBUG_SERIAL.println("🧠 Calibrated - Baseline: " + String(valve->baselineFillTime / 1000) + "s");
    } else {
        DEBUG_SERIAL.println("🎯 First watering - Establishing baseline");
    }

    DEBUG_SERIAL.println("Step 1: Opening valve (sensor needs water flow)...");
    valve->wateringRequested = true;
    valve->rainDetected = false;
    valve->lastRainCheck = 0;
    valve->phase = PHASE_OPENING_VALVE;
    publishStateChange("valve" + String(valveIndex), "cycle_started");
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
        return;
    }

    int valveIndex = sequenceValves[currentSequenceIndex];
    DEBUG_SERIAL.println("\n→ [Sequence " + String(currentSequenceIndex + 1) + "/" + String(sequenceLength) +
                       "] Starting Valve " + String(valveIndex));

    startWatering(valveIndex);
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
    digitalWrite(VALVE_PINS[valveIndex], HIGH);
    valves[valveIndex]->state = VALVE_OPEN;
    activeValveCount++;
    DEBUG_SERIAL.println("Valve " + String(valveIndex) + " opened (GPIO " + String(VALVE_PINS[valveIndex]) + ")");
}

inline void WateringSystem::closeValve(int valveIndex) {
    digitalWrite(VALVE_PINS[valveIndex], LOW);
    valves[valveIndex]->state = VALVE_CLOSED;
    if (activeValveCount > 0) activeValveCount--;
    DEBUG_SERIAL.println("Valve " + String(valveIndex) + " closed (GPIO " + String(VALVE_PINS[valveIndex]) + ")");
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
        DEBUG_SERIAL.println("Pump turned ON (GPIO " + String(PUMP_PIN) + ")");
        publishStateChange("pump", "on");
    } else if (wateringCount == 0 && pumpState == PUMP_ON) {
        digitalWrite(PUMP_PIN, LOW);
        digitalWrite(LED_PIN, LOW);
        pumpState = PUMP_OFF;
        DEBUG_SERIAL.println("Pump turned OFF (GPIO " + String(PUMP_PIN) + ")");
        publishStateChange("pump", "off");
    }
}

// ========== Learning Algorithm ==========
inline void WateringSystem::processLearningData(ValveController* valve, unsigned long currentTime) {
    if (valve->wateringStartTime == 0 || valve->timeoutOccurred || !valve->rainDetected) {
        return; // Only process successful waterings
    }

    unsigned long fillTime = currentTime - valve->wateringStartTime;
    valve->lastFillTime = fillTime;
    valve->totalWateringCycles++;

    DEBUG_SERIAL.println("🧠 LEARNING DATA:");
    DEBUG_SERIAL.println("  Fill time this cycle: " + String(fillTime / 1000) + "." + String((fillTime % 1000) / 100) + "s");

    if (!valve->isCalibrated) {
        // First successful watering - establish baseline
        valve->baselineFillTime = fillTime;
        valve->isCalibrated = true;
        valve->skipCyclesRemaining = 0;
        DEBUG_SERIAL.println("  🎯 BASELINE ESTABLISHED: " + String(fillTime / 1000) + "." + String((fillTime % 1000) / 100) + "s");
        DEBUG_SERIAL.println("  Next cycle will measure consumption rate");
        publishStateChange("valve" + String(valve->valveIndex), "baseline_calibrated");
    } else {
        // Calculate consumption and skip cycles
        float fillRatio = (float)fillTime / (float)valve->baselineFillTime;
        int skipCycles = LearningAlgorithm::calculateSkipCycles(fillRatio, valve->baselineFillTime, fillTime);
        valve->skipCyclesRemaining = skipCycles;

        logLearningData(valve, fillTime, fillRatio, skipCycles);
    }
}

inline void WateringSystem::logSkipCycle(ValveController* valve) {
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    DEBUG_SERIAL.println("🧠 SMART SKIP: Valve " + String(valve->valveIndex));
    DEBUG_SERIAL.println("  Tray not empty yet based on consumption pattern");
    DEBUG_SERIAL.println("  Baseline fill time: " + String(valve->baselineFillTime / 1000) + "s");
    DEBUG_SERIAL.println("  Last fill time: " + String(valve->lastFillTime / 1000) + "s");
    DEBUG_SERIAL.println("  Cycles remaining to skip: " + String(valve->skipCyclesRemaining));
    DEBUG_SERIAL.println("═══════════════════════════════════════");
}

inline void WateringSystem::logLearningData(ValveController* valve, unsigned long fillTime, float fillRatio, int skipCycles) {
    DEBUG_SERIAL.println("  Baseline: " + String(valve->baselineFillTime / 1000) + "." + String((valve->baselineFillTime % 1000) / 100) + "s");
    DEBUG_SERIAL.println("  Fill ratio: " + String(fillRatio, 2) + " (1.0 = tray was empty)");

    if (fillRatio >= LEARNING_EMPTY_THRESHOLD) {
        DEBUG_SERIAL.println("  ✓ Tray was EMPTY - normal consumption");
        DEBUG_SERIAL.println("  Action: Water every cycle");
    } else if (fillRatio < LEARNING_FULL_THRESHOLD) {
        DEBUG_SERIAL.println("  ⚠️  Tray was >90% FULL - very slow consumption");
        DEBUG_SERIAL.println("  Action: Skip next " + String(skipCycles) + " cycles");
    } else {
        int waterRemaining = LearningAlgorithm::calculateWaterRemaining(fillRatio);
        int consumptionPercent = LearningAlgorithm::calculateConsumptionPercent(fillRatio);
        float cyclesToEmpty = (float)valve->baselineFillTime / (float)(valve->baselineFillTime - fillTime);

        DEBUG_SERIAL.println("  Tray had ~" + String(waterRemaining) + "% water remaining");
        DEBUG_SERIAL.println("  Consumption per cycle: ~" + String(consumptionPercent) + "%");
        DEBUG_SERIAL.println("  Cycles to empty: " + String(cyclesToEmpty, 1));
        DEBUG_SERIAL.println("  Action: Skip next " + String(skipCycles) + " cycle(s)");
    }

    String learningMsg = "{\"valve\":" + String(valve->valveIndex) +
                       ",\"fillTime\":" + String(fillTime) +
                       ",\"baseline\":" + String(valve->baselineFillTime) +
                       ",\"ratio\":" + String(fillRatio, 2) +
                       ",\"skipCycles\":" + String(skipCycles) + "}";
    publishStateChange("learning", learningMsg);
}

inline void WateringSystem::resetCalibration(int valveIndex) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) return;

    ValveController* valve = valves[valveIndex];
    valve->isCalibrated = false;
    valve->baselineFillTime = 0;
    valve->lastFillTime = 0;
    valve->skipCyclesRemaining = 0;
    valve->totalWateringCycles = 0;

    DEBUG_SERIAL.println("═══════════════════════════════════════");
    DEBUG_SERIAL.println("🔄 CALIBRATION RESET: Valve " + String(valveIndex));
    DEBUG_SERIAL.println("  Next watering will establish new baseline");
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    publishStateChange("valve" + String(valveIndex), "calibration_reset");
}

inline void WateringSystem::resetAllCalibrations() {
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    DEBUG_SERIAL.println("🔄 RESET ALL CALIBRATIONS");
    for (int i = 0; i < NUM_VALVES; i++) {
        valves[i]->isCalibrated = false;
        valves[i]->baselineFillTime = 0;
        valves[i]->lastFillTime = 0;
        valves[i]->skipCyclesRemaining = 0;
        valves[i]->totalWateringCycles = 0;
    }
    DEBUG_SERIAL.println("  All valves reset to uncalibrated state");
    DEBUG_SERIAL.println("═══════════════════════════════════════");
    publishStateChange("system", "all_calibrations_reset");
}

inline void WateringSystem::printLearningStatus() {
    DEBUG_SERIAL.println("\n╔═══════════════════════════════════════════╗");
    DEBUG_SERIAL.println("║         LEARNING SYSTEM STATUS            ║");
    DEBUG_SERIAL.println("╚═══════════════════════════════════════════╝");

    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* valve = valves[i];
        DEBUG_SERIAL.println("\n📊 Valve " + String(i) + ":");

        if (valve->isCalibrated) {
            DEBUG_SERIAL.println("  Status: ✓ Calibrated");
            DEBUG_SERIAL.println("  Baseline: " + String(valve->baselineFillTime / 1000) + "." + String((valve->baselineFillTime % 1000) / 100) + "s");
            DEBUG_SERIAL.println("  Last fill: " + String(valve->lastFillTime / 1000) + "." + String((valve->lastFillTime % 1000) / 100) + "s");
            DEBUG_SERIAL.println("  Skip cycles: " + String(valve->skipCyclesRemaining));
            DEBUG_SERIAL.println("  Total cycles: " + String(valve->totalWateringCycles));

            if (valve->lastFillTime > 0 && valve->baselineFillTime > 0) {
                float ratio = (float)valve->lastFillTime / (float)valve->baselineFillTime;
                DEBUG_SERIAL.println("  Last ratio: " + String(ratio, 2) + " (" + String((int)(ratio * 100)) + "% of baseline)");
            }
        } else {
            DEBUG_SERIAL.println("  Status: ⚠️  Not calibrated");
            DEBUG_SERIAL.println("  Action: Run first watering to establish baseline");
        }
    }
    DEBUG_SERIAL.println("\n═══════════════════════════════════════");
}

inline void WateringSystem::setSkipCycles(int valveIndex, int cycles) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) return;

    if (cycles < 0) cycles = 0;
    if (cycles > 20) cycles = 20;  // Safety cap

    valves[valveIndex]->skipCyclesRemaining = cycles;
    DEBUG_SERIAL.println("🧠 Valve " + String(valveIndex) + " skip cycles set to: " + String(cycles));
    publishStateChange("valve" + String(valveIndex), "skip_cycles_set");
}

inline void WateringSystem::clearTimeoutFlag(int valveIndex) {
    if (valveIndex >= 0 && valveIndex < NUM_VALVES) {
        valves[valveIndex]->timeoutOccurred = false;
        DEBUG_SERIAL.println("Timeout flag cleared for valve " + String(valveIndex));
        publishStateChange("valve" + String(valveIndex), "timeout_cleared");
    }
}

// ============================================
// Include State Machine Implementation
// ============================================
#include "WateringSystemStateMachine.h"

#endif // WATERING_SYSTEM_H

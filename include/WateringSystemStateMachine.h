#ifndef WATERING_SYSTEM_STATE_MACHINE_H
#define WATERING_SYSTEM_STATE_MACHINE_H

// This file contains the state machine implementation for WateringSystem
// Included at the end of WateringSystem.h

// ========== State Machine Implementation ==========
inline void WateringSystem::processValve(int valveIndex, unsigned long currentTime) {
    ValveController* valve = valves[valveIndex];

    switch (valve->phase) {
        case PHASE_IDLE:
            // Nothing to do
            break;

        case PHASE_OPENING_VALVE:
            openValve(valveIndex);
            valve->valveOpenTime = currentTime;
            valve->phase = PHASE_WAITING_STABILIZATION;
            DEBUG_SERIAL.println("  Valve " + String(valveIndex) + " opened");
            publishStateChange("valve" + String(valveIndex), "valve_opened");
            break;

        case PHASE_WAITING_STABILIZATION:
            if (currentTime - valve->valveOpenTime >= VALVE_STABILIZATION_DELAY) {
                valve->phase = PHASE_CHECKING_INITIAL_RAIN;
                valve->lastRainCheck = currentTime;
                DEBUG_SERIAL.println("Step 2: Checking rain sensor (water is flowing now)...");
            }
            break;

        case PHASE_CHECKING_INITIAL_RAIN:
            if (currentTime - valve->lastRainCheck >= RAIN_CHECK_INTERVAL) {
                valve->lastRainCheck = currentTime;
                bool isRaining = readRainSensor(valveIndex);
                valve->rainDetected = isRaining;

                if (isRaining) {
                    // Sensor already wet - abort without starting pump
                    DEBUG_SERIAL.println("  Sensor " + String(valveIndex) + " is already WET");
                    DEBUG_SERIAL.println("  Pump will NOT start - closing valve");
                    DEBUG_SERIAL.println("═══════════════════════════════════════");
                    publishStateChange("valve" + String(valveIndex), "already_wet_abort");
                    valve->phase = PHASE_CLOSING_VALVE;
                } else {
                    // Sensor dry - start watering
                    DEBUG_SERIAL.println("  Sensor " + String(valveIndex) + " is DRY");
                    DEBUG_SERIAL.println("Step 3: Starting pump, watering until sensor is wet...");
                    DEBUG_SERIAL.println("  Safety timeout: " + String(MAX_WATERING_TIME / 1000) + " seconds");
                    valve->wateringStartTime = currentTime;
                    valve->timeoutOccurred = false;
                    valve->phase = PHASE_WATERING;
                    updatePumpState();
                    publishStateChange("valve" + String(valveIndex), "watering_started");
                }
            }
            break;

        case PHASE_WATERING:
            // Check timeout first
            if (currentTime - valve->wateringStartTime >= MAX_WATERING_TIME) {
                DEBUG_SERIAL.println("\n⚠️  TIMEOUT: Valve " + String(valveIndex) + " exceeded maximum watering time!");
                DEBUG_SERIAL.println("  Max time: " + String(MAX_WATERING_TIME / 1000) + " seconds");
                DEBUG_SERIAL.println("  Sensor may be faulty or water supply issue");
                DEBUG_SERIAL.println("  SAFETY STOP - Closing valve");
                valve->timeoutOccurred = true;
                valve->phase = PHASE_CLOSING_VALVE;
                updatePumpState();
                publishStateChange("valve" + String(valveIndex), "timeout_safety_stop");
                break;
            }

            // Monitor rain sensor
            if (currentTime - valve->lastRainCheck >= RAIN_CHECK_INTERVAL) {
                valve->lastRainCheck = currentTime;
                bool isRaining = readRainSensor(valveIndex);
                valve->rainDetected = isRaining;

                // Show progress every 1 second
                if ((currentTime - valve->wateringStartTime) % 1000 < RAIN_CHECK_INTERVAL) {
                    int elapsed = (currentTime - valve->wateringStartTime) / 1000;
                    int remaining = (MAX_WATERING_TIME - (currentTime - valve->wateringStartTime)) / 1000;
                    DEBUG_SERIAL.println("  Valve " + String(valveIndex) + " watering: " +
                                    String(elapsed) + "s elapsed, " +
                                    String(remaining) + "s remaining, Sensor: " +
                                    String(isRaining ? "WET" : "DRY"));
                }

                if (isRaining) {
                    // Success - sensor detected water
                    int totalTime = (currentTime - valve->wateringStartTime) / 1000;
                    DEBUG_SERIAL.println("  Sensor " + String(valveIndex) + " detected WATER ✓");
                    DEBUG_SERIAL.println("  Total watering time: " + String(totalTime) + " seconds");
                    DEBUG_SERIAL.println("Step 4: Stopping pump and closing valve");
                    DEBUG_SERIAL.println("═══════════════════════════════════════");
                    publishStateChange("valve" + String(valveIndex), "watering_complete");
                    valve->phase = PHASE_CLOSING_VALVE;
                    updatePumpState();
                } else if (!valve->wateringRequested) {
                    // Manual stop requested
                    DEBUG_SERIAL.println("  Manual stop requested for valve " + String(valveIndex));
                    valve->phase = PHASE_CLOSING_VALVE;
                    updatePumpState();
                }
            }
            break;

        case PHASE_CLOSING_VALVE:
            // Process learning data for successful waterings
            processLearningData(valve, currentTime);

            // Close valve and return to idle
            closeValve(valveIndex);
            valve->phase = PHASE_IDLE;
            valve->wateringRequested = false;
            publishStateChange("valve" + String(valveIndex), "valve_closed");
            updatePumpState();
            break;

        case PHASE_ERROR:
            DEBUG_SERIAL.println("Valve " + String(valveIndex) + " in error state");
            closeValve(valveIndex);
            valve->phase = PHASE_IDLE;
            updatePumpState();
            break;
    }
}

// ========== State Publishing ==========
inline void WateringSystem::publishCurrentState() {
    // Build state JSON
    String stateJson = "{";
    stateJson += "\"pump\":\"" + String(pumpState == PUMP_ON ? "on" : "off") + "\",";
    stateJson += "\"sequential_mode\":" + String(sequentialMode ? "true" : "false");

    if (sequentialMode) {
        stateJson += ",\"sequence_progress\":" + String(currentSequenceIndex);
        stateJson += ",\"sequence_total\":" + String(sequenceLength);
    }

    stateJson += ",\"valves\":[";

    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* valve = valves[i];
        stateJson += "{";
        stateJson += "\"id\":" + String(i);
        stateJson += ",\"state\":\"" + String(valve->state == VALVE_OPEN ? "open" : "closed") + "\"";
        stateJson += ",\"phase\":\"" + String(phaseToString(valve->phase)) + "\"";
        stateJson += ",\"rain\":" + String(valve->rainDetected ? "true" : "false");
        stateJson += ",\"timeout\":" + String(valve->timeoutOccurred ? "true" : "false");

        // Add watering progress if active
        if (valve->phase == PHASE_WATERING && valve->wateringStartTime > 0) {
            unsigned long elapsed = millis() - valve->wateringStartTime;
            int remainingSeconds = (MAX_WATERING_TIME - elapsed) / 1000;
            if (remainingSeconds < 0) remainingSeconds = 0;
            stateJson += ",\"watering_seconds\":" + String(elapsed / 1000);
            stateJson += ",\"remaining_seconds\":" + String(remainingSeconds);
        }

        // Add learning data
        stateJson += ",\"learning\":{";
        stateJson += "\"calibrated\":" + String(valve->isCalibrated ? "true" : "false");
        if (valve->isCalibrated) {
            stateJson += ",\"baseline_ms\":" + String(valve->baselineFillTime);
            stateJson += ",\"last_fill_ms\":" + String(valve->lastFillTime);
            stateJson += ",\"skip_cycles\":" + String(valve->skipCyclesRemaining);
            stateJson += ",\"total_cycles\":" + String(valve->totalWateringCycles);
            if (valve->lastFillTime > 0) {
                float ratio = (float)valve->lastFillTime / (float)valve->baselineFillTime;
                stateJson += ",\"fill_ratio\":" + String(ratio, 2);
            }
        }
        stateJson += "}";

        stateJson += "}";
        if (i < NUM_VALVES - 1) stateJson += ",";
    }

    stateJson += "]}";

    // Cache state for web interface
    lastStateJson = stateJson;

    // Publish to MQTT if connected
    if (mqttClient.connected()) {
        mqttClient.publish(STATE_TOPIC.c_str(), stateJson.c_str());
    }
}

inline void WateringSystem::publishStateChange(const String& component, const String& state) {
    if (!mqttClient.connected()) {
        return; // Silently fail - don't disrupt watering
    }

    String eventJson = "{\"component\":\"" + component +
                       "\",\"state\":\"" + state +
                       "\",\"timestamp\":" + String(millis()) + "}";

    if (!mqttClient.publish(EVENT_TOPIC.c_str(), eventJson.c_str())) {
        DEBUG_SERIAL.println("Failed to publish state change: " + component + " -> " + state);
    }
}

#endif // WATERING_SYSTEM_STATE_MACHINE_H

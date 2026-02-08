#ifndef WATERING_SYSTEM_STATE_MACHINE_H
#define WATERING_SYSTEM_STATE_MACHINE_H

#include "DebugHelper.h"

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
            DebugHelper::debugImportant("✓ Valve " + String(valveIndex) + " opened - waiting stabilization");
            publishStateChange("valve" + String(valveIndex), "valve_opened");
            break;

        case PHASE_WAITING_STABILIZATION:
            if (currentTime - valve->valveOpenTime >= VALVE_STABILIZATION_DELAY) {
                valve->phase = PHASE_CHECKING_INITIAL_RAIN;
                valve->lastRainCheck = currentTime;
                DebugHelper::debug("Step 2: Checking rain sensor (water is flowing now)...");
            }
            break;

        case PHASE_CHECKING_INITIAL_RAIN:
            if (currentTime - valve->lastRainCheck >= RAIN_CHECK_INTERVAL) {
                valve->lastRainCheck = currentTime;
                bool isRaining = readRainSensor(valveIndex);
                valve->rainDetected = isRaining;

                if (isRaining) {
                    // Sensor already wet = TRAY IS FULL - treat as successful fill
                    DebugHelper::debugImportant("✓ Sensor " + String(valveIndex) + " already WET - tray is FULL");

                    // SAFETY: Close valve immediately
                    closeValve(valveIndex);
                    updatePumpState();

                    // CRITICAL: Tray is full - update last watering time to NOW
                    // This makes auto-watering wait for consumption period before retrying
                    valve->lastWateringCompleteTime = currentTime;
                    valve->realTimeSinceLastWatering = 0; // Clear outage duration

                    // If not calibrated, set temporary retry duration to attempt calibration later
                    if (!valve->isCalibrated) {
                        valve->emptyToFullDuration = UNCALIBRATED_RETRY_INTERVAL_MS;
                        DebugHelper::debug("  Tray not calibrated - will retry watering in " + String(UNCALIBRATED_RETRY_INTERVAL_MS / 3600000) + " hours for calibration");
                    } else {
                        DebugHelper::debug("  Updated lastWateringCompleteTime - auto-watering will wait for consumption");
                    }

                    publishStateChange("valve" + String(valveIndex), "already_full_skipped");

                    // Go to PHASE_CLOSING_VALVE for proper cleanup (records session end for Telegram)
                    valve->phase = PHASE_CLOSING_VALVE;
                } else {
                    // Sensor dry - start watering
                    DebugHelper::debugImportant("✓ Sensor " + String(valveIndex) + " is DRY - starting pump (timeout: " + String(getValveNormalTimeout(valveIndex) / 1000) + "s)");
                    valve->wateringStartTime = currentTime;
                    valve->timeoutOccurred = false;
                    valve->phase = PHASE_WATERING;
                    updatePumpState();
                    publishStateChange("valve" + String(valveIndex), "watering_started");
                }
            }
            break;

        case PHASE_WATERING:
            // SAFETY CHECK 1: ABSOLUTE EMERGENCY TIMEOUT - HARD CUTOFF
            if (currentTime - valve->wateringStartTime >= getValveEmergencyTimeout(valveIndex)) {
                DebugHelper::debugImportant("🚨 EMERGENCY CUTOFF: Valve " + String(valveIndex) + " exceeded ABSOLUTE limit " + String(getValveEmergencyTimeout(valveIndex) / 1000) + "s!");
                DebugHelper::debugImportant("🚨 This indicates a CRITICAL SAFETY FAILURE!");
                DebugHelper::debugImportant("🚨 Check sensor hardware immediately!");

                // EMERGENCY: Force everything OFF
                valve->timeoutOccurred = true;
                digitalWrite(VALVE_PINS[valveIndex], LOW);  // Force close
                digitalWrite(PUMP_PIN, LOW);  // Force pump off
                updatePumpState();

                publishStateChange("valve" + String(valveIndex), "emergency_cutoff");
                valve->phase = PHASE_CLOSING_VALVE;
                break;
            }

            // SAFETY CHECK 2: Normal timeout - MAX WATERING TIME
            if (currentTime - valve->wateringStartTime >= getValveNormalTimeout(valveIndex)) {
                DebugHelper::debugImportant("⚠️ TIMEOUT: Valve " + String(valveIndex) + " exceeded " + String(getValveNormalTimeout(valveIndex) / 1000) + "s - IMMEDIATE SAFETY STOP");

                // SAFETY: Immediately close valve and stop pump
                valve->timeoutOccurred = true;
                closeValve(valveIndex);
                updatePumpState();

                publishStateChange("valve" + String(valveIndex), "timeout_safety_stop");
                valve->phase = PHASE_CLOSING_VALVE;  // Go to cleanup phase for learning data
                break;
            }

            // SAFETY CHECK 2: Monitor rain sensor - ALWAYS RESPECT RAIN SENSOR
            if (currentTime - valve->lastRainCheck >= RAIN_CHECK_INTERVAL) {
                valve->lastRainCheck = currentTime;
                bool isRaining = readRainSensor(valveIndex);
                valve->rainDetected = isRaining;

                // Show progress every 1 second
                if ((currentTime - valve->wateringStartTime) % 1000 < RAIN_CHECK_INTERVAL) {
                    int elapsed = (currentTime - valve->wateringStartTime) / 1000;
                    int remaining = (getValveNormalTimeout(valveIndex) - (currentTime - valve->wateringStartTime)) / 1000;
                    DebugHelper::debug("Valve " + String(valveIndex) + ": " + String(elapsed) + "s/" + String(remaining) + "s, Sensor: " + String(isRaining ? "WET" : "DRY"));
                }

                if (isRaining) {
                    // SAFETY: Sensor detected water - immediately close valve and stop pump
                    // Calculate FULL cycle time: from valve open to valve close
                    int totalTime = (currentTime - valve->valveOpenTime) / 1000;
                    int pumpTime = (currentTime - valve->wateringStartTime) / 1000;
                    DebugHelper::debugImportant("✓ Valve " + String(valveIndex) + " COMPLETE - Total: " + String(totalTime) + "s (pump: " + String(pumpTime) + "s)");

                    // Count how many valves are watering
                    int wateringCount = 0;
                    for (int i = 0; i < NUM_VALVES; i++) {
                        if (valves[i]->phase == PHASE_WATERING) {
                            wateringCount++;
                        }
                    }

                    // New logic for single valve watering
                    if (wateringCount == 1) {
                        DebugHelper::debugImportant("✓ Single valve watering complete. Stopping pump and closing valve.");
                        // SAFETY: Stop pump immediately and close valve
                        digitalWrite(PUMP_PIN, LOW);
                        pumpState = PUMP_OFF;
                        statusLED.clear();
                        statusLED.show();
                        publishStateChange("pump", "off");
                        closeValve(valveIndex);
                    } else { // Existing logic for sequential watering
                        // SAFETY: Immediately close valve and let updatePumpState handle the pump
                        closeValve(valveIndex);
                        updatePumpState();
                    }

                    publishStateChange("valve" + String(valveIndex), "watering_complete");
                    valve->phase = PHASE_CLOSING_VALVE;  // Go to cleanup phase for learning data
                } else if (!valve->wateringRequested) {
                    // Manual stop requested - immediately close valve and stop pump
                    DebugHelper::debugImportant("⚠️ Manual stop for valve " + String(valveIndex) + " - IMMEDIATE STOP");

                    // SAFETY: Immediately close valve and stop pump
                    closeValve(valveIndex);
                    updatePumpState();

                    valve->phase = PHASE_IDLE;  // Go directly to IDLE (no learning data for manual stop)
                    valve->wateringRequested = false;
                    valve->wateringStartTime = 0;  // Reset for next watering cycle

                    // CRITICAL: Turn off sensor power (GPIO 18) if no other valves are watering
                    {
                        bool anyWateringStop = false;
                        for (int i = 0; i < NUM_VALVES; i++) {
                            if (valves[i]->phase == PHASE_WATERING) {
                                anyWateringStop = true;
                                break;
                            }
                        }
                        if (!anyWateringStop) {
                            digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
                            DebugHelper::debug("Sensor power (GPIO 18) turned OFF - no valves watering");
                        }
                    }
                }
            }
            break;

        case PHASE_CLOSING_VALVE: {
            // Record session end for Telegram before processing learning data
            if (telegramSessionActive && sessionData[valveIndex].active) {
                String status;
                if (valve->timeoutOccurred) {
                    status = "⚠️ TIMEOUT";
                } else if (valve->rainDetected && valve->wateringStartTime > 0) {
                    // Sensor became wet AFTER pump started = successful watering
                    status = "✓ OK";
                } else if (valve->rainDetected && valve->wateringStartTime == 0) {
                    // Sensor was already wet BEFORE pump started = tray already full
                    status = "✓ FULL";
                } else {
                    // Stopped before sensor became wet
                    status = "⚠️ STOPPED";
                }
                recordSessionEnd(valveIndex, status);

                // Send completion notification for auto-watering (not sequential mode)
                if (!sequentialMode && autoWateringValveIndex == valveIndex) {
                    // Build results for single valve
                    String results[1][3];
                    results[0][0] = String(sessionData[valveIndex].trayNumber);
                    results[0][1] = String(sessionData[valveIndex].duration, 1);
                    results[0][2] = sessionData[valveIndex].status;

                    // Flush debug buffer and send completion
                    DebugHelper::flushBuffer();
                    TelegramNotifier::sendWateringComplete(results, 1);
                    endTelegramSession();
                    autoWateringValveIndex = -1;
                }
            }

            // Process learning data for successful waterings
            processLearningData(valve, currentTime);

            // Close valve and return to idle
            closeValve(valveIndex);
            valve->phase = PHASE_IDLE;
            valve->wateringRequested = false;
            valve->wateringStartTime = 0;  // CRITICAL: Reset for next watering cycle
            publishStateChange("valve" + String(valveIndex), "valve_closed");
            updatePumpState();

            // CRITICAL: Turn off sensor power (GPIO 18) if no other valves are watering
            // This complements the fix in readRainSensor() to manage GPIO 18 state
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
            break;
        }

        case PHASE_ERROR: {
            DebugHelper::debugImportant("❌ ERROR: Valve " + String(valveIndex) + " in error state");
            closeValve(valveIndex);
            valve->phase = PHASE_IDLE;
            valve->wateringStartTime = 0;  // Reset for next watering cycle
            updatePumpState();

            // CRITICAL: Turn off sensor power (GPIO 18) if no other valves are watering
            bool anyWateringError = false;
            for (int i = 0; i < NUM_VALVES; i++) {
                if (valves[i]->phase == PHASE_WATERING) {
                    anyWateringError = true;
                    break;
                }
            }
            if (!anyWateringError) {
                digitalWrite(RAIN_SENSOR_POWER_PIN, LOW);
                DebugHelper::debug("Sensor power (GPIO 18) turned OFF - no valves watering");
            }
            break;
        }
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

    // Add water level sensor status
    stateJson += ",\"water_level\":{";
    stateJson += "\"status\":\"" + String(waterLevelLow ? "low" : "ok") + "\"";
    stateJson += ",\"blocked\":" + String(waterLevelLow ? "true" : "false");
    stateJson += "}";

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
            int remainingSeconds = (getValveNormalTimeout(i) - elapsed) / 1000;
            if (remainingSeconds < 0) remainingSeconds = 0;
            stateJson += ",\"watering_seconds\":" + String(elapsed / 1000);
            stateJson += ",\"remaining_seconds\":" + String(remainingSeconds);
        }

        // Add time-based learning data
        stateJson += ",\"learning\":{";
        stateJson += "\"calibrated\":" + String(valve->isCalibrated ? "true" : "false");
        stateJson += ",\"auto_watering\":" + String(valve->autoWateringEnabled ? "true" : "false");

        if (valve->isCalibrated) {
            unsigned long currentTime = millis();

            stateJson += ",\"baseline_fill_ms\":" + String(valve->baselineFillDuration);
            stateJson += ",\"last_fill_ms\":" + String(valve->lastFillDuration);
            stateJson += ",\"empty_duration_ms\":" + String(valve->emptyToFullDuration);
            stateJson += ",\"total_cycles\":" + String(valve->totalWateringCycles);

            if (valve->emptyToFullDuration > 0 && valve->lastWateringCompleteTime > 0) {
                // Calculate current water level
                float currentWaterLevel = calculateCurrentWaterLevel(valve, currentTime);
                stateJson += ",\"water_level_pct\":" + String((int)currentWaterLevel);
                stateJson += ",\"tray_state\":\"" + String(getTrayState(currentWaterLevel)) + "\"";

                // Time since last watering
                unsigned long timeSinceWatering = currentTime - valve->lastWateringCompleteTime;
                stateJson += ",\"time_since_watering_ms\":" + String(timeSinceWatering);

                // Time until empty
                if (currentWaterLevel > 0 && timeSinceWatering < valve->emptyToFullDuration) {
                    unsigned long timeUntilEmpty = valve->emptyToFullDuration - timeSinceWatering;
                    stateJson += ",\"time_until_empty_ms\":" + String(timeUntilEmpty);
                } else {
                    stateJson += ",\"time_until_empty_ms\":0";
                }
            }

            if (valve->lastFillDuration > 0 && valve->lastWaterLevelPercent >= 0) {
                stateJson += ",\"last_water_level_pct\":" + String((int)valve->lastWaterLevelPercent);
            }
        }
        stateJson += "}";

        stateJson += "}";
        if (i < NUM_VALVES - 1) stateJson += ",";
    }

    stateJson += "]}";

    // Cache state for web interface
    lastStateJson = stateJson;

    // Signal Core 0 (networkTask) to publish via MQTT
    // NOTE: mqttClient is NOT accessed here - it's not thread-safe
    mqttPublishPending = true;
}

// Called from Core 0 (networkTask) to publish cached state via MQTT
inline void WateringSystem::publishPendingMQTTState() {
    if (mqttPublishPending && mqttClient.connected()) {
        mqttClient.publish(STATE_TOPIC.c_str(), lastStateJson.c_str());
        mqttPublishPending = false;
    }
}

inline void WateringSystem::publishStateChange(const String& component, const String& state) {
    // NOTE: This is called from Core 1 (watering loop).
    // Do NOT access mqttClient here - it's not thread-safe.
    // State changes are captured in periodic publishCurrentState() updates
    // which Core 0 publishes via publishPendingMQTTState().
    (void)component;
    (void)state;
}

#endif // WATERING_SYSTEM_STATE_MACHINE_H

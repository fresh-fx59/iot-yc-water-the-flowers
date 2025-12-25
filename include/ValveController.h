#ifndef VALVE_CONTROLLER_H
#define VALVE_CONTROLLER_H

#include "config.h"
#include <Arduino.h>

// ============================================
// Enums
// ============================================
enum ValveState { VALVE_CLOSED = 0, VALVE_OPEN = 1 };

enum PumpState { PUMP_OFF = 0, PUMP_ON = 1 };

enum WateringPhase {
  PHASE_IDLE,
  PHASE_OPENING_VALVE,         // Step 1: Open valve first
  PHASE_WAITING_STABILIZATION, // Step 2: Wait for water flow
  PHASE_CHECKING_INITIAL_RAIN, // Step 3: Check sensor with flowing water
  PHASE_WATERING,              // Step 4: Pump on, wait for wet sensor
  PHASE_CLOSING_VALVE,         // Step 5: Close valve
  PHASE_ERROR
};

// ============================================
// Valve Controller Struct
// ============================================
struct ValveController {
  // Basic state
  int valveIndex;
  ValveState state;
  WateringPhase phase;
  bool wateringRequested;

  // Sensor state
  bool rainDetected;
  bool timeoutOccurred;

  // Timing
  unsigned long lastRainCheck;
  unsigned long valveOpenTime;
  unsigned long wateringStartTime;

  // Time-based learning algorithm data
  unsigned long
      lastWateringCompleteTime; // Timestamp when tray became full (millis)
  unsigned long lastWateringAttemptTime; // Timestamp of last watering attempt
                                         // (successful or not)
  unsigned long emptyToFullDuration; // Learned time for tray to go from full to
                                     // empty (consumption time, ms)
  unsigned long baselineFillDuration; // Time to fill from completely empty (ms)
  unsigned long lastFillDuration;     // Most recent fill duration (ms)
  unsigned long previousFillDuration; // Previous fill duration for trend
                                      // analysis (adaptive interval)
  float lastWaterLevelPercent; // Last measured water level before watering
                               // (0-100%)
  bool isCalibrated;           // Has baseline been established?
  int totalWateringCycles;     // Total successful cycles
  bool autoWateringEnabled;    // Enable automatic watering when empty

  // Adaptive interval learning (binary search for optimal watering interval)
  float intervalMultiplier; // Multiplier for base 24h interval
                            // (1.0=24h, 2.0=48h, 3.5=84h, etc.)

  // Constructor
  ValveController(int idx)
      : valveIndex(idx), state(VALVE_CLOSED), phase(PHASE_IDLE),
        wateringRequested(false), rainDetected(false), timeoutOccurred(false),
        lastRainCheck(0), valveOpenTime(0), wateringStartTime(0),
        lastWateringCompleteTime(0), lastWateringAttemptTime(0),
        emptyToFullDuration(0), baselineFillDuration(0), lastFillDuration(0),
        previousFillDuration(0), lastWaterLevelPercent(0.0),
        isCalibrated(false), totalWateringCycles(0), autoWateringEnabled(true),
        intervalMultiplier(1.0) {}
};

// ============================================
// Helper Functions
// ============================================

// Convert phase enum to string for logging/state
inline const char *phaseToString(WateringPhase phase) {
  switch (phase) {
  case PHASE_IDLE:
    return "idle";
  case PHASE_OPENING_VALVE:
    return "opening_valve";
  case PHASE_WAITING_STABILIZATION:
    return "waiting_stabilization";
  case PHASE_CHECKING_INITIAL_RAIN:
    return "checking_rain";
  case PHASE_WATERING:
    return "watering";
  case PHASE_CLOSING_VALVE:
    return "closing_valve";
  case PHASE_ERROR:
    return "error";
  default:
    return "unknown";
  }
}

// Calculate current water level percentage based on time elapsed
inline float calculateCurrentWaterLevel(const ValveController *valve,
                                        unsigned long currentTime) {
  if (!valve->isCalibrated || valve->emptyToFullDuration == 0) {
    return 0.0; // Unknown
  }

  unsigned long timeSinceLastWatering =
      currentTime - valve->lastWateringCompleteTime;

  if (timeSinceLastWatering >= valve->emptyToFullDuration) {
    return 0.0; // Empty
  }

  // Water level decreases over time
  float consumedPercent =
      (float)timeSinceLastWatering / (float)valve->emptyToFullDuration * 100.0f;
  float waterLevel = 100.0f - consumedPercent;
  return (waterLevel < 0.0f) ? 0.0f : waterLevel;
}

// Get tray state: "empty", "full", "between"
inline const char *getTrayState(float waterLevelPercent) {
  if (waterLevelPercent < 10.0)
    return "empty";
  if (waterLevelPercent > 90.0)
    return "full";
  return "between";
}

// Check if tray is empty or nearly empty (should water)
inline bool shouldWaterNow(const ValveController *valve,
                           unsigned long currentTime) {
  if (!valve->autoWateringEnabled) {
    return false;
  }

  // Block only truly uncalibrated trays without temporary duration
  if (!valve->isCalibrated && valve->emptyToFullDuration == 0) {
    return false; // No consumption data and not calibrated
  }

  // SAFETY 0: Future timestamp check (clock drift/sync error protection)
  // If last watering time is in the future, DO NOT water until time catches up
  if (valve->lastWateringCompleteTime > currentTime) {
    return false;
  }

  // SAFETY 1: Minimum 24-hour interval between ANY watering attempts
  // Prevents excessive watering even if learning data suggests shorter interval
  if (valve->lastWateringAttemptTime > 0) {
    unsigned long timeSinceLastAttempt =
        currentTime - valve->lastWateringAttemptTime;
    if (timeSinceLastAttempt < AUTO_WATERING_MIN_INTERVAL_MS) {
      return false; // Minimum interval not reached (prevents retry loops and
                    // over-watering)
    }
  }

  // LEARN mode (calibrated but no consumption data yet): use 24h minimum
  // interval
  if (valve->emptyToFullDuration == 0) {
    // Already passed 24h check above, so if we're here, it's time to water
    return true;
  }

  // SAFETY 2: Check if tray is empty based on learned consumption rate
  // If tray was last filled 3 days ago and consumption takes 3 days, tray
  // should be empty now
  if (valve->lastWateringCompleteTime > 0) {
    unsigned long timeSinceLastWatering =
        currentTime - valve->lastWateringCompleteTime;
    return timeSinceLastWatering >= valve->emptyToFullDuration;
  }

  // If no lastWateringCompleteTime, don't water (invalid/uncertain timestamp
  // data) Boot watering logic will handle truly overdue valves instead
  return false;
}

#endif // VALVE_CONTROLLER_H

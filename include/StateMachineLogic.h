#ifndef STATE_MACHINE_LOGIC_H
#define STATE_MACHINE_LOGIC_H

#include "ValveController.h"
#include <Arduino.h>

// Pure state machine logic - no hardware dependencies
// This allows testing the state machine transitions in isolation

namespace StateMachineLogic {

// Action types that the state machine can request
enum Action {
  ACTION_NONE,
  ACTION_OPEN_VALVE,
  ACTION_CLOSE_VALVE,
  ACTION_TURN_PUMP_ON,
  ACTION_TURN_PUMP_OFF,
  ACTION_READ_SENSOR,
  ACTION_EMERGENCY_STOP
};

// Result of processing a valve state
struct ProcessResult {
  WateringPhase newPhase;
  Action action;
  bool rainDetected;
  bool timeoutOccurred;
  unsigned long newValveOpenTime;
  unsigned long newWateringStartTime;
  unsigned long newLastRainCheck;

  ProcessResult()
      : newPhase(PHASE_IDLE), action(ACTION_NONE), rainDetected(false),
        timeoutOccurred(false), newValveOpenTime(0), newWateringStartTime(0),
        newLastRainCheck(0) {}
};

// Process a single valve in the state machine
// This is a pure function that takes valve state and returns new state + actions
inline ProcessResult processValveLogic(
    WateringPhase currentPhase,
    unsigned long currentTime,
    unsigned long valveOpenTime,
    unsigned long wateringStartTime,
    unsigned long lastRainCheck,
    bool isRaining,
    bool wateringRequested,
    unsigned long valveStabilizationDelay,
    unsigned long rainCheckInterval,
    unsigned long maxWateringTime,
    unsigned long absoluteSafetyTimeout
) {
  ProcessResult result;
  result.newPhase = currentPhase;
  result.rainDetected = isRaining;
  result.timeoutOccurred = false;
  result.newValveOpenTime = valveOpenTime;
  result.newWateringStartTime = wateringStartTime;
  result.newLastRainCheck = lastRainCheck;

  switch (currentPhase) {
    case PHASE_IDLE:
      // Nothing to do - waiting for external trigger
      result.action = ACTION_NONE;
      break;

    case PHASE_OPENING_VALVE:
      // Open valve and transition to stabilization
      result.action = ACTION_OPEN_VALVE;
      result.newPhase = PHASE_WAITING_STABILIZATION;
      result.newValveOpenTime = currentTime;
      break;

    case PHASE_WAITING_STABILIZATION:
      // Wait for valve to fully open
      if (currentTime - valveOpenTime >= valveStabilizationDelay) {
        result.newPhase = PHASE_CHECKING_INITIAL_RAIN;
        result.newLastRainCheck = currentTime;
      }
      result.action = ACTION_NONE;
      break;

    case PHASE_CHECKING_INITIAL_RAIN:
      // Check if sensor needs to be read
      if (currentTime - lastRainCheck >= rainCheckInterval) {
        result.newLastRainCheck = currentTime;
        result.action = ACTION_READ_SENSOR;

        if (isRaining) {
          // Tray already full - close valve, don't turn on pump
          result.newPhase = PHASE_CLOSING_VALVE;
          result.action = ACTION_CLOSE_VALVE;
        } else {
          // Tray dry - start watering
          result.newPhase = PHASE_WATERING;
          result.newWateringStartTime = currentTime;
          result.action = ACTION_TURN_PUMP_ON;
        }
      } else {
        result.action = ACTION_NONE;
      }
      break;

    case PHASE_WATERING:
      // SAFETY CHECK 1: ABSOLUTE EMERGENCY TIMEOUT
      if (currentTime - wateringStartTime >= absoluteSafetyTimeout) {
        result.timeoutOccurred = true;
        result.newPhase = PHASE_CLOSING_VALVE;
        result.action = ACTION_EMERGENCY_STOP;
        break;
      }

      // SAFETY CHECK 2: Normal timeout - MAX WATERING TIME
      if (currentTime - wateringStartTime >= maxWateringTime) {
        result.timeoutOccurred = true;
        result.newPhase = PHASE_CLOSING_VALVE;
        result.action = ACTION_CLOSE_VALVE;
        break;
      }

      // Check rain sensor periodically
      if (currentTime - lastRainCheck >= rainCheckInterval) {
        result.newLastRainCheck = currentTime;
        result.action = ACTION_READ_SENSOR;

        if (isRaining) {
          // Watering complete - sensor became wet
          result.newPhase = PHASE_CLOSING_VALVE;
          result.action = ACTION_CLOSE_VALVE;
        } else if (!wateringRequested) {
          // Manual stop requested
          result.newPhase = PHASE_IDLE;
          result.newWateringStartTime = 0;
          result.action = ACTION_CLOSE_VALVE;
        }
      } else {
        result.action = ACTION_NONE;
      }
      break;

    case PHASE_CLOSING_VALVE:
      // Final cleanup - close valve and return to idle
      result.action = ACTION_CLOSE_VALVE;
      result.newPhase = PHASE_IDLE;
      result.newWateringStartTime = 0;
      break;

    case PHASE_ERROR:
      // Error recovery - close valve and return to idle
      result.action = ACTION_CLOSE_VALVE;
      result.newPhase = PHASE_IDLE;
      result.newWateringStartTime = 0;
      break;

    default:
      result.action = ACTION_NONE;
      break;
  }

  return result;
}

} // namespace StateMachineLogic

#endif // STATE_MACHINE_LOGIC_H

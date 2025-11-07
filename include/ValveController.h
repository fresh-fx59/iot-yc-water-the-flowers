#ifndef VALVE_CONTROLLER_H
#define VALVE_CONTROLLER_H

#include <Arduino.h>

// ============================================
// Enums
// ============================================
enum ValveState {
    VALVE_CLOSED = 0,
    VALVE_OPEN = 1
};

enum PumpState {
    PUMP_OFF = 0,
    PUMP_ON = 1
};

enum WateringPhase {
    PHASE_IDLE,
    PHASE_OPENING_VALVE,           // Step 1: Open valve first
    PHASE_WAITING_STABILIZATION,   // Step 2: Wait for water flow
    PHASE_CHECKING_INITIAL_RAIN,   // Step 3: Check sensor with flowing water
    PHASE_WATERING,                // Step 4: Pump on, wait for wet sensor
    PHASE_CLOSING_VALVE,           // Step 5: Close valve
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

    // Learning algorithm data
    unsigned long baselineFillTime;   // Time to fill from empty (ms)
    unsigned long lastFillTime;       // Most recent fill time (ms)
    int skipCyclesRemaining;          // Cycles to skip before next watering
    bool isCalibrated;                // Has baseline been established?
    int totalWateringCycles;          // Total successful cycles

    // Constructor
    ValveController(int idx) :
        valveIndex(idx),
        state(VALVE_CLOSED),
        phase(PHASE_IDLE),
        wateringRequested(false),
        rainDetected(false),
        timeoutOccurred(false),
        lastRainCheck(0),
        valveOpenTime(0),
        wateringStartTime(0),
        baselineFillTime(0),
        lastFillTime(0),
        skipCyclesRemaining(0),
        isCalibrated(false),
        totalWateringCycles(0) {}
};

// ============================================
// Helper Functions
// ============================================

// Convert phase enum to string for logging/state
inline const char* phaseToString(WateringPhase phase) {
    switch (phase) {
        case PHASE_IDLE: return "idle";
        case PHASE_OPENING_VALVE: return "opening_valve";
        case PHASE_WAITING_STABILIZATION: return "waiting_stabilization";
        case PHASE_CHECKING_INITIAL_RAIN: return "checking_rain";
        case PHASE_WATERING: return "watering";
        case PHASE_CLOSING_VALVE: return "closing_valve";
        case PHASE_ERROR: return "error";
        default: return "unknown";
    }
}

#endif // VALVE_CONTROLLER_H

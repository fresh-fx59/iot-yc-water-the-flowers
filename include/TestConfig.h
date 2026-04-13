#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

// Minimal configuration for native testing
// Avoids duplicate symbol issues from full config.h

// ============================================
// System Constants for Testing
// ============================================
#ifdef NATIVE_TEST
static const int NUM_VALVES = 6;

// ============================================
// Timing Constants for Testing
// ============================================
static const unsigned long RAIN_CHECK_INTERVAL = 100;
static const unsigned long VALVE_STABILIZATION_DELAY = 500;
static const unsigned long STATE_PUBLISH_INTERVAL = 2000;
static const unsigned long MAX_WATERING_TIME = 25000;
static const unsigned long ABSOLUTE_SAFETY_TIMEOUT = 30000;

// Per-valve timeouts (mirror production config for testing)
static const unsigned long VALVE_NORMAL_TIMEOUTS[NUM_VALVES] = {
    40000,  // Valve 0: 40s (slower fill rate, matches production)
    25000, 25000, 25000, 25000, 25000
};
static const unsigned long VALVE_EMERGENCY_TIMEOUTS[NUM_VALVES] = {
    45000,  // Valve 0: 45s (5s margin, matches production)
    30000, 30000, 30000, 30000, 30000
};

// Helper functions (mirror config.h)
inline unsigned long getValveNormalTimeout(int valveIndex) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
        return MAX_WATERING_TIME;
    }
    return VALVE_NORMAL_TIMEOUTS[valveIndex];
}

inline unsigned long getValveEmergencyTimeout(int valveIndex) {
    if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
        return ABSOLUTE_SAFETY_TIMEOUT;
    }
    return VALVE_EMERGENCY_TIMEOUTS[valveIndex];
}

static const unsigned long SENSOR_POWER_STABILIZATION = 100;

// ============================================
// Learning Algorithm Constants for Testing
// ============================================
static const float LEARNING_EMPTY_THRESHOLD = 0.95;
static const float LEARNING_FULL_THRESHOLD = 0.10;
static const int LEARNING_MAX_SKIP_CYCLES = 15;
static const int LEARNING_FULL_SKIP_CYCLES = 10;
static const unsigned long AUTO_WATERING_MIN_INTERVAL_MS = 86400000;
static const unsigned long UNCALIBRATED_RETRY_INTERVAL_MS = 86400000;

// ============================================
// Plant Light Constants for Testing
// ============================================
#define PLANT_LIGHT_RELAY_PIN 41
static const bool PLANT_LIGHT_ACTIVE_HIGH = false;
static const int PLANT_LIGHT_SCHEDULE_ON_HOUR = 22;
static const int PLANT_LIGHT_SCHEDULE_ON_MINUTE = 0;
static const int PLANT_LIGHT_SCHEDULE_OFF_HOUR = 7;
static const int PLANT_LIGHT_SCHEDULE_OFF_MINUTE = 0;
static const unsigned long PLANT_LIGHT_SCHEDULE_CHECK_INTERVAL_MS = 1000;
#endif

#endif // TEST_CONFIG_H

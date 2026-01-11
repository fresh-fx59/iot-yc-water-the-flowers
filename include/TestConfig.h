#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

// Minimal configuration for native testing
// Avoids duplicate symbol issues from full config.h

// ============================================
// Timing Constants for Testing
// ============================================
#ifdef NATIVE_TEST
static const unsigned long RAIN_CHECK_INTERVAL = 100;
static const unsigned long VALVE_STABILIZATION_DELAY = 500;
static const unsigned long STATE_PUBLISH_INTERVAL = 2000;
static const unsigned long MAX_WATERING_TIME = 25000;
static const unsigned long ABSOLUTE_SAFETY_TIMEOUT = 30000;
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
// System Constants for Testing
// ============================================
static const int NUM_VALVES = 6;
#endif

#endif // TEST_CONFIG_H

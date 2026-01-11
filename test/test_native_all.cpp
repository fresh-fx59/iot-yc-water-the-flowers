#ifdef NATIVE_TEST

#include <unity.h>
#include <ArduinoFake.h>
#include "LearningAlgorithm.h"
#include "StateMachineLogic.h"
#include "TestConfig.h"

using namespace fakeit;
using namespace StateMachineLogic;

void setUp(void) {
    // Reset before each test
}

void tearDown(void) {
    // Cleanup after each test
}

// ============================================
// LEARNING ALGORITHM TESTS
// ============================================

// Test calculateWaterLevelBefore
void test_calculate_water_level(void) {
    // Case 1: Full fill (took same time as baseline) -> Was 0%
    TEST_ASSERT_FLOAT_WITHIN(1.0, 0.0, LearningAlgorithm::calculateWaterLevelBefore(10000, 10000));

    // Case 2: Half fill (took half time) -> Was 50%
    TEST_ASSERT_FLOAT_WITHIN(1.0, 50.0, LearningAlgorithm::calculateWaterLevelBefore(5000, 10000));

    // Case 3: Quarter fill (took 25% time) -> Was 75%
    TEST_ASSERT_FLOAT_WITHIN(1.0, 75.0, LearningAlgorithm::calculateWaterLevelBefore(2500, 10000));

    // Case 4: Zero baseline (avoid divide by zero)
    TEST_ASSERT_EQUAL_FLOAT(0.0, LearningAlgorithm::calculateWaterLevelBefore(5000, 0));
}

// Test calculateEmptyDuration
void test_calculate_empty_duration(void) {
    // Case 1: From empty (fillRatio = 1.0)
    // If it took 24h to become empty, duration should be 24h
    unsigned long timeSince = 24 * 3600 * 1000; // 24h
    TEST_ASSERT_EQUAL_UINT32(timeSince, LearningAlgorithm::calculateEmptyDuration(10000, 10000, timeSince));

    // Case 2: From half empty (fillRatio = 0.5)
    // If it took 12h to consume 50%, total capacity is 24h
    unsigned long twelveHours = 12 * 3600 * 1000;
    unsigned long twentyFourHours = 24 * 3600 * 1000;
    TEST_ASSERT_EQUAL_UINT32(twentyFourHours, LearningAlgorithm::calculateEmptyDuration(5000, 10000, twelveHours));
}

// Test formatDuration
void test_format_duration(void) {
    // 1. Seconds
    TEST_ASSERT_EQUAL_STRING("5.5s", LearningAlgorithm::formatDuration(5500).c_str());

    // 2. Minutes
    TEST_ASSERT_EQUAL_STRING("2m 30s", LearningAlgorithm::formatDuration(150000).c_str());

    // 3. Hours
    TEST_ASSERT_EQUAL_STRING("1h 30m", LearningAlgorithm::formatDuration(5400000).c_str());

    // 4. Days
    TEST_ASSERT_EQUAL_STRING("2d 2h", LearningAlgorithm::formatDuration(180000000).c_str());
}

// ============================================
// STATE MACHINE TESTS
// ============================================

// ========== PHASE_IDLE Tests ==========

void test_idle_phase_does_nothing(void) {
    ProcessResult result = processValveLogic(
        PHASE_IDLE, 1000, 0, 0, 0, false, false,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);
}

// ========== PHASE_OPENING_VALVE Tests ==========

void test_opening_valve_transitions_to_stabilization(void) {
    unsigned long currentTime = 5000;
    ProcessResult result = processValveLogic(
        PHASE_OPENING_VALVE, currentTime, 0, 0, 0, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_OPEN_VALVE, result.action);
    TEST_ASSERT_EQUAL(currentTime, result.newValveOpenTime);
}

// ========== PHASE_WAITING_STABILIZATION Tests ==========

void test_stabilization_waits_for_delay(void) {
    unsigned long valveOpenTime = 1000;
    unsigned long currentTime = 1200;
    ProcessResult result = processValveLogic(
        PHASE_WAITING_STABILIZATION, currentTime, valveOpenTime, 0, 0, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);
}

void test_stabilization_transitions_after_delay(void) {
    unsigned long valveOpenTime = 1000;
    unsigned long currentTime = 1500;
    ProcessResult result = processValveLogic(
        PHASE_WAITING_STABILIZATION, currentTime, valveOpenTime, 0, 0, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, result.newPhase);
    TEST_ASSERT_EQUAL(currentTime, result.newLastRainCheck);
}

// ========== PHASE_CHECKING_INITIAL_RAIN Tests ==========

void test_initial_rain_check_sensor_dry_starts_watering(void) {
    unsigned long currentTime = 2000;
    unsigned long lastRainCheck = 1800;
    ProcessResult result = processValveLogic(
        PHASE_CHECKING_INITIAL_RAIN, currentTime, 1000, 0, lastRainCheck, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_TURN_PUMP_ON, result.action);
    TEST_ASSERT_EQUAL(currentTime, result.newWateringStartTime);
}

void test_initial_rain_check_sensor_wet_skips_watering(void) {
    unsigned long currentTime = 2000;
    unsigned long lastRainCheck = 1800;
    ProcessResult result = processValveLogic(
        PHASE_CHECKING_INITIAL_RAIN, currentTime, 1000, 0, lastRainCheck, true, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_TRUE(result.rainDetected);
}

void test_initial_rain_check_waits_for_interval(void) {
    unsigned long currentTime = 2000;
    unsigned long lastRainCheck = 1950;
    ProcessResult result = processValveLogic(
        PHASE_CHECKING_INITIAL_RAIN, currentTime, 1000, 0, lastRainCheck, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);
}

// ========== PHASE_WATERING Tests ==========

void test_watering_completes_when_sensor_wet(void) {
    unsigned long wateringStartTime = 3000;
    unsigned long currentTime = 6000;
    unsigned long lastRainCheck = 5800;
    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 2000, wateringStartTime, lastRainCheck, true, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_TRUE(result.rainDetected);
    TEST_ASSERT_FALSE(result.timeoutOccurred);
}

void test_watering_continues_when_sensor_dry(void) {
    unsigned long wateringStartTime = 3000;
    unsigned long currentTime = 6000;
    unsigned long lastRainCheck = 5800;
    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 2000, wateringStartTime, lastRainCheck, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_READ_SENSOR, result.action);
}

void test_watering_timeout_normal(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = 26000;
    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, 25900, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
}

void test_watering_timeout_emergency(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = 31000;
    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, 30900, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_EMERGENCY_STOP, result.action);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
}

void test_watering_manual_stop(void) {
    unsigned long wateringStartTime = 3000;
    unsigned long currentTime = 5000;
    unsigned long lastRainCheck = 4800;
    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 2000, wateringStartTime, lastRainCheck, false, false,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_EQUAL(0, result.newWateringStartTime);
}

void test_watering_waits_for_sensor_check_interval(void) {
    unsigned long wateringStartTime = 3000;
    unsigned long currentTime = 5000;
    unsigned long lastRainCheck = 4950;
    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 2000, wateringStartTime, lastRainCheck, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);
}

// ========== PHASE_CLOSING_VALVE Tests ==========

void test_closing_valve_returns_to_idle(void) {
    ProcessResult result = processValveLogic(
        PHASE_CLOSING_VALVE, 10000, 5000, 6000, 9800, true, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_EQUAL(0, result.newWateringStartTime);
}

// ========== PHASE_ERROR Tests ==========

void test_error_phase_recovers_to_idle(void) {
    ProcessResult result = processValveLogic(
        PHASE_ERROR, 10000, 5000, 6000, 9800, false, false,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_EQUAL(0, result.newWateringStartTime);
}

// ========== Full Cycle Tests ==========

void test_full_successful_watering_cycle(void) {
    unsigned long time = 1000;

    // 1. PHASE_OPENING_VALVE
    ProcessResult r1 = processValveLogic(PHASE_OPENING_VALVE, time, 0, 0, 0, false, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, r1.newPhase);

    // 2. PHASE_WAITING_STABILIZATION
    time = 1500;
    ProcessResult r2 = processValveLogic(PHASE_WAITING_STABILIZATION, time, r1.newValveOpenTime,
                                         0, 0, false, true, VALVE_STABILIZATION_DELAY,
                                         RAIN_CHECK_INTERVAL, MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, r2.newPhase);

    // 3. PHASE_CHECKING_INITIAL_RAIN (dry)
    time = 1600;
    ProcessResult r3 = processValveLogic(PHASE_CHECKING_INITIAL_RAIN, time, r1.newValveOpenTime,
                                         0, r2.newLastRainCheck, false, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_WATERING, r3.newPhase);
    TEST_ASSERT_EQUAL(ACTION_TURN_PUMP_ON, r3.action);

    // 4. PHASE_WATERING (sensor becomes wet)
    time = 4600;
    ProcessResult r4 = processValveLogic(PHASE_WATERING, time, r1.newValveOpenTime,
                                         r3.newWateringStartTime, r3.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, r4.newPhase);
    TEST_ASSERT_FALSE(r4.timeoutOccurred);

    // 5. PHASE_CLOSING_VALVE
    ProcessResult r5 = processValveLogic(PHASE_CLOSING_VALVE, time, r1.newValveOpenTime,
                                         r3.newWateringStartTime, r4.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_IDLE, r5.newPhase);
}

void test_full_already_wet_cycle(void) {
    unsigned long time = 1000;

    // 1. PHASE_OPENING_VALVE
    ProcessResult r1 = processValveLogic(PHASE_OPENING_VALVE, time, 0, 0, 0, false, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, r1.newPhase);

    // 2. PHASE_WAITING_STABILIZATION
    time = 1500;
    ProcessResult r2 = processValveLogic(PHASE_WAITING_STABILIZATION, time, r1.newValveOpenTime,
                                         0, 0, false, true, VALVE_STABILIZATION_DELAY,
                                         RAIN_CHECK_INTERVAL, MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, r2.newPhase);

    // 3. PHASE_CHECKING_INITIAL_RAIN (WET - tray already full)
    time = 1600;
    ProcessResult r3 = processValveLogic(PHASE_CHECKING_INITIAL_RAIN, time, r1.newValveOpenTime,
                                         0, r2.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, r3.newPhase);
    TEST_ASSERT_TRUE(r3.rainDetected);

    // 4. PHASE_CLOSING_VALVE (pump never turned on)
    ProcessResult r4 = processValveLogic(PHASE_CLOSING_VALVE, time, r1.newValveOpenTime,
                                         0, r3.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_IDLE, r4.newPhase);
}

// ============================================
// OVERWATERING SCENARIO TESTS
// ============================================

// Helper: Simulate a full watering cycle
unsigned long simulateFullCycle(bool sensorStuckDry, unsigned long maxTime) {
    unsigned long time = 0;
    unsigned long startTime = time;
    bool isRaining = false;

    // Start watering
    ProcessResult r1 = processValveLogic(
        PHASE_OPENING_VALVE, time, 0, 0, 0, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Stabilization
    time += VALVE_STABILIZATION_DELAY;
    ProcessResult r2 = processValveLogic(
        PHASE_WAITING_STABILIZATION, time, r1.newValveOpenTime, 0, 0, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Initial rain check (sensor DRY)
    time += RAIN_CHECK_INTERVAL;
    ProcessResult r3 = processValveLogic(
        PHASE_CHECKING_INITIAL_RAIN, time, r1.newValveOpenTime,
        0, r2.newLastRainCheck, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Now in PHASE_WATERING - loop until timeout or sensor wet
    ProcessResult result = r3;
    while (result.newPhase == PHASE_WATERING && time - startTime < maxTime) {
        time += RAIN_CHECK_INTERVAL;

        // If sensor stuck dry, never becomes wet
        // Otherwise, simulate sensor becoming wet at 3 seconds
        if (!sensorStuckDry && (time - r3.newWateringStartTime) >= 3000) {
            isRaining = true;
        }

        result = processValveLogic(
            PHASE_WATERING, time, r1.newValveOpenTime,
            r3.newWateringStartTime, result.newLastRainCheck, isRaining, true,
            VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
            MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
        );

        if (result.newPhase != PHASE_WATERING) {
            break;
        }
    }

    return time - startTime;
}

// Test 1: Sensor stuck dry triggers timeout
void test_overwatering_sensor_stuck_dry(void) {
    unsigned long duration = simulateFullCycle(true, 40000);
    TEST_ASSERT_GREATER_OR_EQUAL(MAX_WATERING_TIME, duration);
    TEST_ASSERT_LESS_THAN(MAX_WATERING_TIME + 2000, duration);
}

// Test 2: Emergency timeout triggers after absolute limit
void test_overwatering_emergency_timeout(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = wateringStartTime + ABSOLUTE_SAFETY_TIMEOUT;

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, currentTime - 100,
        false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
    TEST_ASSERT_EQUAL(ACTION_EMERGENCY_STOP, result.action);
}

// Test 3: millis() overflow handling
void test_overwatering_millis_overflow(void) {
    // Start watering 10 seconds before overflow
    unsigned long wateringStartTime = ULONG_MAX - 10000;

    // Current time is 5 seconds after overflow
    unsigned long currentTime = 5000;

    // BUG DETECTION: The state machine uses unsigned arithmetic
    // currentTime - wateringStartTime = 5000 - (ULONG_MAX - 10000)
    // This wraps to ~15000ms (not the expected ~49 days overflow)

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, currentTime - 100,
        false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // ACTUAL BEHAVIOR: System calculates elapsed as ~15000ms (< MAX_WATERING_TIME)
    // So it continues watering (DOES NOT trigger timeout)
    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_FALSE(result.timeoutOccurred);

    // However, system will eventually timeout when the calculated time exceeds limit
    // Let's test: advance to point where wraparound elapsed > MAX_WATERING_TIME
    currentTime = MAX_WATERING_TIME + 1000; // 26 seconds after overflow

    result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, currentTime - 100,
        false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Now it should timeout
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
}

// Test 4: Multiple sensor failures
void test_overwatering_multiple_sensors_fail(void) {
    int failedValves = 6;
    unsigned long totalDuration = 0;

    for (int i = 0; i < failedValves; i++) {
        unsigned long valveDuration = simulateFullCycle(true, 40000);
        totalDuration += valveDuration;
    }

    // Total: ~6 * 25s = 150s (2.5 minutes)
    TEST_ASSERT_GREATER_OR_EQUAL(6 * MAX_WATERING_TIME, totalDuration);
    TEST_ASSERT_LESS_THAN(6 * (MAX_WATERING_TIME + 2000), totalDuration);
}

// Test 5: Manual stop works during overwatering
void test_overwatering_manual_stop_works(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = 15000; // 14 seconds in
    unsigned long lastRainCheck = 14800;

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, lastRainCheck,
        false, // Sensor dry (stuck)
        false, // Manual stop
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should immediately stop
    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
}

// Test 6: Timeout takes priority over sensor check interval
void test_overwatering_timeout_priority(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = wateringStartTime + MAX_WATERING_TIME;
    unsigned long lastRainCheck = currentTime - 50; // Only 50ms ago

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, lastRainCheck,
        false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Timeout check runs BEFORE sensor check
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
}

// Test 7: Sensor becomes wet immediately stops
void test_overwatering_sensor_recovery(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = 15000; // 14 seconds in
    unsigned long lastRainCheck = 14800;

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, lastRainCheck,
        true, // Sensor becomes wet (recovered)
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should immediately close
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_FALSE(result.timeoutOccurred); // Not a timeout - sensor detected
}

// Test 8: Realistic scenario - partial sensor failure
void test_overwatering_realistic_partial_failure(void) {
    unsigned long failedValveTime = 0;
    unsigned long normalValveTime = 0;

    // 3 failed sensors (timeout)
    for (int i = 0; i < 3; i++) {
        failedValveTime += simulateFullCycle(true, 40000);
    }

    // 3 normal sensors (complete quickly)
    for (int i = 3; i < 6; i++) {
        normalValveTime += simulateFullCycle(false, 40000);
    }

    // Failed valves: ~25s each
    TEST_ASSERT_GREATER_OR_EQUAL(75000, failedValveTime);

    // Normal valves: ~3s each
    TEST_ASSERT_LESS_THAN(20000, normalValveTime);
}

// ========== Main Test Runner ==========

int main(int argc, char **argv) {
    UNITY_BEGIN();

    // Learning Algorithm Tests
    RUN_TEST(test_calculate_water_level);
    RUN_TEST(test_calculate_empty_duration);
    RUN_TEST(test_format_duration);

    // State Machine Tests - PHASE_IDLE
    RUN_TEST(test_idle_phase_does_nothing);

    // State Machine Tests - PHASE_OPENING_VALVE
    RUN_TEST(test_opening_valve_transitions_to_stabilization);

    // State Machine Tests - PHASE_WAITING_STABILIZATION
    RUN_TEST(test_stabilization_waits_for_delay);
    RUN_TEST(test_stabilization_transitions_after_delay);

    // State Machine Tests - PHASE_CHECKING_INITIAL_RAIN
    RUN_TEST(test_initial_rain_check_sensor_dry_starts_watering);
    RUN_TEST(test_initial_rain_check_sensor_wet_skips_watering);
    RUN_TEST(test_initial_rain_check_waits_for_interval);

    // State Machine Tests - PHASE_WATERING
    RUN_TEST(test_watering_completes_when_sensor_wet);
    RUN_TEST(test_watering_continues_when_sensor_dry);
    RUN_TEST(test_watering_timeout_normal);
    RUN_TEST(test_watering_timeout_emergency);
    RUN_TEST(test_watering_manual_stop);
    RUN_TEST(test_watering_waits_for_sensor_check_interval);

    // State Machine Tests - PHASE_CLOSING_VALVE
    RUN_TEST(test_closing_valve_returns_to_idle);

    // State Machine Tests - PHASE_ERROR
    RUN_TEST(test_error_phase_recovers_to_idle);

    // State Machine Tests - Full Cycles
    RUN_TEST(test_full_successful_watering_cycle);
    RUN_TEST(test_full_already_wet_cycle);

    // Overwatering Scenario Tests
    RUN_TEST(test_overwatering_sensor_stuck_dry);
    RUN_TEST(test_overwatering_emergency_timeout);
    RUN_TEST(test_overwatering_millis_overflow);
    RUN_TEST(test_overwatering_multiple_sensors_fail);
    RUN_TEST(test_overwatering_manual_stop_works);
    RUN_TEST(test_overwatering_timeout_priority);
    RUN_TEST(test_overwatering_sensor_recovery);
    RUN_TEST(test_overwatering_realistic_partial_failure);

    return UNITY_END();
}

#endif // NATIVE_TEST

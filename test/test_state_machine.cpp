// This test file is deprecated - use test_native_all.cpp instead
// Keeping for reference only
#ifndef NATIVE_TEST

#include <unity.h>
#include <ArduinoFake.h>
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

// ========== PHASE_IDLE Tests ==========

void test_idle_phase_does_nothing(void) {
    ProcessResult result = processValveLogic(
        PHASE_IDLE,
        1000,  // currentTime
        0,     // valveOpenTime
        0,     // wateringStartTime
        0,     // lastRainCheck
        false, // isRaining
        false, // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);
}

// ========== PHASE_OPENING_VALVE Tests ==========

void test_opening_valve_transitions_to_stabilization(void) {
    unsigned long currentTime = 5000;

    ProcessResult result = processValveLogic(
        PHASE_OPENING_VALVE,
        currentTime,
        0,     // valveOpenTime (will be set)
        0,     // wateringStartTime
        0,     // lastRainCheck
        false, // isRaining
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_OPEN_VALVE, result.action);
    TEST_ASSERT_EQUAL(currentTime, result.newValveOpenTime);
}

// ========== PHASE_WAITING_STABILIZATION Tests ==========

void test_stabilization_waits_for_delay(void) {
    unsigned long valveOpenTime = 1000;
    unsigned long currentTime = 1200;  // Only 200ms elapsed

    ProcessResult result = processValveLogic(
        PHASE_WAITING_STABILIZATION,
        currentTime,
        valveOpenTime,
        0,     // wateringStartTime
        0,     // lastRainCheck
        false, // isRaining
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);
}

void test_stabilization_transitions_after_delay(void) {
    unsigned long valveOpenTime = 1000;
    unsigned long currentTime = 1500;  // 500ms elapsed

    ProcessResult result = processValveLogic(
        PHASE_WAITING_STABILIZATION,
        currentTime,
        valveOpenTime,
        0,     // wateringStartTime
        0,     // lastRainCheck
        false, // isRaining
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, result.newPhase);
    TEST_ASSERT_EQUAL(currentTime, result.newLastRainCheck);
}

// ========== PHASE_CHECKING_INITIAL_RAIN Tests ==========

void test_initial_rain_check_sensor_dry_starts_watering(void) {
    unsigned long currentTime = 2000;
    unsigned long lastRainCheck = 1800;  // 200ms ago (> 100ms interval)

    ProcessResult result = processValveLogic(
        PHASE_CHECKING_INITIAL_RAIN,
        currentTime,
        1000,  // valveOpenTime
        0,     // wateringStartTime
        lastRainCheck,
        false, // isRaining (sensor DRY)
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_TURN_PUMP_ON, result.action);
    TEST_ASSERT_EQUAL(currentTime, result.newWateringStartTime);
    TEST_ASSERT_EQUAL(currentTime, result.newLastRainCheck);
}

void test_initial_rain_check_sensor_wet_skips_watering(void) {
    unsigned long currentTime = 2000;
    unsigned long lastRainCheck = 1800;  // 200ms ago (> 100ms interval)

    ProcessResult result = processValveLogic(
        PHASE_CHECKING_INITIAL_RAIN,
        currentTime,
        1000,  // valveOpenTime
        0,     // wateringStartTime
        lastRainCheck,
        true,  // isRaining (sensor WET - tray already full)
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_TRUE(result.rainDetected);
}

void test_initial_rain_check_waits_for_interval(void) {
    unsigned long currentTime = 2000;
    unsigned long lastRainCheck = 1950;  // Only 50ms ago (< 100ms interval)

    ProcessResult result = processValveLogic(
        PHASE_CHECKING_INITIAL_RAIN,
        currentTime,
        1000,  // valveOpenTime
        0,     // wateringStartTime
        lastRainCheck,
        false, // isRaining
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);
}

// ========== PHASE_WATERING Tests ==========

void test_watering_completes_when_sensor_wet(void) {
    unsigned long wateringStartTime = 3000;
    unsigned long currentTime = 6000;  // 3 seconds of watering
    unsigned long lastRainCheck = 5800;  // 200ms ago (> 100ms interval)

    ProcessResult result = processValveLogic(
        PHASE_WATERING,
        currentTime,
        2000,  // valveOpenTime
        wateringStartTime,
        lastRainCheck,
        true,  // isRaining (sensor became WET)
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_TRUE(result.rainDetected);
    TEST_ASSERT_FALSE(result.timeoutOccurred);
}

void test_watering_continues_when_sensor_dry(void) {
    unsigned long wateringStartTime = 3000;
    unsigned long currentTime = 6000;  // 3 seconds of watering
    unsigned long lastRainCheck = 5800;  // 200ms ago (> 100ms interval)

    ProcessResult result = processValveLogic(
        PHASE_WATERING,
        currentTime,
        2000,  // valveOpenTime
        wateringStartTime,
        lastRainCheck,
        false, // isRaining (sensor still DRY)
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_READ_SENSOR, result.action);
    TEST_ASSERT_EQUAL(currentTime, result.newLastRainCheck);
}

void test_watering_timeout_normal(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = 26000;  // 25 seconds elapsed (MAX_WATERING_TIME)

    ProcessResult result = processValveLogic(
        PHASE_WATERING,
        currentTime,
        500,   // valveOpenTime
        wateringStartTime,
        25900, // lastRainCheck
        false, // isRaining
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
}

void test_watering_timeout_emergency(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = 31000;  // 30 seconds elapsed (ABSOLUTE_SAFETY_TIMEOUT)

    ProcessResult result = processValveLogic(
        PHASE_WATERING,
        currentTime,
        500,   // valveOpenTime
        wateringStartTime,
        30900, // lastRainCheck
        false, // isRaining
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_EMERGENCY_STOP, result.action);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
}

void test_watering_manual_stop(void) {
    unsigned long wateringStartTime = 3000;
    unsigned long currentTime = 5000;  // 2 seconds of watering
    unsigned long lastRainCheck = 4800;  // 200ms ago (> 100ms interval)

    ProcessResult result = processValveLogic(
        PHASE_WATERING,
        currentTime,
        2000,  // valveOpenTime
        wateringStartTime,
        lastRainCheck,
        false, // isRaining
        false, // wateringRequested (MANUAL STOP)
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_EQUAL(0, result.newWateringStartTime);
}

void test_watering_waits_for_sensor_check_interval(void) {
    unsigned long wateringStartTime = 3000;
    unsigned long currentTime = 5000;
    unsigned long lastRainCheck = 4950;  // Only 50ms ago (< 100ms interval)

    ProcessResult result = processValveLogic(
        PHASE_WATERING,
        currentTime,
        2000,  // valveOpenTime
        wateringStartTime,
        lastRainCheck,
        false, // isRaining
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);
}

// ========== PHASE_CLOSING_VALVE Tests ==========

void test_closing_valve_returns_to_idle(void) {
    ProcessResult result = processValveLogic(
        PHASE_CLOSING_VALVE,
        10000, // currentTime
        5000,  // valveOpenTime
        6000,  // wateringStartTime
        9800,  // lastRainCheck
        true,  // isRaining
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_EQUAL(0, result.newWateringStartTime);
}

// ========== PHASE_ERROR Tests ==========

void test_error_phase_recovers_to_idle(void) {
    ProcessResult result = processValveLogic(
        PHASE_ERROR,
        10000, // currentTime
        5000,  // valveOpenTime
        6000,  // wateringStartTime
        9800,  // lastRainCheck
        false, // isRaining
        false, // wateringRequested
        VALVE_STABILIZATION_DELAY,
        RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME,
        ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_EQUAL(0, result.newWateringStartTime);
}

// ========== Full Cycle Tests ==========

void test_full_successful_watering_cycle(void) {
    unsigned long time = 1000;

    // 1. Start with PHASE_OPENING_VALVE
    ProcessResult r1 = processValveLogic(PHASE_OPENING_VALVE, time, 0, 0, 0, false, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, r1.newPhase);
    TEST_ASSERT_EQUAL(ACTION_OPEN_VALVE, r1.action);

    // 2. Wait for stabilization (500ms)
    time = 1500;
    ProcessResult r2 = processValveLogic(PHASE_WAITING_STABILIZATION, time, r1.newValveOpenTime,
                                         0, 0, false, true, VALVE_STABILIZATION_DELAY,
                                         RAIN_CHECK_INTERVAL, MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, r2.newPhase);

    // 3. Check initial sensor (dry)
    time = 1600;
    ProcessResult r3 = processValveLogic(PHASE_CHECKING_INITIAL_RAIN, time, r1.newValveOpenTime,
                                         0, r2.newLastRainCheck, false, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_WATERING, r3.newPhase);
    TEST_ASSERT_EQUAL(ACTION_TURN_PUMP_ON, r3.action);

    // 4. Water until sensor becomes wet (3 seconds)
    time = 4600;
    ProcessResult r4 = processValveLogic(PHASE_WATERING, time, r1.newValveOpenTime,
                                         r3.newWateringStartTime, r3.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, r4.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, r4.action);
    TEST_ASSERT_TRUE(r4.rainDetected);
    TEST_ASSERT_FALSE(r4.timeoutOccurred);

    // 5. Close valve and return to idle
    ProcessResult r5 = processValveLogic(PHASE_CLOSING_VALVE, time, r1.newValveOpenTime,
                                         r3.newWateringStartTime, r4.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_IDLE, r5.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, r5.action);
}

void test_full_already_wet_cycle(void) {
    unsigned long time = 1000;

    // 1. Start with PHASE_OPENING_VALVE
    ProcessResult r1 = processValveLogic(PHASE_OPENING_VALVE, time, 0, 0, 0, false, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, r1.newPhase);

    // 2. Wait for stabilization
    time = 1500;
    ProcessResult r2 = processValveLogic(PHASE_WAITING_STABILIZATION, time, r1.newValveOpenTime,
                                         0, 0, false, true, VALVE_STABILIZATION_DELAY,
                                         RAIN_CHECK_INTERVAL, MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, r2.newPhase);

    // 3. Check initial sensor (WET - tray already full)
    time = 1600;
    ProcessResult r3 = processValveLogic(PHASE_CHECKING_INITIAL_RAIN, time, r1.newValveOpenTime,
                                         0, r2.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, r3.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, r3.action);
    TEST_ASSERT_TRUE(r3.rainDetected);

    // 4. Close valve and return to idle (pump never turned on)
    ProcessResult r4 = processValveLogic(PHASE_CLOSING_VALVE, time, r1.newValveOpenTime,
                                         0, r3.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_IDLE, r4.newPhase);
}

// ========== Main Test Runner ==========

int main(int argc, char **argv) {
    UNITY_BEGIN();

    // PHASE_IDLE tests
    RUN_TEST(test_idle_phase_does_nothing);

    // PHASE_OPENING_VALVE tests
    RUN_TEST(test_opening_valve_transitions_to_stabilization);

    // PHASE_WAITING_STABILIZATION tests
    RUN_TEST(test_stabilization_waits_for_delay);
    RUN_TEST(test_stabilization_transitions_after_delay);

    // PHASE_CHECKING_INITIAL_RAIN tests
    RUN_TEST(test_initial_rain_check_sensor_dry_starts_watering);
    RUN_TEST(test_initial_rain_check_sensor_wet_skips_watering);
    RUN_TEST(test_initial_rain_check_waits_for_interval);

    // PHASE_WATERING tests
    RUN_TEST(test_watering_completes_when_sensor_wet);
    RUN_TEST(test_watering_continues_when_sensor_dry);
    RUN_TEST(test_watering_timeout_normal);
    RUN_TEST(test_watering_timeout_emergency);
    RUN_TEST(test_watering_manual_stop);
    RUN_TEST(test_watering_waits_for_sensor_check_interval);

    // PHASE_CLOSING_VALVE tests
    RUN_TEST(test_closing_valve_returns_to_idle);

    // PHASE_ERROR tests
    RUN_TEST(test_error_phase_recovers_to_idle);

    // Full cycle tests
    RUN_TEST(test_full_successful_watering_cycle);
    RUN_TEST(test_full_already_wet_cycle);

    return UNITY_END();
}

#endif // NATIVE_TEST

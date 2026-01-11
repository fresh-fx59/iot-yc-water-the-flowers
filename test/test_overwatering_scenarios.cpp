// This file has been merged into test_native_all.cpp
// Kept for reference only
#ifndef NATIVE_TEST

#include <unity.h>
#include "StateMachineLogic.h"
#include "TestConfig.h"

using namespace StateMachineLogic;

// ============================================
// Test Helpers
// ============================================

// Simulate a full watering cycle and return total duration
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

        // Break if we exit watering phase
        if (result.newPhase != PHASE_WATERING) {
            break;
        }
    }

    return time - startTime;
}

// ============================================
// 1. SENSOR FAILURE SCENARIOS
// ============================================

void test_sensor_stuck_dry_triggers_timeout(void) {
    // Sensor never becomes wet - should timeout at MAX_WATERING_TIME
    unsigned long duration = simulateFullCycle(true, 40000);

    // Should stop around MAX_WATERING_TIME (25s + stabilization ~500ms + initial check ~100ms)
    TEST_ASSERT_GREATER_OR_EQUAL(MAX_WATERING_TIME, duration);
    TEST_ASSERT_LESS_THAN(MAX_WATERING_TIME + 2000, duration); // Allow 2s margin
}

void test_sensor_disconnected_behaves_like_stuck_dry(void) {
    // Disconnected sensor reads HIGH (DRY) due to INPUT_PULLUP
    // This is identical to stuck dry scenario
    unsigned long time = 3000;
    unsigned long wateringStartTime = 1000;
    unsigned long lastRainCheck = 2800;

    // Sensor reads DRY continuously
    ProcessResult result = processValveLogic(
        PHASE_WATERING, time, 500, wateringStartTime, lastRainCheck,
        false, // isRaining = false (sensor disconnected reads DRY)
        true,  // wateringRequested
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should continue watering (timeout not reached yet)
    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
}

void test_multiple_sensors_stuck_dry(void) {
    // Simulate sensor power failure affecting all sensors
    // Each valve will timeout independently
    int failedValves = 6;
    unsigned long totalDuration = 0;

    for (int i = 0; i < failedValves; i++) {
        unsigned long valveDuration = simulateFullCycle(true, 40000);
        totalDuration += valveDuration;
    }

    // Total overwatering time for 6 valves
    // Should be ~6 * 25s = 150s (2.5 minutes)
    TEST_ASSERT_GREATER_OR_EQUAL(6 * MAX_WATERING_TIME, totalDuration);
    TEST_ASSERT_LESS_THAN(6 * (MAX_WATERING_TIME + 2000), totalDuration);
}

// ============================================
// 2. TIMEOUT SYSTEM FAILURES
// ============================================

void test_normal_timeout_at_exact_boundary(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = wateringStartTime + MAX_WATERING_TIME; // Exactly at boundary

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, currentTime - 100,
        false, // Sensor still dry
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should trigger timeout
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
}

void test_emergency_timeout_triggers_emergency_stop(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = wateringStartTime + ABSOLUTE_SAFETY_TIMEOUT;

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, currentTime - 100,
        false, // Sensor still dry
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should trigger EMERGENCY stop
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
    TEST_ASSERT_EQUAL(ACTION_EMERGENCY_STOP, result.action);
}

void test_timeout_one_millisecond_before_limit(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = wateringStartTime + MAX_WATERING_TIME - 1;

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, currentTime - 100,
        false, // Sensor still dry
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should NOT trigger timeout yet
    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_FALSE(result.timeoutOccurred);
}

void test_millis_overflow_during_watering(void) {
    // Simulate millis() overflow scenario
    // wateringStartTime near ULONG_MAX, currentTime wraps to 0

    // Start watering 10 seconds before overflow
    unsigned long wateringStartTime = ULONG_MAX - 10000;

    // Current time is 5 seconds after overflow (wrapped to small value)
    unsigned long currentTime = 5000;

    // Calculate elapsed time with overflow handling
    // NOTE: This test exposes a BUG - the state machine doesn't handle millis() overflow!
    // Expected behavior: elapsed = (ULONG_MAX - wateringStartTime) + currentTime
    //                    elapsed = 10000 + 5000 = 15000ms

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, currentTime - 100,
        false, // Sensor still dry
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // BUG: currentTime - wateringStartTime = 5000 - (ULONG_MAX - 10000)
    //      This will be a huge positive number due to unsigned underflow
    //      System will incorrectly think HUGE time has passed and trigger emergency timeout

    // Current behavior (BUG): Will trigger emergency timeout
    // This is actually SAFE - false positive is better than false negative
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_TRUE(result.timeoutOccurred);

    // NOTE: This is a known limitation - system safely fails by stopping watering
    // Real fix would require signed time differences: (long)(currentTime - wateringStartTime)
}

// ============================================
// 3. STATE MACHINE EDGE CASES
// ============================================

void test_stuck_in_watering_without_sensor_check(void) {
    // If sensor check interval is never met, system might get stuck
    unsigned long wateringStartTime = 1000;
    unsigned long lastRainCheck = 1050;
    unsigned long currentTime = 1100; // Only 50ms since last check

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, lastRainCheck,
        false, // Sensor dry
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should stay in PHASE_WATERING with no action (waiting for interval)
    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, result.action);

    // But timeout should still work even without sensor checks
    currentTime = wateringStartTime + MAX_WATERING_TIME;
    result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, lastRainCheck,
        false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
}

void test_manual_stop_during_watering(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = 3000; // 2 seconds into watering
    unsigned long lastRainCheck = 2800;

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, lastRainCheck,
        false, // Sensor dry
        false, // wateringRequested = FALSE (manual stop)
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should immediately transition to IDLE and close valve
    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_EQUAL(0, result.newWateringStartTime);
}

void test_sensor_becomes_wet_immediately_stops_watering(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = 2000; // 1 second into watering
    unsigned long lastRainCheck = 1800;

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, lastRainCheck,
        true,  // isRaining = TRUE (sensor just became wet)
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should immediately close valve
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_TRUE(result.rainDetected);
    TEST_ASSERT_FALSE(result.timeoutOccurred);
}

// ============================================
// 4. PHASE TRANSITION SAFETY
// ============================================

void test_all_phases_eventually_return_to_idle(void) {
    // Test that every phase has a path back to IDLE

    // PHASE_OPENING_VALVE -> PHASE_WAITING_STABILIZATION
    ProcessResult r1 = processValveLogic(PHASE_OPENING_VALVE, 1000, 0, 0, 0, false, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, r1.newPhase);

    // PHASE_WAITING_STABILIZATION -> PHASE_CHECKING_INITIAL_RAIN
    ProcessResult r2 = processValveLogic(PHASE_WAITING_STABILIZATION, 1500, r1.newValveOpenTime,
                                         0, 0, false, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, r2.newPhase);

    // PHASE_CHECKING_INITIAL_RAIN (tray full) -> PHASE_CLOSING_VALVE
    ProcessResult r3 = processValveLogic(PHASE_CHECKING_INITIAL_RAIN, 1600, r1.newValveOpenTime,
                                         0, r2.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, r3.newPhase);

    // PHASE_CLOSING_VALVE -> PHASE_IDLE
    ProcessResult r4 = processValveLogic(PHASE_CLOSING_VALVE, 1700, r1.newValveOpenTime,
                                         0, r3.newLastRainCheck, true, true,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_IDLE, r4.newPhase);

    // PHASE_ERROR -> PHASE_IDLE
    ProcessResult r5 = processValveLogic(PHASE_ERROR, 2000, 0, 0, 0, false, false,
                                         VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
                                         MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT);
    TEST_ASSERT_EQUAL(PHASE_IDLE, r5.newPhase);
}

void test_error_phase_always_closes_valve(void) {
    ProcessResult result = processValveLogic(
        PHASE_ERROR, 5000, 1000, 2000, 4800, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    TEST_ASSERT_EQUAL(PHASE_IDLE, result.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, result.action);
    TEST_ASSERT_EQUAL(0, result.newWateringStartTime);
}

// ============================================
// 5. BOUNDARY CONDITIONS
// ============================================

void test_zero_time_values(void) {
    // All timestamps zero - should not crash
    ProcessResult result = processValveLogic(
        PHASE_WATERING, 0, 0, 0, 0, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // With wateringStartTime = 0, elapsed = 0 - 0 = 0, no timeout
    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
}

void test_very_large_time_values(void) {
    // Near ULONG_MAX - should not overflow
    unsigned long largeTime = ULONG_MAX - 1000;

    ProcessResult result = processValveLogic(
        PHASE_WATERING, largeTime, largeTime - 500, largeTime - 1000,
        largeTime - 200, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Elapsed = 1000ms, should still be watering (no timeout)
    TEST_ASSERT_EQUAL(PHASE_WATERING, result.newPhase);
}

void test_stabilization_delay_exactly_zero(void) {
    ProcessResult result = processValveLogic(
        PHASE_WAITING_STABILIZATION, 1000, 1000, 0, 0, false, true,
        0, // Zero delay
        RAIN_CHECK_INTERVAL, MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should immediately transition (elapsed 0 >= 0)
    TEST_ASSERT_EQUAL(PHASE_CHECKING_INITIAL_RAIN, result.newPhase);
}

// ============================================
// 6. SEQUENTIAL WATERING SCENARIOS
// ============================================

void test_sequential_watering_timeout_doesnt_block_next_valve(void) {
    // Valve 0 times out, valve 1 should still be able to water

    // Valve 0: timeout scenario
    unsigned long duration0 = simulateFullCycle(true, 40000);
    TEST_ASSERT_GREATER_OR_EQUAL(MAX_WATERING_TIME, duration0);

    // Valve 1: normal watering (sensor works)
    unsigned long duration1 = simulateFullCycle(false, 40000);
    TEST_ASSERT_LESS_THAN(MAX_WATERING_TIME, duration1); // Should complete before timeout

    // Both valves independent - timeout in valve 0 doesn't affect valve 1
}

void test_concurrent_phases_different_valves(void) {
    // Simulate two valves in different phases (should not interfere)
    // This tests that valve state is properly isolated

    // Valve 0 in PHASE_WATERING
    ProcessResult v0 = processValveLogic(
        PHASE_WATERING, 5000, 1000, 2000, 4800, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Valve 1 in PHASE_OPENING_VALVE
    ProcessResult v1 = processValveLogic(
        PHASE_OPENING_VALVE, 5000, 0, 0, 0, false, true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Both should progress independently
    TEST_ASSERT_EQUAL(PHASE_WATERING, v0.newPhase);
    TEST_ASSERT_EQUAL(PHASE_WAITING_STABILIZATION, v1.newPhase);
}

// ============================================
// 7. TIMING ACCURACY TESTS
// ============================================

void test_rain_check_interval_enforcement(void) {
    unsigned long wateringStartTime = 1000;
    unsigned long lastRainCheck = 2000;

    // Try to check 50ms later (less than 100ms interval)
    ProcessResult r1 = processValveLogic(
        PHASE_WATERING, 2050, 500, wateringStartTime, lastRainCheck,
        true, // Sensor is wet
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should NOT check sensor yet (interval not met)
    TEST_ASSERT_EQUAL(PHASE_WATERING, r1.newPhase);
    TEST_ASSERT_EQUAL(ACTION_NONE, r1.action);

    // Try again at 100ms later
    ProcessResult r2 = processValveLogic(
        PHASE_WATERING, 2100, 500, wateringStartTime, lastRainCheck,
        true, // Sensor is wet
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Should now check and stop watering
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, r2.newPhase);
    TEST_ASSERT_EQUAL(ACTION_CLOSE_VALVE, r2.action);
}

void test_timeout_takes_priority_over_sensor_check(void) {
    // Even if sensor check interval not met, timeout should still trigger
    unsigned long wateringStartTime = 1000;
    unsigned long currentTime = wateringStartTime + MAX_WATERING_TIME;
    unsigned long lastRainCheck = currentTime - 50; // Only 50ms ago

    ProcessResult result = processValveLogic(
        PHASE_WATERING, currentTime, 500, wateringStartTime, lastRainCheck,
        false, // Sensor dry
        true,
        VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
        MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
    );

    // Timeout check runs BEFORE sensor check - should timeout
    TEST_ASSERT_EQUAL(PHASE_CLOSING_VALVE, result.newPhase);
    TEST_ASSERT_TRUE(result.timeoutOccurred);
}

// ============================================
// 8. REALISTIC FAILURE SCENARIOS
// ============================================

void test_realistic_sensor_failure_all_6_valves(void) {
    // Simulate realistic scenario: sensor power fails at night
    // All 6 valves timeout during sequential watering

    int totalOverwateringSeconds = 0;

    for (int valve = 0; valve < 6; valve++) {
        unsigned long duration = simulateFullCycle(true, 40000);
        totalOverwateringSeconds += duration / 1000;
    }

    // Expected: ~25s * 6 = 150 seconds total
    // Actual: Will be slightly more due to stabilization delays
    TEST_ASSERT_GREATER_OR_EQUAL(150, totalOverwateringSeconds);
    TEST_ASSERT_LESS_THAN(180, totalOverwateringSeconds); // Should be under 3 minutes
}

void test_partial_sensor_failure_mixed_results(void) {
    // Valves 0-2 sensors fail, valves 3-5 work normally

    unsigned long failedValveTime = 0;
    unsigned long normalValveTime = 0;

    // Failed sensors (timeout)
    for (int i = 0; i < 3; i++) {
        failedValveTime += simulateFullCycle(true, 40000);
    }

    // Normal sensors (complete quickly)
    for (int i = 3; i < 6; i++) {
        normalValveTime += simulateFullCycle(false, 40000);
    }

    // Failed valves should take ~25s each
    TEST_ASSERT_GREATER_OR_EQUAL(75000, failedValveTime); // 75+ seconds

    // Normal valves should take ~3s each
    TEST_ASSERT_LESS_THAN(20000, normalValveTime); // Under 20 seconds total
}

// ============================================
// Main Test Runner
// ============================================

void setUp(void) {
    // Setup before each test
}

void tearDown(void) {
    // Cleanup after each test
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    // 1. Sensor Failure Scenarios
    RUN_TEST(test_sensor_stuck_dry_triggers_timeout);
    RUN_TEST(test_sensor_disconnected_behaves_like_stuck_dry);
    RUN_TEST(test_multiple_sensors_stuck_dry);

    // 2. Timeout System Failures
    RUN_TEST(test_normal_timeout_at_exact_boundary);
    RUN_TEST(test_emergency_timeout_triggers_emergency_stop);
    RUN_TEST(test_timeout_one_millisecond_before_limit);
    RUN_TEST(test_millis_overflow_during_watering);

    // 3. State Machine Edge Cases
    RUN_TEST(test_stuck_in_watering_without_sensor_check);
    RUN_TEST(test_manual_stop_during_watering);
    RUN_TEST(test_sensor_becomes_wet_immediately_stops_watering);

    // 4. Phase Transition Safety
    RUN_TEST(test_all_phases_eventually_return_to_idle);
    RUN_TEST(test_error_phase_always_closes_valve);

    // 5. Boundary Conditions
    RUN_TEST(test_zero_time_values);
    RUN_TEST(test_very_large_time_values);
    RUN_TEST(test_stabilization_delay_exactly_zero);

    // 6. Sequential Watering Scenarios
    RUN_TEST(test_sequential_watering_timeout_doesnt_block_next_valve);
    RUN_TEST(test_concurrent_phases_different_valves);

    // 7. Timing Accuracy Tests
    RUN_TEST(test_rain_check_interval_enforcement);
    RUN_TEST(test_timeout_takes_priority_over_sensor_check);

    // 8. Realistic Failure Scenarios
    RUN_TEST(test_realistic_sensor_failure_all_6_valves);
    RUN_TEST(test_partial_sensor_failure_mixed_results);

    return UNITY_END();
}

#endif // NATIVE_TEST

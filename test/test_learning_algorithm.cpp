// This test file is deprecated - use test_native_all.cpp instead
// Keeping for reference only
#ifndef NATIVE_TEST

#include <unity.h>
#include <ArduinoFake.h>
#include "LearningAlgorithm.h"

using namespace fakeit;

void setUp(void) {
    // ArduinoFake setup if needed
}

void tearDown(void) {
    // ArduinoFake cleanup if needed
}

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
    // Use String from ArduinoFake
    
    // 1. Seconds
    TEST_ASSERT_EQUAL_STRING("5.5s", LearningAlgorithm::formatDuration(5500).c_str());
    
    // 2. Minutes
    TEST_ASSERT_EQUAL_STRING("2m 30s", LearningAlgorithm::formatDuration(150000).c_str());
    
    // 3. Hours
    TEST_ASSERT_EQUAL_STRING("1h 30m", LearningAlgorithm::formatDuration(5400000).c_str());
    
    // 4. Days
    TEST_ASSERT_EQUAL_STRING("2d 2h", LearningAlgorithm::formatDuration(180000000).c_str());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_calculate_water_level);
    RUN_TEST(test_calculate_empty_duration);
    RUN_TEST(test_format_duration);
    return UNITY_END();
}

#endif // NATIVE_TEST

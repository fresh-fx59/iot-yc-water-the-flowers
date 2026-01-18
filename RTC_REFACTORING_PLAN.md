# RTC-Based Timestamp Refactoring Plan

## Problem Statement

**Current Issue (v1.15.2):**
After short power cycles (10-20 seconds), the system incorrectly detects valves as "overdue" and waters immediately, even when the schedule shows watering should occur days later (e.g., January 16-17).

**Root Cause:**
The system stores timestamps as `millis()` values, which reset to 0 on every reboot. The loading logic tries to convert old timestamps to the new millis() epoch using offset calculations. When the device has only been running for 21 seconds since boot:
- `currentMillis` = 21,600 ms (21.6 seconds)
- `timeSinceWatering` = 8h 45m + 24h+ = ~33+ hours = 118,800,000+ ms
- Check: `21,600 >= 118,800,000`? **NO**
- Result: Sets `lastWateringCompleteTime = 0`
- `hasOverdueValves()` sees timestamp=0 and thinks "too old to represent" → waters immediately

**Temporary Fix (v1.15.3):**
Removed the CASE 1 check in `hasOverdueValves()` that treated `lastWateringCompleteTime == 0` as "overdue". Now relies on auto-watering (24h minimum interval) instead of boot watering for these cases.

## Proposed Solution: RTC-Based Timestamps

### Architecture

The system already has:
- DS3231 RTC with CR2032 battery (time persists across power loss)
- RTC sync at boot: `DS3231RTC::setSystemTimeFromRTC()` in main.cpp:309
- Standard C `time(nullptr)` returns unix epoch seconds

### Changes Required

#### 1. ValveController.h (Lines 47-53)
**Current:**
```cpp
unsigned long lastWateringCompleteTime; // millis()
unsigned long lastWateringAttemptTime;  // millis()
unsigned long emptyToFullDuration;      // milliseconds
```

**Proposed:**
```cpp
time_t lastWateringCompleteTime;    // Unix epoch seconds (persists across reboots)
time_t lastWateringAttemptTime;     // Unix epoch seconds
unsigned long emptyToFullDuration;  // seconds (not milliseconds!)
```

**Impact:**
- Add `#include <time.h>` to ValveController.h:10
- Change constructor initialization (line 73)
- **CRITICAL:** `emptyToFullDuration` changes from milliseconds to seconds
  - This affects ~15-20 calculation sites
  - Currently: `86400000` ms = 24 hours
  - Proposed: `86400` seconds = 24 hours

#### 2. WateringSystem.h - saveLearningData() (Lines 246-293)

**Current:**
```cpp
doc["savedAtMillis"] = millis();
time_t now;
time(&now);
doc["savedAtRealTime"] = (unsigned long)now;

valveObj["lastWateringCompleteTime"] = (unsigned long)valve->lastWateringCompleteTime;
valveObj["lastWateringAttemptTime"] = (unsigned long)valve->lastWateringAttemptTime;
valveObj["emptyToFullDuration"] = (unsigned long)valve->emptyToFullDuration;
```

**Proposed:**
```cpp
// No longer need savedAtMillis or offset calculation
time_t now;
time(&now);
doc["savedAtRealTime"] = (unsigned long)now; // For debugging only

valveObj["lastWateringCompleteTime"] = (unsigned long)valve->lastWateringCompleteTime;
valveObj["lastWateringAttemptTime"] = (unsigned long)valve->lastWateringAttemptTime;
valveObj["emptyToFullDuration"] = (unsigned long)valve->emptyToFullDuration; // Now seconds
```

#### 3. WateringSystem.h - loadLearningData() (Lines 295-422)

**Current:** Complex offset calculation (lines 328-412):
```cpp
unsigned long savedAtMillis = doc["savedAtMillis"] | 0;
unsigned long savedAtRealTime = doc["savedAtRealTime"] | 0;
unsigned long currentMillis = millis();
time_t currentRealTime;
time(&currentRealTime);

// Calculate time offset using real time...
if (savedAtRealTime > 0 && currentRealTime > 1000000000) {
    // Complex epoch conversion logic
    timeOffsetMs = elapsedSeconds * 1000UL;
} else if (currentMillis >= savedAtMillis) {
    // Fallback to millis()
    timeOffsetMs = currentMillis - savedAtMillis;
} else {
    // Reboot detected
    timeOffsetMs = currentMillis;
}

// Convert saved millis() to current millis() epoch
if (savedCompleteTime > 0 && savedAtMillis > 0) {
    unsigned long timeFromWateringToSave = savedAtMillis - savedCompleteTime;
    unsigned long timeSinceWatering = timeFromWateringToSave + timeOffsetMs;
    if (currentMillis >= timeSinceWatering) {
        valve->lastWateringCompleteTime = currentMillis - timeSinceWatering;
    } else {
        valve->lastWateringCompleteTime = 0; // BUG: This triggers false "overdue"
    }
}
```

**Proposed:** Direct load (simple!):
```cpp
// No offset calculation needed - timestamps are already in unix epoch
valve->lastWateringCompleteTime = valveObj["lastWateringCompleteTime"] | 0;
valve->lastWateringAttemptTime = valveObj["lastWateringAttemptTime"] | 0;
valve->emptyToFullDuration = valveObj["emptyToFullDuration"] | 0; // seconds
```

#### 4. ValveController.h - shouldWaterNow() (Lines 137-184)

**Current:**
```cpp
inline bool shouldWaterNow(const ValveController *valve, unsigned long currentTime) {
    // currentTime = millis()
    if (valve->lastWateringCompleteTime > currentTime) {
        return false; // Future timestamp (reboot)
    }

    unsigned long timeSinceLastAttempt = currentTime - valve->lastWateringAttemptTime;
    if (timeSinceLastAttempt < AUTO_WATERING_MIN_INTERVAL_MS) { // 86400000 ms
        return false;
    }

    unsigned long timeSinceLastWatering = currentTime - valve->lastWateringCompleteTime;
    return timeSinceLastWatering >= valve->emptyToFullDuration;
}
```

**Proposed:**
```cpp
inline bool shouldWaterNow(const ValveController *valve, time_t currentTime) {
    // currentTime = time(nullptr)
    if (valve->lastWateringCompleteTime > currentTime) {
        return false; // Future timestamp (clock error)
    }

    time_t timeSinceLastAttempt = currentTime - valve->lastWateringAttemptTime;
    if (timeSinceLastAttempt < AUTO_WATERING_MIN_INTERVAL_SEC) { // 86400 seconds
        return false;
    }

    time_t timeSinceLastWatering = currentTime - valve->lastWateringCompleteTime;
    return timeSinceLastWatering >= valve->emptyToFullDuration;
}
```

#### 5. WateringSystem.h - hasOverdueValves() (Lines 1893-1930)

**Current:**
```cpp
inline bool WateringSystem::hasOverdueValves() {
    unsigned long currentTime = millis();
    // ... checks with millis() arithmetic
}
```

**Proposed:**
```cpp
inline bool WateringSystem::hasOverdueValves() {
    time_t currentTime = time(nullptr);

    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController *valve = valves[i];

        if (!valve->autoWateringEnabled || !valve->isCalibrated) {
            continue;
        }

        if (valve->emptyToFullDuration > 0 && valve->lastWateringCompleteTime > 0) {
            // No future timestamp check needed - RTC time is stable
            time_t nextWateringTime = valve->lastWateringCompleteTime + valve->emptyToFullDuration;

            if (currentTime >= nextWateringTime) {
                return true; // Genuinely overdue
            }
        }
    }

    return false;
}
```

#### 6. All Call Sites Using Timestamps

**Files to update:**
- `WateringSystemStateMachine.h`: processValve() - change millis() to time(nullptr)
- `WateringSystem.h`:
  - checkAutoWatering() - line 814
  - processLearningData() - lines 1317-1347, 1394-1395, 1482
  - calculateCurrentWaterLevel() calls - lines 893, 1632
  - sendWateringSchedule() - lines 1799-1873

**Pattern:**
```cpp
// OLD
unsigned long currentTime = millis();
unsigned long timeSince = currentTime - valve->lastWateringCompleteTime;

// NEW
time_t currentTime = time(nullptr);
time_t timeSince = currentTime - valve->lastWateringCompleteTime;
```

#### 7. config.h Constants (Lines 86-98)

**Current:**
```cpp
const unsigned long BASE_INTERVAL_MS = 86400000; // 24 hours in milliseconds
const unsigned long AUTO_WATERING_MIN_INTERVAL_MS = 86400000;
const unsigned long UNCALIBRATED_RETRY_INTERVAL_MS = 86400000;
const unsigned long RECENT_WATERING_THRESHOLD_MS = 7200000; // 2 hours
```

**Proposed:**
```cpp
const unsigned long BASE_INTERVAL_SEC = 86400; // 24 hours in seconds
const unsigned long AUTO_WATERING_MIN_INTERVAL_SEC = 86400;
const unsigned long UNCALIBRATED_RETRY_INTERVAL_SEC = 86400;
const unsigned long RECENT_WATERING_THRESHOLD_SEC = 7200; // 2 hours
```

#### 8. LearningAlgorithm.h - calculateCurrentWaterLevel() (Lines 107-125)

**Current:**
```cpp
inline float calculateCurrentWaterLevel(const ValveController *valve, unsigned long currentTime) {
    unsigned long timeSinceLastWatering = currentTime - valve->lastWateringCompleteTime;
    float consumedPercent = (float)timeSinceLastWatering / (float)valve->emptyToFullDuration * 100.0f;
    // ...
}
```

**Proposed:**
```cpp
inline float calculateCurrentWaterLevel(const ValveController *valve, time_t currentTime) {
    time_t timeSinceLastWatering = currentTime - valve->lastWateringCompleteTime;
    float consumedPercent = (float)timeSinceLastWatering / (float)valve->emptyToFullDuration * 100.0f;
    // ... (same calculation, different units)
}
```

### Migration Strategy

**Option A: Clean Break (Recommended)**
1. Increment version to 1.16.0 (breaking change)
2. Change learning data filename to `/learning_data_v1.16.0.json`
3. Auto-delete old learning data on boot (existing pattern)
4. All valves recalibrate from scratch (1-2 weeks)

**Option B: Data Migration**
1. Load old data (milliseconds)
2. Convert emptyToFullDuration: `oldValue / 1000` → seconds
3. Convert timestamps using RTC offset (if available)
4. Save in new format
5. More complex, risk of errors

### Benefits

1. **Robustness**: Works correctly after any power cycle duration (seconds to weeks)
2. **Simplicity**: Removes ~50 lines of complex offset calculation logic
3. **Correctness**: No more false "overdue" detections
4. **Clarity**: Unix epoch time is industry standard, easier to understand
5. **Future-proof**: Foundation for features requiring absolute time (schedules, logs)

### Risks

1. **Testing burden**: Need to verify all timestamp calculations
2. **Data loss**: All existing calibration data lost (1-2 weeks to recalibrate)
3. **RTC dependency**: If RTC fails, system loses all timing (already exists)
4. **Precision**: Second precision vs millisecond (negligible for multi-hour intervals)

### Testing Checklist

- [ ] Verify boot watering logic (first boot, overdue, on-schedule)
- [ ] Verify auto-watering triggers at correct times
- [ ] Verify learning algorithm calculations
- [ ] Verify schedule display shows correct planned times
- [ ] Test power cycle after 1 minute running
- [ ] Test power cycle after 1 hour running
- [ ] Test power cycle after 1 day running
- [ ] Verify MQTT state publishes correct timestamps
- [ ] Verify persistence (save/load cycle)
- [ ] Test RTC battery removal (time loss scenario)

### Estimated Effort

- **Code changes**: ~40 files touched, ~100 lines modified
- **Testing**: 2-3 hours (simulate various scenarios)
- **Calibration**: 1-2 weeks for all valves to relearn
- **Total development time**: 4-6 hours

## Conclusion

The RTC refactoring is the **architecturally correct** solution but requires significant work. The immediate fix (v1.15.3) solves the reported issue with minimal risk. Consider RTC refactoring for v1.16.0 when there's time for thorough testing.

## Related Files

- Current fix: `WateringSystem.h:1893-1930` (hasOverdueValves)
- Problematic loading logic: `WateringSystem.h:295-422` (loadLearningData)
- Constants: `config.h:86-98`
- Helper functions: `ValveController.h:107-184`
- State machine: `WateringSystemStateMachine.h`

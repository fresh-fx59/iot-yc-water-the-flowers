# Timeout Retry Feature for Uncalibrated Valves

## Overview
Implemented automatic 24-hour retry scheduling when a timeout occurs during the first watering attempt of an uncalibrated valve.

## Problem
Previously, when a timeout occurred during the first watering of an uncalibrated valve:
- The system skipped learning and returned early
- No retry schedule was set up
- The valve remained uncalibrated with no auto-watering enabled
- Manual intervention was required to retry

## Solution
When a timeout occurs on an uncalibrated valve's first watering attempt, the system now:
1. Sets up minimal learning data to enable auto-watering
2. Schedules the next watering attempt in exactly 24 hours
3. Keeps the valve marked as uncalibrated
4. When retry succeeds, establishes proper baseline calibration

## Implementation Details

### Changes to WateringSystem.h (lines 1397-1419)
**Location**: `processLearningData()` function

**Before**:
```cpp
if (valve->timeoutOccurred) {
  DebugHelper::debug("🧠 Skipping learning - timeout occurred");
  return;
}
```

**After**:
```cpp
if (valve->timeoutOccurred) {
  // SPECIAL CASE: First watering timeout on uncalibrated valve
  if (!valve->isCalibrated) {
    DebugHelper::debugImportant("⏰ TIMEOUT on first watering - scheduling 24h retry");

    valve->lastWateringAttemptTime = currentTime;
    valve->emptyToFullDuration = BASE_INTERVAL_MS; // 24 hours
    valve->intervalMultiplier = MIN_INTERVAL_MULTIPLIER; // 1.0x
    // Keep isCalibrated = false

    saveLearningData();
    sendScheduleUpdateIfNeeded();
    return;
  }

  // For calibrated valves, skip learning on timeout
  DebugHelper::debug("🧠 Skipping learning - timeout occurred (calibrated valve)");
  return;
}
```

### Changes to ValveController.h (lines 169-180)
**Location**: `shouldWaterNow()` function

**Added new logic block**:
```cpp
// TIMEOUT RETRY mode: Uncalibrated valve with timeout, retrying after 24h
// Has emptyToFullDuration set but no lastWateringCompleteTime (never succeeded)
// Relies on lastWateringAttemptTime 24h check above
if (!valve->isCalibrated && valve->lastWateringCompleteTime == 0 &&
    valve->lastWateringAttemptTime > 0) {
  // Already passed 24h minimum interval check above (line 160-166)
  // Safe to retry watering attempt
  return true;
}
```

## Behavior Flow

### Scenario: Timeout on First Watering
1. **Initial State**:
   - Valve is uncalibrated
   - User triggers manual watering or sequential watering
   - Timeout occurs (sensor doesn't detect wet condition in time)

2. **Timeout Handler Executes** (`processLearningData`):
   - Detects timeout on uncalibrated valve
   - Sets `lastWateringAttemptTime = currentTime`
   - Sets `emptyToFullDuration = 86400000` (24 hours)
   - Keeps `isCalibrated = false`
   - Saves learning data
   - Logs: "⏰ TIMEOUT on first watering - scheduling 24h retry"

3. **Auto-Watering Check** (`shouldWaterNow`):
   - Every loop checks if valve should water
   - Waits for 24 hours to pass since `lastWateringAttemptTime`
   - After 24h, returns `true` (triggers auto-watering)

4. **Retry Watering**:
   - System automatically waters the valve again
   - **If successful**: Establishes baseline, valve becomes calibrated
   - **If timeout again**: Reschedules another 24-hour retry

## Safety Features
- ✅ Maintains 24-hour minimum interval (prevents over-watering)
- ✅ Respects all existing safety mechanisms (overflow, water level, etc.)
- ✅ Persistent across reboots (learning data saved)
- ✅ No interference with calibrated valves
- ✅ Compatible with manual watering and sequential watering

## Testing
- ✅ All 30 existing native tests pass
- ✅ No breaking changes to existing functionality
- ✅ Backward compatible with existing learning data

## Version
- Implemented: v1.16.3 (pending)
- Files modified:
  - `include/WateringSystem.h`
  - `include/ValveController.h`

## Usage Example

### Serial Log Output
```
⏰ TIMEOUT on first watering - scheduling 24h retry
  Next attempt in: 24 hours
  Valve remains uncalibrated until successful watering

... 24 hours later ...

[Auto-watering triggers]
Starting watering for valve 0
...
🧠 ADAPTIVE LEARNING:
  ✨ INITIAL CALIBRATION: 15.2s
  Baseline will auto-update when tray is emptier
  Starting interval: 1.0x (24 hours)
```

## Notes
- Timeout flag is cleared after each watering cycle completes
- Multiple consecutive timeouts will each schedule 24-hour retries
- This feature only applies to uncalibrated valves
- Calibrated valves with timeout skip learning (existing behavior)

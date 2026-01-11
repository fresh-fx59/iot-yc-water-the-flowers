# Plan for Native Logic Testing (No Hardware Required)

- [x] **1. Create Pure Logic Header**
    - [x] Create `include/LearningAlgorithm.h`.
    - [x] Move the `LearningAlgorithm` namespace and its functions (`calculateWaterLevelBefore`, `calculateEmptyDuration`, `formatDuration`) from `include/WateringSystem.h` to this new file.
    - [x] Ensure this new file has **no** hardware dependencies (only `<Arduino.h>` or standard C++ types).

- [x] **2. Refactor `WateringSystem.h`**
    - [x] Include the new `LearningAlgorithm.h` in `include/WateringSystem.h`.
    - [x] Verify the project still compiles for the ESP32 (ensure no breaking changes).

- [x] **3. Configure PlatformIO for Native Testing**
    - [x] Edit `platformio.ini`.
    - [x] Add a `[env:native]` environment.
    - [x] Add `ArduinoFake` to `lib_deps` for the native environment (this allows using `String`, `millis`, etc. on desktop).

- [x] **4. Create Native Test Suite**
    - [x] Create `test/test_learning_algorithm.cpp`.
    - [x] Write unit tests for:
        - [x] `calculateWaterLevelBefore` (e.g., half fill time = 50% level).
        - [x] `calculateEmptyDuration` (e.g., consumption rate calculation).
        - [x] `formatDuration` (verify string formatting like "2h 30m").

- [x] **5. Run Native Tests**
    - [x] Execute `pio test -e native`.
    - [x] Verify all tests pass on the local machine (3/3 tests passed in 0.93s).

---

## ✅ Implementation Complete

All steps have been successfully implemented. You can now run native tests without uploading to ESP32:

```bash
pio test -e native
```

**Test Results:**

```
================= 20 test cases: 20 succeeded in 00:00:01.876 =================
```

*Learning Algorithm Tests (3 tests):*
- `test_calculate_water_level` - ✅ PASSED
- `test_calculate_empty_duration` - ✅ PASSED
- `test_format_duration` - ✅ PASSED

*State Machine Tests (17 tests):*
- All phase transitions - ✅ PASSED
- Timeout handling (normal & emergency) - ✅ PASSED
- Full watering cycles - ✅ PASSED

**Key Changes Made:**
1. Created `include/LearningAlgorithm.h` with pure logic functions (no hardware deps)
2. Created `include/StateMachineLogic.h` with testable state machine logic
3. Created `include/TestConfig.h` for native testing configuration (avoids duplicate symbols)
4. Updated `include/ValveController.h` to conditionally use TestConfig.h in NATIVE_TEST mode
5. Updated `include/LearningAlgorithm.h` to conditionally use TestConfig.h in NATIVE_TEST mode
6. Refactored `include/WateringSystem.h` to use the new headers
7. Added `[env:native]` to `platformio.ini` with ArduinoFake dependency
8. Created comprehensive unit tests:
   - `test/test_native_all.cpp` - Combined test suite (learning algorithm + state machine)
   - `test/test_learning_algorithm.cpp` - Deprecated (kept for reference)
   - `test/test_state_machine.cpp` - Deprecated (kept for reference)
9. Wrapped deprecated test files with `#ifndef NATIVE_TEST` to exclude from native builds

## 📋 State Machine Test Coverage

The state machine tests cover:

### Phase-by-Phase Testing
- **PHASE_IDLE**: Idle state behavior
- **PHASE_OPENING_VALVE**: Valve opening and transition to stabilization
- **PHASE_WAITING_STABILIZATION**: 500ms delay and transition timing
- **PHASE_CHECKING_INITIAL_RAIN**:
  - Sensor dry → starts watering
  - Sensor wet → skips watering (tray already full)
  - Respects rain check interval (100ms)
- **PHASE_WATERING**:
  - Normal completion when sensor becomes wet
  - Continuous monitoring while sensor dry
  - Normal timeout (25s) handling
  - Emergency timeout (30s) handling
  - Manual stop functionality
  - Respects sensor check interval
- **PHASE_CLOSING_VALVE**: Cleanup and return to idle
- **PHASE_ERROR**: Error recovery

### Full Cycle Testing
- Complete successful watering cycle (all phases)
- Tray already full cycle (skips watering)

### Safety Features Tested
- MAX_WATERING_TIME timeout (25 seconds)
- ABSOLUTE_SAFETY_TIMEOUT emergency cutoff (30 seconds)
- Manual stop during watering
- Sensor check interval enforcement

Both learning algorithm and state machine logic can now be tested rapidly on your local machine without any ESP32 hardware!

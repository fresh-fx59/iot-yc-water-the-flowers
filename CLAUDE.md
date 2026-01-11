# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 smart watering system controlling 6 valves, 6 rain sensors, and 1 water pump. The system waters plants based on rain sensor feedback, automatically waters when trays are empty using time-based learning, publishes state to Yandex IoT Core MQTT, sends Telegram notifications for watering sessions with queue-based debug system, and provides a web interface for control.

**Key Technologies**:
- Platform: ESP32-S3-N8R2 / ESP32-S3-DevKitC-1 (Espressif32, Arduino framework)
- Filesystem: LittleFS (1MB partition for web UI and learning data persistence)
- Libraries: PubSubClient 2.8 (MQTT), ArduinoJson 6.21.0 (persistence), WiFiClientSecure (TLS), HTTPClient (Telegram), WebServer, mDNS, Adafruit NeoPixel 1.15.2
- Time Source: DS3231 RTC (I2C at GPIO 14/3) with battery backup and temperature sensor
- Current Version: 1.13.0 (defined in config.h:10)
- Testing: Native testing framework with 20 unit tests (no hardware required)

## Build & Deploy Commands

The project has **two separate build environments**:
- `esp32-s3-devkitc-1` - **Production firmware** (src/main.cpp)
- `esp32-s3-devkitc-1-test` - **Hardware testing firmware** (src/test-main.cpp)

### Production Build/Upload
```bash
# Build and upload production firmware
platformio run -t upload -e esp32-s3-devkitc-1

# Monitor serial output
platformio device monitor -b 115200 --raw
```

### Hardware Test Build/Upload
```bash
# Build and upload test firmware (filesystem already contains test HTML)
platformio run -t upload -e esp32-s3-devkitc-1-test

# Monitor test output
platformio device monitor -b 115200 --raw
```

**Test Firmware Features:**
- **Web Dashboard** at `http://<device-ip>/dashboard` with real-time output & controls
  - Test all hardware components (pump, valves, rain sensors)
  - Individual sensor testing (R1-R6 for single read, M1-M6 for continuous monitor)
  - Test DS3231 RTC (I2C at GPIO 14/3) with current time sync
  - Test water level sensor (GPIO 19)
  - WebSocket-based real-time console output
  - All serial commands available as clickable buttons
- Interactive serial menu (press 'H' for help)
- **WiFi + OTA + WebSocket support** for remote firmware switching
- No MQTT/Telegram/production watering logic

**Important:** Both production and test firmware share the same filesystem. Upload filesystem once, then switch modes without re-uploading to preserve learning data.

### Filesystem Operations
```bash
# Build filesystem image from /data folder
platformio run -t buildfs -e esp32-s3-devkitc-1

# Upload filesystem (contains web UI)
platformio run -t uploadfs -e esp32-s3-devkitc-1
```

### Full Clean Deploy
```bash
# Erase flash, rebuild everything, upload, and monitor
platformio run -t erase -e esp32-s3-devkitc-1 && \
platformio run -t buildfs -e esp32-s3-devkitc-1 && \
platformio run -t uploadfs -e esp32-s3-devkitc-1 && \
platformio run -t upload -e esp32-s3-devkitc-1 && \
platformio device monitor -b 115200 --raw
```

### Quick Redeploy (code changes only)
```bash
platformio run -t clean -e esp32-s3-devkitc-1 && \
platformio run -t upload -e esp32-s3-devkitc-1 && \
platformio device monitor -b 115200 --raw
```

## Code Organization (v1.13.0 - Extracted State Machine Architecture)

The codebase follows standard C++ organization with inline implementations in headers. Version 1.13.0 introduces a major architectural improvement by extracting hardware-independent logic for better testability.

### File Structure

```
include/
  ├── config.h                      # All constants and pin definitions
  ├── DS3231RTC.h                   # DS3231 Real-Time Clock helper library (v1.10.0+)
  ├── StateMachineLogic.h           # Pure state machine logic (hardware-independent, v1.13.0)
  ├── LearningAlgorithm.h           # Time-based learning helpers (pure functions, v1.13.0)
  ├── ValveController.h             # Valve state struct, enums, helper functions
  ├── WateringSystem.h              # Main watering logic + time-based learning + halt mode
  ├── WateringSystemStateMachine.h  # State machine + MQTT state publishing + safety timeouts
  ├── NetworkManager.h              # WiFi + MQTT management + halt/resume commands
  ├── TelegramNotifier.h            # Telegram Bot API + command polling (v1.12.0)
  ├── DebugHelper.h                 # Queue-based debug system with retry & grouping (v1.6.1)
  ├── api_handlers.h                # Web API endpoints
  ├── ota.h                         # OTA updates and web server
  ├── TestConfig.h                  # Configuration for native testing (v1.13.0)
  └── secret.h                      # WiFi/MQTT/Telegram credentials (not in git)

src/
  ├── main.cpp                      # Production firmware + boot countdown (v1.12.0)
  └── test-main.cpp                 # Hardware test firmware with OTA + DS3231 tests

test/
  ├── test_native_all.cpp           # Combined test suite (20 tests, v1.13.0)
  ├── test_state_machine.cpp        # State machine tests (17 tests, v1.13.0)
  ├── test_learning_algorithm.cpp   # Learning algorithm tests (3 tests, v1.13.0)
  └── test_overwatering_scenarios.cpp  # Safety scenario tests (v1.13.0)

data/
  └── web/                          # Shared filesystem (served via LittleFS)
      ├── prod/                     # Production web UI
      │   ├── index.html
      │   ├── css/style.css
      │   └── js/app.js
      └── test/                     # Test mode web UI
          ├── index.html            # Test mode home page
          └── firmware.html         # OTA upload page

platformio.ini                      # Three build environments:
                                    # - esp32-s3-devkitc-1 (production)
                                    # - esp32-s3-devkitc-1-test (testing)
                                    # - native (desktop unit tests, v1.13.0)

NATIVE_TESTING_PLAN.md              # Testing strategy and framework (v1.13.0)
OVERWATERING_RISK_ANALYSIS.md       # Safety analysis and mitigation (v1.13.0)
OVERWATERING_TEST_SUMMARY.md        # Test results and validation (v1.13.0)
```

### Key Design Principles

1. **Separation of Concerns**: Each header file has a single, well-defined responsibility
2. **Hardware Independence** (v1.13.0): Pure logic extracted for testability (StateMachineLogic.h, LearningAlgorithm.h)
3. **Helper Functions**: Complex logic extracted into namespaces
4. **Clear Documentation**: Each section is clearly marked with comments
5. **Standard Patterns**: Uses inline functions in headers (common for embedded C++)
6. **Persistence**: Learning data saved to LittleFS, survives reboots
7. **Comprehensive Testing** (v1.13.0): 20 native unit tests run on desktop without hardware

### Working with the Refactored Code

**Adding new features**:
- Hardware config → `config.h`
- State machine logic → `StateMachineLogic.h` (hardware-independent, v1.13.0)
- Learning algorithm → `LearningAlgorithm.h` (pure functions, v1.13.0)
- Valve logic → `WateringSystem.h` or `WateringSystemStateMachine.h`
- Network features → `NetworkManager.h`
- Web API → `api_handlers.h`
- Unit tests → `test/` directory (v1.13.0)

**Running Tests** (v1.13.0):
```bash
# Run all native tests on your computer (no ESP32 required)
pio test -e native

# Expected output: 20 test cases: 20 succeeded
```

**The main.cpp file** is clean and minimal (~110 lines):
- Global object declarations
- LittleFS initialization
- setup() function
- loop() function
- API handler registration

## Architecture

### Extracted State Machine (v1.13.0)

**StateMachineLogic.h** - Hardware-independent state machine:

The state machine logic has been extracted into a pure, testable module that returns actions instead of executing hardware operations directly.

**Key Components**:
- `ProcessResult` struct: Contains new phase, action to execute, and updated timestamps
- `Action` enum: ACTION_OPEN_VALVE, ACTION_CLOSE_VALVE, ACTION_TURN_PUMP_ON, ACTION_TURN_PUMP_OFF, ACTION_READ_SENSOR, ACTION_EMERGENCY_STOP
- `processValveLogic()`: Pure function that processes state transitions

**Benefits**:
- ✅ **Testable**: Can test state machine logic without ESP32 hardware
- ✅ **Predictable**: Pure function with no side effects
- ✅ **Clear**: Separates logic from hardware operations
- ✅ **Reliable**: Comprehensive unit test coverage (17 tests)

**Example Usage**:
```cpp
// Call pure logic function
ProcessResult result = StateMachineLogic::processValveLogic(
    currentPhase, currentTime, valveOpenTime, wateringStartTime,
    lastRainCheck, isRaining, wateringRequested,
    VALVE_STABILIZATION_DELAY, RAIN_CHECK_INTERVAL,
    MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT
);

// Execute returned action
switch (result.action) {
    case ACTION_OPEN_VALVE: digitalWrite(valvePin, HIGH); break;
    case ACTION_CLOSE_VALVE: digitalWrite(valvePin, LOW); break;
    // ... etc
}

// Update state
valve.phase = result.newPhase;
valve.valveOpenTime = result.newValveOpenTime;
```

### Extracted Learning Algorithm (v1.13.0)

**LearningAlgorithm.h** - Pure learning algorithm helpers:

Time-based learning functions extracted into reusable, testable helpers:

- `calculateWaterLevelBefore(fillDuration, baselineFillDuration)`: Calculate water level percentage before watering based on how long it took to fill
- `calculateEmptyDuration(fillDuration, baselineFillDuration, timeSinceLastWatering)`: Estimate how long water lasts based on consumption rate
- `formatDuration(milliseconds)`: Convert milliseconds to human-readable format (e.g., "2d 4h", "3h 15m")

**Benefits**:
- ✅ **Reusable**: Can be used across different components
- ✅ **Testable**: Unit tests verify calculations (3 tests)
- ✅ **Documented**: Clear algorithm documentation
- ✅ **Reliable**: No hardware dependencies

### Testing Infrastructure (v1.13.0)

**Native Testing Framework**:

The project includes comprehensive unit tests that run on your desktop without requiring ESP32 hardware:

**Test Files**:
- `test/test_native_all.cpp` - Combined test suite (20 tests)
- `test/test_state_machine.cpp` - State machine specific tests (17 tests)
- `test/test_learning_algorithm.cpp` - Learning algorithm tests (3 tests)
- `test/test_overwatering_scenarios.cpp` - Safety scenario tests

**Test Coverage**:
- ✅ All state machine phase transitions (idle → opening → stabilization → checking → watering → closing)
- ✅ Timeout handling (normal 25s timeout, emergency 30s cutoff)
- ✅ Full watering cycles from start to completion
- ✅ Learning algorithm calculations (water level, empty duration, formatting)
- ✅ Overwatering scenarios and safety measures

**Run Tests**:
```bash
# Run all native tests
pio test -e native

# Expected output:
# ================= 20 test cases: 20 succeeded in 00:00:01.876 =================
```

**Documentation**:
- `NATIVE_TESTING_PLAN.md` - Testing strategy and framework details
- `OVERWATERING_RISK_ANALYSIS.md` - Safety analysis and mitigation strategies
- `OVERWATERING_TEST_SUMMARY.md` - Test results and validation

### Safety Features (v1.11.0 - v1.12.5)

The system includes **multi-layer safety protection** to prevent overwatering:

**Layer 1: Master Overflow Sensor** (v1.12.1)
- **Hardware**: Rain sensor connected via 2N2222 transistor circuit to GPIO 42
- **Detection**: LOW = overflow detected (water present), HIGH = normal (dry)
- **Polling**: 100ms (fastest response - runs FIRST in every loop)
- **Function**: `checkMasterOverflowSensor(unsigned long currentTime)`
- **Emergency Response**:
  - Calls `emergencyStopAll("OVERFLOW DETECTED")`
  - Direct GPIO writes to close all valves (bypasses state machine)
  - Force pump OFF
  - Turn off LED
  - Stop sequential mode
  - Set `overflowDetected` flag (blocks all future watering)
  - Send Telegram emergency alert
- **Recovery**: Manual intervention required, send `reset_overflow` or `/reset_overflow` command
- **Location**: WateringSystem.h:530-595

**Layer 2: Safety Timeouts** (config.h:66-67, v1.12.5)
- `MAX_WATERING_TIME = 25000` (25s) - Normal watering timeout
- `ABSOLUTE_SAFETY_TIMEOUT = 30000` (30s) - Emergency hard limit

**Layer 3: Two-Tier State Machine Timeouts** (WateringSystemStateMachine.h:77-106)
- Normal timeout (25s): Standard valve closure with learning data processing
- Emergency cutoff (30s): Forces hardware shutdown via direct GPIO control
- Both run in PHASE_WATERING state

**Layer 4: Global Safety Watchdog** (WateringSystem.h:479-520)
- Function: `globalSafetyWatchdog(unsigned long currentTime)`
- Runs independently every loop iteration (called after master overflow check)
- Bypasses state machine if ABSOLUTE_SAFETY_TIMEOUT exceeded
- Forces valves and pump OFF via direct GPIO writes
- Cannot be blocked by state machine issues

**Layer 5: Enhanced Sensor Logging** (WateringSystem.h:832-838)
- Logs raw GPIO values every 5 seconds during watering
- Format: `Sensor N GPIO X: raw=Y (WET/DRY)`
- Helps diagnose hardware failures post-incident

**Layer 6: Sensor Diagnostic Tools** (WateringSystem.h:1530-1627)
- `testSensor(valveIndex)`: Test individual sensor with detailed report
- `testAllSensors()`: Test all 6 sensors and create summary table
- Checks: power pin config, sensor pin config, power-off reading, power-on reading
- Detects: pullup resistor failures, shorts, hardware faults
- Accessible via MQTT commands: `test_sensors`, `test_sensor_N`

### Emergency Halt Mode (v1.12.0)

**Boot Countdown System** (main.cpp:92-180)

Every boot provides a 10-second safety window for emergency firmware updates:

1. Device boots and connects to WiFi
2. Sends Telegram notification with countdown
3. Calls `bootCountdown()` function
4. Polls Telegram Bot API every 500ms for `/halt` command
5. If `/halt` received → enters halt mode
6. If 10 seconds expire → normal operation

**Halt Mode Implementation** (WateringSystem.h:133-206)

State management:
- `bool haltMode` - Blocks all watering when true
- `setHaltMode(bool enabled)` - Activate/deactivate
- `isHaltMode()` - Check status

When halt mode activated:
- Stops any ongoing sequential watering
- Stops all active valves immediately
- Turns off LED indicator
- Blocks future watering attempts:
  - `startWatering()` - Returns immediately
  - `startSequentialWatering()` - Returns immediately
  - `startSequentialWateringCustom()` - Returns immediately
  - `checkAutoWatering()` - Returns immediately without checking

Commands (Telegram + MQTT):
- `/halt` or `halt` - Enter halt mode
- `/resume` or `resume` - Exit halt mode

**Use Cases**:
- Emergency firmware fix after discovering critical bug
- Quick OTA access without waiting for watering cycle
- Block operations while remote testing/debugging

**Telegram Command Polling** (TelegramNotifier.h:182-232)

Function: `checkForCommands(int &lastUpdateId)`
- Uses Telegram Bot API `getUpdates` method
- Offset parameter prevents duplicate processing
- 1-second timeout, 2-second HTTP timeout
- Parses JSON manually (simple extraction)
- Returns command string or empty

### Watering Algorithm Flow

The system implements a 5-phase watering cycle per valve to ensure accurate rain sensor readings:

1. **PHASE_IDLE**: Valve is inactive
2. **PHASE_OPENING_VALVE**: Open valve first (sensor needs water flow to function)
3. **PHASE_WAITING_STABILIZATION**: Wait 500ms for valve to open and water to start flowing
4. **PHASE_CHECKING_INITIAL_RAIN**: Check rain sensor (now accurate with flowing water)
   - If already wet: Close valve, abort (pump never turns on)
   - If dry: Proceed to watering phase
5. **PHASE_WATERING**: Turn on pump, monitor sensor every 100ms until wet or 15s timeout
6. **PHASE_CLOSING_VALVE**: Close valve, turn off pump if no other valves active, process learning data
7. **PHASE_ERROR**: Error state (not currently used)

**Critical Design Decision**: Valve opens BEFORE sensor check because rain sensors require water flow to produce accurate readings. The pump only turns on during the actual watering phase if the sensor is initially dry.

**Sequential Watering**: When `startSequentialWatering()` is called, valves are watered in reverse order (5→0) one at a time. The next valve starts only after the previous completes its cycle.

**Automatic Watering**: System automatically checks each valve every loop iteration. If time-based learning indicates a tray is empty AND auto-watering is enabled, it starts watering automatically.

### Core Classes & System Organization

**Classes are properly separated into header files** (v1.5.0):

- **WateringSystem** (WateringSystem.h): Main orchestrator managing 6 ValveController instances
  - `processWateringLoop()`: State machine executor + auto-watering checker, called every loop
  - `checkAutoWatering()`: Checks if any tray is empty and needs automatic watering
  - `startWatering(valveIndex)`: Initiates watering cycle, checks time-based skip logic
  - `startSequentialWatering()`: Waters all valves 5→0 in sequence
  - `startSequentialWateringCustom(indices[], count)`: Custom valve sequence
  - `publishCurrentState()`: MQTT state updates every 2 seconds (cached in `lastStateJson`)
  - `getLastState()`: Returns cached state JSON for web API
  - `clearTimeoutFlag(valveIndex)`: Clears timeout flag for recovery
  - **Time-based learning methods**:
    - `resetCalibration(valveIndex)`: Reset calibration for valve
    - `resetAllCalibrations()`: Reset all valves
    - `printLearningStatus()`: Print detailed time-based learning status
    - `setAutoWatering(valveIndex, enabled)`: Enable/disable auto-watering per valve
    - `setAllAutoWatering(enabled)`: Enable/disable auto-watering for all
  - **Persistence methods**:
    - `saveLearningData()`: Save learning data to `/learning_data.json`
    - `loadLearningData()`: Load learning data on startup

- **ValveController** (ValveController.h): Per-valve state tracking struct
  - Tracks: phase, state, rainDetected, valveOpenTime, wateringStartTime, timeoutOccurred
  - **Time-based learning fields**:
    - `lastWateringCompleteTime`: When tray became full (millis)
    - `emptyToFullDuration`: How long water lasts (consumption time, ms)
    - `baselineFillDuration`: Time to fill from empty (adaptive, updates when longer fill observed)
    - `lastFillDuration`: Most recent fill duration
    - `lastWaterLevelPercent`: Water level before last watering (0-100%)
    - `isCalibrated`: Has baseline been established
    - `totalWateringCycles`: Total successful cycles
    - `autoWateringEnabled`: Auto-water when empty (default: true)
  - Each valve operates independently with its own state machine and learning
  - **Helper functions**:
    - `phaseToString()`: Convert phase enum to string
    - `calculateCurrentWaterLevel()`: Real-time water level based on time elapsed
    - `getTrayState()`: Returns "empty", "full", or "between"
    - `shouldWaterNow()`: Check if tray needs watering based on time

- **NetworkManager** (NetworkManager.h): Combined WiFi + MQTT management
  - WiFi: `connectWiFi()`, `isWiFiConnected()` with 30-attempt retry logic
  - MQTT: `connectMQTT()`, `loopMQTT()`, `isMQTTConnected()`
  - Subscribes to: `$devices/{DEVICE_ID}/commands`
  - Publishes to: `$devices/{DEVICE_ID}/state` and `events`
  - Command parser in `processCommand()` handles all MQTT commands
  - Command helper: `handleSequenceCommand()`

- **LearningAlgorithm** namespace (LearningAlgorithm.h, v1.13.0): Time-based learning helpers
  - `calculateWaterLevelBefore()`: Calculate water level from fill duration ratio
  - `calculateEmptyDuration()`: Estimate time until tray is empty
  - `formatDuration()`: Format milliseconds to human-readable (e.g., "2d 4h")
  - **Pure functions**: No hardware dependencies, fully unit tested

- **StateMachineLogic** namespace (StateMachineLogic.h, v1.13.0): Hardware-independent state machine
  - `processValveLogic()`: Pure state machine function that returns actions to execute
  - `ProcessResult` struct: Contains new phase, action, and updated state
  - **Testable**: 17 unit tests cover all phase transitions and timeout scenarios

**Supporting files**:
- **config.h**: All constants, pin definitions, and configuration
- **ota.h**: OTA updates, web server setup, LittleFS file serving
- **api_handlers.h**: Web API endpoint implementations
- **secret.h**: WiFi/MQTT credentials (never commit)
- **TestConfig.h** (v1.13.0): Configuration for native testing environment

### Hardware Configuration (ESP32-S3-N8R2)

**Pin Assignments**:
- Pump: GPIO 4
- Valves: GPIO 5, 6, 7, 15, 16, 17
- Rain Sensors: GPIO 8, 9, 10, 11, 12, 13 (INPUT_PULLUP, LOW=wet, HIGH=dry)
- Sensor Power (Optocoupler): GPIO 18 (powers sensors only when needed)
- Master Overflow Sensor: GPIO 42 (INPUT_PULLUP, LOW=overflow, HIGH=normal) - v1.12.1
- NeoPixel RGB LED: GPIO 48 (built-in on ESP32-S3-N8R2)
- DS3231 RTC I2C: SDA=GPIO 14, SCL=GPIO 3, Address=0x68
- DS3231 Battery Monitor: ADC=GPIO 1, Control=GPIO 2

**Rain Sensor Reading Logic**:
- **CRITICAL**: Sensors require TWO power signals to function:
  1. **Valve pin HIGH** (GPIO 5/6/7/15/16/17) - Powers the specific sensor circuit
  2. **GPIO 18 HIGH** - Enables the common sensor power rail via optocoupler
- Reading sequence: Set valve pin HIGH → Set GPIO 18 HIGH → delay 100ms → read sensor → power off
- LOW (0) = WET (sensor pulls to ground when water detected)
- HIGH (1) = DRY (internal pull-up resistor keeps line high when dry)
- Sensors configured with INPUT_PULLUP mode in setup
- Production firmware reads sensors during watering (valve already open), test firmware powers both explicitly

**Master Overflow Sensor (v1.12.1)**:
- Rain sensor detects water overflow from trays
- Connected via 2N2222 transistor circuit to GPIO 42
- Circuit: Rain sensor → resistors → transistor base → collector pulls GPIO 42 LOW when wet
- LOW (0) = OVERFLOW (water detected - emergency condition)
- HIGH (1) = NORMAL (dry - no overflow)
- Configured with INPUT_PULLUP mode
- Polled every 100ms (highest priority check in main loop)

**DS3231 RTC (v1.10.0+)**:
- Provides all time functions (replaces NTP dependency)
- Battery-backed real-time clock (CR2032)
- Temperature sensor (-40°C to +85°C, ±3°C accuracy)
- Battery voltage monitoring with calibration support
- I2C communication at 100kHz
- Helper library: `DS3231RTC.h`

**NeoPixel LED (v1.10.0+)**:
- Single RGB LED on GPIO 48
- Adafruit NeoPixel library
- Status indication during watering
- OFF when in halt mode

### Adaptive Interval Learning Algorithm (v1.8.0+)

**Purpose**: Uses binary search/gradient ascent to find the optimal watering interval where trays are consistently empty (maximum fill time). Each tray learns its own optimal interval based on consumption rate.

**Algorithm Phases**:

**Phase 1: Exponential Search (Coarse Adjustment)**
- **Tray already full** → Double interval (1.0x → 2.0x → 4.0x)
- **Fill < 95% baseline** → Increase by 1.0x (tray not fully empty yet)
- **Fill > baseline** → Update baseline + increase by 1.0x (tray was emptier!)

**Phase 2: Binary Search Refinement (Fine-Tuning)**
- **Fill ≈ baseline and stable** → Decrease by 0.5x (interval too long)
- **Fill decreasing from previous** → Increase by 0.25x (interval was too long)
- **Fill increasing from previous** → Increase by 0.25x (can try longer)

**Constants** (tunable in `processLearningData()`):
```cpp
const float BASELINE_TOLERANCE = 0.95;            // 95% - threshold for "tray not fully empty"
const long FILL_STABLE_TOLERANCE_MS = 500;        // ±0.5s - threshold for "same fill time"
const float INTERVAL_DOUBLE = 2.0;                // Double when tray already full
const float INTERVAL_INCREMENT_LARGE = 1.0;       // Large adjustment for coarse search
const float INTERVAL_DECREMENT_BINARY = 0.5;      // Binary search refinement
const float INTERVAL_INCREMENT_FINE = 0.25;       // Fine-tuning adjustment
```

**Example Learning Sequence**:
```
1. water 15s → baseline=15s, interval=1.0x (24h)
2. tray full → interval=2.0x (48h) [doubled - consuming slower]
3. water 10s < 15s → interval=3.0x (72h) [+1.0, not empty yet]
4. water 16s > 15s → baseline=16s, interval=4.0x (96h) [+1.0, emptier!]
5. water 16s = 16s → interval=3.5x (84h) [-0.5, stable - binary search]
6. water 15s < 16s → interval=3.75x (90h) [+0.25, worse - fine tune]
7. water 16s = 16s → interval=3.25x (78h) [-0.5, stable]
8. ✅ OPTIMAL FOUND: 3.25x (78 hours) with 16s baseline
```

**Key Features**:
- Self-adjusting per tray (different consumption rates)
- Converges to optimal interval (not too soon, not too late)
- Adaptive baseline updates when tray emptier than ever seen
- Uses actual fill duration trends, not fixed calculations
- Persists to flash storage (`/learning_data.json`)
- Survives reboots and power cycles
- Handles `millis()` overflow
- **Migration system**: Uses two-file approach (current + old) for idempotent resets

**Smart Boot Watering (v1.8.1+, fixed in v1.8.4)**:

System intelligently decides whether to water on power-on:

```
Boot Decision Tree:
├─ 1️⃣ First boot (no calibration data)?
│   └─ Water all valves (initial calibration needed)
├─ 2️⃣ Any valve overdue for watering?
│   └─ Check: currentTime >= (lastWatering + learnedInterval)
│   └─ Water overdue valves (catch up after long outage)
└─ 3️⃣ All valves on schedule?
    └─ Skip boot watering (prevents over-watering during frequent power cycles)
```

**Benefits**:
- Prevents over-watering during frequent power cycles
- Catches missed waterings after long outages
- Uses learned intervals (not arbitrary thresholds)
- Self-recovering from power issues

**v1.8.4 Fix**: Fixed auto-watering fallback bug that caused immediate watering on reboot when timestamps were invalid. System now safely defaults to NOT watering when timestamp data is uncertain, letting boot logic handle truly overdue valves instead.

**v1.8.5 Fix**: Fixed critical boot watering bug where valves missed waterings after long outages. When outage duration exceeded `millis()` range, timestamps couldn't be represented in the new epoch (set to 0), causing `hasOverdueValves()` to incorrectly skip watering. Now detects calibrated valves with learning data but zero timestamps as definitely overdue.

### MQTT Commands

Format: Plain text commands sent to `$devices/{DEVICE_ID}/commands`

**Watering Commands**:
- `start_all`: Start sequential watering of all valves (5→0 order)

**Safety & Control Commands (v1.11.0+)**:
- `halt` or `/halt`: Enter halt mode (blocks all watering operations)
- `resume` or `/resume`: Exit halt mode (resume normal operations)

**Sensor Diagnostic Commands (v1.11.0+)**:
- `test_sensors`: Test all 6 sensors and generate diagnostic report
- `test_sensor_N` (N=0-5): Test individual sensor (e.g., `test_sensor_0`, `test_sensor_1`)

**Master Overflow Sensor Commands (v1.12.1)**:
- `reset_overflow` or `/reset_overflow`: Reset overflow flag after fixing overflow issue (manual intervention required)

**Notes**:
- System automatically publishes state every 2 seconds
- Auto-watering and learning features work automatically in background
- Individual valve control via web interface only
- Telegram Bot also accepts `/halt`, `/resume`, and `/reset_overflow` commands

### MQTT State Publishing

Published to `$devices/{DEVICE_ID}/state` every 2 seconds. Example:

```json
{
  "pump": "off",
  "sequential_mode": false,
  "valves": [
    {
      "id": 0,
      "state": "closed",
      "phase": "idle",
      "rain": false,
      "timeout": false,
      "learning": {
        "calibrated": true,
        "auto_watering": true,
        "baseline_fill_ms": 5200,
        "last_fill_ms": 4200,
        "empty_duration_ms": 86400000,
        "total_cycles": 5,
        "water_level_pct": 45,
        "tray_state": "between",
        "time_since_watering_ms": 43200000,
        "time_until_empty_ms": 43200000,
        "last_water_level_pct": 16
      }
    }
  ]
}
```

### Telegram Bot Notifications

The system sends automatic Telegram notifications during sequential watering sessions.

**Configuration** (in `include/secret.h`):
```cpp
#define TELEGRAM_BOT_TOKEN "your_bot_token"
#define TELEGRAM_CHAT_ID "your_chat_id"
```

**Start Notification** - Sent when sequential watering begins:
```
🚿 Watering Started
⏰ Session 12345s
🔧 Trigger: MQTT Command
🌱 Trays: 6, 5, 4, 3, 2, 1
```

**Completion Notification** - Sent when all valves complete:
```
✅ Watering Complete

tray | duration(sec) | status
-----|---------------|-------
6    | 3.2          | ✓ OK
5    | 4.5          | ✓ OK
4    | 0.5          | ⚠️ ALREADY_WET
3    | 14.5         | ⚠️ TIMEOUT
2    | 2.8          | ⚠️ MANUAL_STOP
1    | 3.8          | ✓ OK
```

**Status Meanings** (v1.6.1 updated):
- `✓ OK`: Watering completed successfully (sensor became wet after pump started)
- `✓ FULL`: Tray was already full (sensor already wet before pump started)
- `⚠️ TIMEOUT`: Exceeded maximum watering time (25s, v1.12.5)
- `⚠️ STOPPED`: Watering was stopped manually or other interruption

**Duration Calculation**: Time from valve open to final state (includes full watering cycle)

**How It Works**:
1. `TelegramNotifier` class in `TelegramNotifier.h` handles HTTP requests to Telegram Bot API
2. `WateringSystem` tracks session data for each valve during sequential watering
3. Start message sent immediately when `startSequentialWatering()` is called
4. Each valve's duration and status recorded during watering cycle
5. Completion table sent when all valves finish

**Notes**:
- Only triggered during sequential watering (not individual valve operations)
- Uses direct HTTPS calls to `api.telegram.org`
- Requires WiFi connection (silently fails if offline)
- Session tracking data stored in `WateringSessionData` struct

### Telegram Debug System (v1.6.1)

The `DebugHelper` class provides a sophisticated debug message delivery system with automatic retry and message grouping.

**Configuration** (in `include/config.h`):
```cpp
#define IS_DEBUG_TO_SERIAL_ENABLED false    // Enable serial console debug
#define IS_DEBUG_TO_TELEGRAM_ENABLED true   // Enable Telegram debug

const int TELEGRAM_QUEUE_SIZE = 20;                     // Circular buffer size
const int TELEGRAM_MAX_RETRY_ATTEMPTS = 5;              // Retry attempts per message
const unsigned long TELEGRAM_RETRY_DELAY_MS = 2000;     // 2s delay between retries
const unsigned long MESSAGE_GROUP_INTERVAL_MS = 2000;   // 2s grouping window
const unsigned long MESSAGE_GROUP_MAX_AGE_MS = 180000;  // 3min max age (safety)
```

**Key Features**:
- **Circular Buffer Queue**: Holds up to 20 messages with retry tracking
- **Automatic Retry**: Up to 5 attempts per message with 2-second delays
- **Non-Blocking Processing**: Processes one message per loop iteration
- **Message Grouping**: Messages arriving within 2s are batched together
- **Safety Limit**: Groups flush after 3 minutes max to prevent infinite buffering
- **Explicit Flush**: Buffer flushed before "watering complete" notification
- **Timestamped**: Each message shows `[DD-MM-YYYY HH:MM:SS.mmm]` format

**Usage in Code**:
```cpp
// Regular debug message (buffered and grouped)
DebugHelper::debug("Valve opened");

// Important message (marked with 🔴 prefix)
DebugHelper::debugImportant("⚠️ TIMEOUT occurred");

// Explicit flush (called before completion notification)
DebugHelper::flushBuffer();

// Process queue (called in main loop)
DebugHelper::loop();
```

**Message Grouping Behavior**:
1. Messages arriving within 2 seconds are grouped into one Telegram message
2. If group age exceeds 3 minutes → automatic flush (safety)
3. If 2 seconds silence → flush and send
4. Explicit flush called before completion notification ensures all debug messages appear

**Example Output**:
```
🐛 Debug
[17-11-2025 14:23:10.125] ✓ Valve 0 opened - waiting stabilization
[17-11-2025 14:23:10.625] Step 2: Checking rain sensor (water is flowing now)...
[17-11-2025 14:23:10.725] ✓ Sensor 0 is DRY - starting pump (timeout: 20s)
[17-11-2025 14:23:13.125] ✓ Valve 0 COMPLETE - Total: 3s (pump: 2s)
```

**Implementation Details**:
- Queue uses circular buffer pattern (head/tail pointers)
- Each message tracks: content, timestamp, retry count, last retry time
- Messages dropped after 5 failed attempts to prevent queue stall
- WiFi disconnect doesn't block watering (messages queue and send when reconnected)
- Located in `include/DebugHelper.h` (~326 lines)

### Web Interface

**Files** (in `/data/web/`):
- `index.html`: Main control panel
- `css/style.css`: Styling
- `js/app.js`: Frontend logic

**API Endpoints**:
- `GET /api/water?valve=N` (N=1-6): Start watering
- `GET /api/stop?valve=N` or `?valve=all`: Stop watering
- `GET /api/status`: Get current system state JSON (with time-based learning data)
- `GET /firmware`: OTA update page (auth: admin/OTA_PASSWORD)

**Important**: API uses 1-indexed valves (1-6), internal code uses 0-indexed (0-5)

### Testing & Development

**Native Testing** (v1.13.0):

The project includes comprehensive unit tests that run on your desktop:

```bash
# Run all native tests (no ESP32 required)
pio test -e native
```

**Test Coverage**:
- State machine phase transitions (17 tests)
- Learning algorithm calculations (3 tests)
- Overwatering scenarios and safety measures
- Timeout handling (normal & emergency)

**Test Files**:
- `test/test_native_all.cpp` - Combined test suite
- `test/test_state_machine.cpp` - State machine tests
- `test/test_learning_algorithm.cpp` - Learning algorithm tests
- `test/test_overwatering_scenarios.cpp` - Safety tests

**Documentation**:
- `NATIVE_TESTING_PLAN.md` - Testing strategy
- `OVERWATERING_RISK_ANALYSIS.md` - Safety analysis
- `OVERWATERING_TEST_SUMMARY.md` - Test results

### Configuration Files

**include/secret.h** (never commit, must exist):
```cpp
#define SSID "wifi_name"
#define SSID_PASSWORD "wifi_password"
#define YC_DEVICE_ID "device_id"
#define MQTT_PASSWORD "mqtt_password"
#define OTA_USER "admin"
#define OTA_PASSWORD "ota_password"
#define TELEGRAM_BOT_TOKEN "your_bot_token"
#define TELEGRAM_CHAT_ID "your_chat_id"
```

### Safety Features Summary

- **Timeout Protection**: Max watering time 25 seconds per valve (MAX_WATERING_TIME, v1.12.5)
- **Emergency Cutoff**: Absolute 30-second hard limit (ABSOLUTE_SAFETY_TIMEOUT)
- **Timeout Flags**: Persist until cleared via `clear_timeout_N` command
- **Pump Coordination**: Pump only runs when valves are in PHASE_WATERING and sensor is dry
- **MQTT Failure Isolation**: Network issues never block watering algorithm
- **Auto-Watering Safety**: Only triggers when calibrated AND enabled AND tray is calculated empty
- **Data Persistence**: Learning data survives reboots, preventing re-calibration
- **Comprehensive Testing** (v1.13.0): 20 unit tests validate safety logic

### Testing & Debugging

**Native Tests** (v1.13.0):
Run on your desktop (no hardware required):
```bash
pio test -e native
```

**Hardware test firmware**: `src/test-main.cpp`
Build and upload: `platformio run -t upload -e esp32-s3-devkitc-1-test`

**Test menu commands** (via serial at 115200 baud):
- **LED**: `L` - Toggle LED
- **Pump**: `P` - Toggle pump
- **Valves**: `1-6` - Toggle individual valves, `A` - All on, `Z` - All off
- **Rain Sensors (All)**: `R` - Read all once, `M` - Monitor all continuous, `S` - Stop monitoring
- **Rain Sensors (Individual)**: `R1-R6` - Read specific sensor once, `M1-M6` - Monitor specific sensor continuous
- **Water Level**: `W` - Read once, `N` - Monitor continuous
- **DS3231 RTC**: `T` - Read time/temperature, `I` - Scan I2C bus
- **System**: `F` - Full sequence test, `X` - Emergency stop, `H` - Show menu

**Individual Sensor Commands Details**:
- `R1` - Read sensor 1 (Valve GPIO 5, Sensor GPIO 8)
- `R2` - Read sensor 2 (Valve GPIO 6, Sensor GPIO 9)
- `R3` - Read sensor 3 (Valve GPIO 7, Sensor GPIO 10)
- `R4` - Read sensor 4 (Valve GPIO 15, Sensor GPIO 11)
- `R5` - Read sensor 5 (Valve GPIO 16, Sensor GPIO 12)
- `R6` - Read sensor 6 (Valve GPIO 17, Sensor GPIO 13)
- `M1-M6` - Same as above, but continuous monitoring (updates every 500ms)
- `S` - Stops all monitoring and powers off all sensors

**Important**: Individual sensor commands power only the specific valve pin + GPIO 18, minimizing power consumption during testing.

**Switching between Production/Test firmware:**
1. **To Test Mode**: `platformio run -t upload -e esp32-s3-devkitc-1-test`
2. **To Production**: `platformio run -t upload -e esp32-s3-devkitc-1`
3. **Via OTA** (when remote): Upload appropriate `.bin` file from `.pio/build/` folder

**Debug Output**: All system events print to serial console at 115200 baud. Key debug patterns:
- `═══` headers mark major state changes (valve cycle start, sequential mode)
- `🧠` prefix for learning algorithm updates
- `✨` prefix for baseline updates
- `⏰` prefix for auto-watering triggers
- `✓` prefix for successful operations
- `ERROR:` prefix for failures
- Phase transitions print with step numbers and explanations

**Common Debug Techniques**:
- Watch for "Timeout occurred!" messages during watering
- Check `lastStateJson` cache for web API state discrepancies
- Monitor MQTT connection status (system continues working if MQTT fails)
- Use `learning_status` command to see detailed learning state
- Check `/learning_data.json` file in LittleFS for persisted data
- Use `platformio device monitor --raw` if seeing garbage characters

## Making Code Changes

### Modifying Watering Logic
- **State machine logic** (v1.13.0): `StateMachineLogic.h` - `processValveLogic()` pure function
- **State machine execution**: `WateringSystemStateMachine.h` - `processValve()` method (calls StateMachineLogic)
- **Learning algorithm** (v1.13.0): `LearningAlgorithm.h` - Pure helper functions
- **Learning data processing**: `WateringSystem.h` - `processLearningData()` method
- **Time-based skip check**: `WateringSystem.h` - `startWatering()` method (checks time instead of cycles)
- **Auto-watering check**: `WateringSystem.h` - `checkAutoWatering()` method
- **Constants**: `config.h` - `LEARNING_*` constants
- **Testing**: Run native tests first (`pio test -e native`), then hardware test program before deploying

### Adding New MQTT Commands
1. Add command parsing in `NetworkManager.h` - `processCommand()` method (currently only supports `start_all`)
2. Call appropriate WateringSystem method
3. Test via MQTT: `mosquitto_pub -t "$devices/DEVICE_ID/commands" -m "start_all"`

**Note**: MQTT interface has been simplified to only support `start_all` command. Other control methods:
- Web API (still supports individual valve control)
- Auto-watering (automatic based on time-based learning)
- Serial debugging commands (via hardware test program)

### Adding New API Endpoints
1. Add handler function in `api_handlers.h` (inline implementation)
2. Register in `main.cpp` - `registerApiHandlers()` function
3. Remember: API uses 1-indexed valves (1-6), convert to 0-indexed (0-5) for internal calls
4. Handler has access to `g_wateringSystem_ptr` global

### Modifying Web Interface
1. Edit files in `/data` directory (NOT `/web`)
2. Rebuild filesystem: `platformio run -t buildfs -e esp32-s3-devkitc-1`
3. Upload filesystem: `platformio run -t uploadfs -e esp32-s3-devkitc-1`
4. Changes won't appear until both buildfs and uploadfs complete

### Changing Configuration
- **Pin assignments**: `config.h` - Update `#define` statements and arrays
- **Timing constants**: `config.h` - Update `*_INTERVAL` and `*_DELAY` constants
- **Learning parameters**: `config.h` - Update `LEARNING_*` constants (e.g., thresholds)
- **MQTT settings**: `config.h` - Update `MQTT_*` constants
- **Credentials**: `include/secret.h` (never commit this file)

### Modifying Learning Algorithm
- **Core algorithm logic** (v1.13.0): `LearningAlgorithm.h` - Pure functions, fully tested
- **Water level calculation**: `LearningAlgorithm::calculateWaterLevelBefore()` helper
- **Consumption calculation**: `LearningAlgorithm::calculateEmptyDuration()` helper
- **Duration formatting**: `LearningAlgorithm::formatDuration()` helper
- **Baseline update logic**: `WateringSystem.h` - `processLearningData()` method
- **Smoothing factor**: Currently 70% old, 30% new
- **Auto-watering decision**: `shouldWaterNow()` helper in `ValveController.h`
- **Testing**: Add tests to `test/test_learning_algorithm.cpp`

### Working with Persistence
- **Save location**: `/learning_data.json` on LittleFS (current active file)
- **Old file location**: `/learning_data_v5.json` (auto-deleted on boot via idempotent migration)
- **Save triggers**: After each successful watering, after calibration reset, manual `save_data` command
- **Load trigger**: On `wateringSystem.init()` during startup
- **Data format**: JSON with valve array containing all learning fields
- **Testing**: Use LittleFS browser or read file via serial commands
- **Resetting learning data**: Swap filenames in `WateringSystem.h` (LEARNING_DATA_FILE ↔ LEARNING_DATA_FILE_OLD), old file auto-deletes on next boot

## Program Flow & Initialization

**setup() sequence** (main.cpp):
1. Initialize serial at 115200 baud
2. Print banner with version and device info
3. Configure battery measurement pins (GPIO 1, 2)
4. **`initializeRTC()`** - Initialize DS3231 RTC (I2C GPIO 14/3), read time/temp/battery (v1.10.0+)
5. **Initialize LittleFS** for learning data persistence
6. `wateringSystem.init()` - Initialize pins and hardware, **loads learning data from flash**
7. `NetworkManager::setWateringSystem()` - Link network manager to watering system
8. `NetworkManager::init()` - Configure MQTT client
9. Idempotent migration: Delete old learning data file if exists (v1.8.5+)
10. Load learning data from LittleFS
11. `NetworkManager::connectWiFi()` - Connect to WiFi (30 retries)
12. `NetworkManager::connectMQTT()` - Connect to MQTT broker (if WiFi available)
13. `setWateringSystemRef()` - **CRITICAL**: Set global pointer for web API
14. `setupOta()` - Initialize web server and OTA
15. **`bootCountdown()`** - 10-second countdown with `/halt` polling (v1.12.0)
16. API handlers registered via `registerApiHandlers()` (called from setupOta)

**loop() sequence** (main.cpp):
1. **First loop only** (if WiFi connected and not halt mode):
   - Send watering schedule to Telegram
   - Smart boot watering: water if first boot OR overdue valves detected
   - Skip if halt mode active (v1.12.0)
2. Check WiFi connection with `NetworkManager::isWiFiConnected()`, reconnect if needed
3. `NetworkManager::loopMQTT()` - Process MQTT messages (includes halt/resume commands)
4. `wateringSystem.processWateringLoop()` - Execute valve state machines + **check auto-watering** (blocked if halt mode)
5. `loopOta()` - Handle web server requests
6. `DebugHelper::loop()` - Flush buffered debug messages to Telegram
7. 10ms delay to prevent watchdog reset

**processWateringLoop() sequence** (WateringSystem.h):
1. **Global Safety Watchdog** - Check all valves for ABSOLUTE_SAFETY_TIMEOUT (v1.11.0)
2. **Check auto-watering** (if not in sequential mode AND not halt mode): For each idle valve with auto-watering enabled, check if tray is empty based on time
3. Process each valve's state machine
3. Handle sequential watering transitions
4. Publish state every 2 seconds

## Known Issues & Gotchas

1. **Baud Rate**: MUST be 115200. Use `--raw` flag if output is gibberish
2. **Filesystem Upload**: Always use `buildfs` before `uploadfs`. Files go to `/web/` not `/data/web/`
3. **API vs Internal Indexing**: Web API uses 1-6, code uses 0-5 internally
4. **Sensor Power - CRITICAL**: Rain sensors require TWO power signals: (1) Valve pin HIGH (specific sensor), (2) GPIO 18 HIGH (common rail). Reading sensors without powering the valve pin will always show WET (LOW). Production firmware reads during watering (valve already open), test firmware must explicitly power both.
5. **Global Pointer**: `setWateringSystemRef()` MUST be called before `setupOta()` in setup(), otherwise web API will fail
6. **Network Independence**: Watering algorithm continues even if WiFi/MQTT disconnects (by design)
7. **LittleFS Initialization Order**: LittleFS MUST be initialized before `wateringSystem.init()` because init() loads learning data
8. **millis() Overflow**: System handles overflow every ~49 days by detecting when current < saved timestamp
9. **Auto-Watering Default**: Auto-watering is ENABLED by default for all valves
10. **Baseline Auto-Update**: Baseline updates automatically when longer fill observed (tray was emptier)
11. **First Watering Assumption**: First watering may not be from empty; system learns true capacity over time
12. **Consumption Smoothing**: Uses weighted average to prevent wild swings from single measurements
13. **Boot Countdown Delay** (v1.12.0): Every boot has 10-second countdown before operations start - allows time for emergency `/halt` command
14. **Halt Mode Persistence**: Halt mode is NOT persistent across reboots - must send `/halt` during countdown window or after boot
15. **Safety Timeout Hierarchy** (v1.12.5): MAX_WATERING_TIME (25s) < ABSOLUTE_SAFETY_TIMEOUT (30s) < Global Watchdog (runs every loop)
16. **DS3231 RTC Dependency** (v1.10.0+): System uses DS3231 as sole time source - no NTP dependency, works without WiFi for timing
17. **NeoPixel LED** (v1.10.0+): GPIO 48 used for RGB status LED on ESP32-S3-N8R2 (not GPIO 2)
18. **Master Overflow Sensor** (v1.12.1): Overflow detection is NOT persistent across reboots - `overflowDetected` flag resets to false on boot, must send `/reset_overflow` if overflow persists after reboot
19. **Overflow Recovery** (v1.12.1): After overflow detected, ALL watering blocked until manual intervention - send `reset_overflow` command ONLY after physically fixing overflow issue
20. **Native Testing** (v1.13.0): Run `pio test -e native` to execute 20 unit tests on your desktop - no ESP32 hardware required, tests validate state machine and learning algorithm logic
21. **Test Firmware Individual Sensors** (v1.13.1): Commands R1-R6 and M1-M6 power only the specific valve pin + GPIO 18, not all valves. This minimizes power consumption and allows precise per-sensor testing. Monitor commands (M1-M6) keep power on until stopped with 'S'.

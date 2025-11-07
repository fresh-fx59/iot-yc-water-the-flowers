# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 smart watering system controlling 6 valves, 6 rain sensors, and 1 water pump. The system waters plants based on rain sensor feedback, publishes state to Yandex IoT Core MQTT, and provides a web interface for control.

**Key Technologies**:
- Platform: ESP32-S3-DevKitC-1 (Espressif32, Arduino framework)
- Filesystem: LittleFS (1MB partition for web UI files)
- Libraries: PubSubClient 2.8 (MQTT), WiFiClientSecure (TLS), WebServer, mDNS
- Current Version: 1.3.4 (defined in main.cpp:8)

## Build & Deploy Commands

### Standard Build/Upload
```bash
# Build and upload firmware only
platformio run -t upload -e esp32-s3-devkitc-1

# Monitor serial output
platformio device monitor -b 115200 --raw
```

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

## Code Organization (v1.4.0 Refactored)

The codebase has been refactored for better maintainability and follows standard C++ organization practices:

### File Structure

```
include/
  ├── config.h                      # All constants and pin definitions
  ├── ValveController.h             # Valve state struct and enums
  ├── WateringSystem.h              # Main watering logic class
  ├── WateringSystemStateMachine.h  # State machine implementation
  ├── NetworkManager.h              # WiFi + MQTT management
  ├── api_handlers.h                # Web API endpoints
  ├── ota.h                         # OTA updates and web server
  └── secret.h                      # WiFi/MQTT credentials (not in git)

src/
  └── main.cpp                      # Entry point (~100 lines)
```

### Key Design Principles

1. **Separation of Concerns**: Each header file has a single, well-defined responsibility
2. **Helper Functions**: Complex logic extracted into small, focused functions (e.g., `LearningAlgorithm` namespace)
3. **Clear Documentation**: Each section is clearly marked with comments
4. **Standard Patterns**: Uses inline functions in headers (common for embedded C++)
5. **Const Correctness**: All configuration constants properly defined

### Working with the Refactored Code

**Adding new features**:
- Hardware config → `config.h`
- Valve logic → `WateringSystem.h` or `WateringSystemStateMachine.h`
- Network features → `NetworkManager.h`
- Web API → `api_handlers.h`

**The main.cpp file** is now clean and minimal (~100 lines vs 1159 lines before):
- Global object declarations
- setup() function
- loop() function
- API handler registration

## Architecture

### Watering Algorithm Flow

The system implements a 5-phase watering cycle per valve to ensure accurate rain sensor readings:

1. **PHASE_IDLE**: Valve is inactive
2. **PHASE_OPENING_VALVE**: Open valve first (sensor needs water flow to function)
3. **PHASE_WAITING_STABILIZATION**: Wait 500ms for valve to open and water to start flowing
4. **PHASE_CHECKING_INITIAL_RAIN**: Check rain sensor (now accurate with flowing water)
   - If already wet: Close valve, abort (pump never turns on)
   - If dry: Proceed to watering phase
5. **PHASE_WATERING**: Turn on pump, monitor sensor every 100ms until wet or 15s timeout
6. **PHASE_CLOSING_VALVE**: Close valve, turn off pump if no other valves active
7. **PHASE_ERROR**: Error state (not currently used)

**Critical Design Decision**: Valve opens BEFORE sensor check because rain sensors require water flow to produce accurate readings. The pump only turns on during the actual watering phase if the sensor is initially dry.

**Sequential Watering**: When `startSequentialWatering()` is called, valves are watered in reverse order (5→0) one at a time. The next valve starts only after the previous completes its cycle.

### Core Classes & System Organization

**Classes are now properly separated into header files** (v1.4.0 refactored):

- **WateringSystem** (WateringSystem.h): Main orchestrator managing 6 ValveController instances
  - `processWateringLoop()`: State machine executor called every loop, processes all valves
  - `startWatering(valveIndex)`: Initiates watering cycle for one valve
  - `startSequentialWatering()`: Waters all valves 5→0 in sequence
  - `startSequentialWateringCustom(indices[], count)`: Custom valve sequence
  - `publishCurrentState()`: MQTT state updates every 2 seconds (cached in `lastStateJson`)
  - `getLastState()`: Returns cached state JSON for web API
  - `clearTimeoutFlag(valveIndex)`: Clears timeout flag for recovery
  - Learning methods: `resetCalibration()`, `resetAllCalibrations()`, `printLearningStatus()`, `setSkipCycles()`

- **ValveController** (ValveController.h): Per-valve state tracking struct
  - Tracks: phase, state, rainDetected, valveOpenTime, wateringStartTime, timeoutOccurred
  - **Learning fields**: baselineFillTime, lastFillTime, skipCyclesRemaining, isCalibrated, totalWateringCycles
  - Each valve operates independently with its own state machine and learning algorithm
  - Helper function: `phaseToString()` for debugging and state publishing

- **NetworkManager** (NetworkManager.h): Combined WiFi + MQTT management
  - WiFi: `connectWiFi()`, `isWiFiConnected()` with 30-attempt retry logic
  - MQTT: `connectMQTT()`, `loopMQTT()`, `isMQTTConnected()`
  - Subscribes to: `$devices/{DEVICE_ID}/commands`
  - Publishes to: `$devices/{DEVICE_ID}/state` and `events`
  - Command parser in `processCommand()` handles all MQTT commands
  - Command helpers: `handleSequenceCommand()`, `handleSkipCyclesCommand()`

- **LearningAlgorithm** namespace (WateringSystem.h): Helper functions for learning logic
  - `calculateSkipCycles()`: Determines cycles to skip based on fill ratio
  - `calculateWaterRemaining()`: Calculates water remaining percentage
  - `calculateConsumptionPercent()`: Gets consumption percentage

**Supporting files**:
- **config.h**: All constants, pin definitions, and configuration
- **ota.h**: OTA updates, web server setup, LittleFS file serving
- **api_handlers.h**: Web API endpoint implementations
- **secret.h**: WiFi/MQTT credentials (never commit)

### Hardware Configuration (ESP32-S3-DevKitC-1)

**Pin Assignments**:
- Pump: GPIO 4
- Valves: GPIO 5, 6, 7, 15, 16, 17
- Rain Sensors: GPIO 8, 9, 10, 11, 12, 13 (INPUT_PULLUP, LOW=wet, HIGH=dry)
- Sensor Power (Optocouple): GPIO 18 (powers sensors only when needed)
- LED: GPIO 2

**Rain Sensor Reading Logic**:
- Sensors powered via GPIO 18 optocoupler (on-demand to save power)
- LOW (0) = WET (sensor pulls to ground when water detected)
- HIGH (1) = DRY (internal pull-up resistor keeps line high when dry)
- Power-on sequence: Set GPIO 18 HIGH, delay 100ms, read sensor, set LOW
- Sensors configured with INPUT_PULLUP mode in setup

### Dynamic Learning Algorithm

**Purpose**: Automatically learns each tray's capacity and water consumption rate to optimize watering frequency.

**How It Works**:

1. **First Watering (Baseline Establishment)**:
   - Assumes tray is empty
   - Records fill time as baseline (e.g., Tray 1: 30s, Tray 2: 90s)
   - Next cycle will water again to measure consumption

2. **Subsequent Waterings (Consumption Analysis)**:
   - Measures current fill time
   - Compares to baseline: `fill_ratio = current_fill_time / baseline_time`
   - If ratio ≈ 1.0 (≥0.95): Tray was empty, water every cycle
   - If ratio < 1.0: Tray had water remaining, calculate skip cycles
   - Formula: `skip_cycles = floor(baseline / (baseline - current)) - 1`

3. **Example Calculation**:
   - Baseline: 30s (tray capacity)
   - Current fill: 10s (only needed 1/3 capacity)
   - Interpretation: Tray had 2/3 water remaining
   - Consumption: 30s - 20s = 10s per cycle
   - Cycles to empty: 30s / 10s = 3 cycles
   - Action: Skip next 2 cycles

**Key Features**:
- Each valve learns independently (different tray sizes supported)
- Adapts to varying consumption rates (temperature, humidity, plant needs)
- Safety limits: Max 15 cycles skip, min 0 cycles
- Persists across waterings but not across reboots
- Learning data published in MQTT state updates

**Edge Cases**:
- Fill ratio < 0.10: Tray >90% full, skip 10 cycles (very slow consumption)
- Timeout occurred: Learning data not updated (sensor may be faulty)
- Manual stop: Learning data not updated (incomplete watering)

### MQTT Commands

Format: Plain text commands sent to `$devices/{DEVICE_ID}/commands`

**Basic Watering**:
- `start_valve_N` (N=0-5): Start single valve
- `stop_valve_N`: Stop single valve
- `start_all`: Sequential watering all valves
- `start_sequence_0,2,4`: Custom sequence (comma-separated indices)
- `stop_all`: Emergency stop all
- `state`: Force state publish
- `clear_timeout_N`: Clear timeout flag for valve N

**Learning Algorithm Commands**:
- `reset_calibration_N`: Reset calibration for valve N (next watering establishes new baseline)
- `reset_all_calibrations`: Reset all valves to uncalibrated state
- `learning_status`: Print detailed learning status for all valves to serial
- `set_skip_cycles_N_X`: Manually set valve N to skip X cycles (e.g., `set_skip_cycles_0_5`)

### Web Interface

**Files** (in `/data/web/`):
- `index.html`: Main control panel
- `css/style.css`: Styling
- `js/app.js`: Frontend logic

**API Endpoints** (defined in main.cpp:872-946):
- `GET /api/water?valve=N` (N=1-6): Start watering
- `GET /api/stop?valve=N` or `?valve=all`: Stop watering
- `GET /api/status`: Get current system state JSON
- `GET /firmware`: OTA update page (auth: admin/OTA_PASSWORD)

**Important**: API uses 1-indexed valves (1-6), internal code uses 0-indexed (0-5)

### Configuration Files

**include/secret.h** (never commit, must exist):
```cpp
#define SSID "wifi_name"
#define SSID_PASSWORD "wifi_password"
#define YC_DEVICE_ID "device_id"
#define MQTT_PASSWORD "mqtt_password"
#define OTA_USER "admin"
#define OTA_PASSWORD "ota_password"
```

### Safety Features

- **Timeout Protection**: Max watering time 15 seconds per valve (MAX_WATERING_TIME)
- **Timeout Flags**: Persist until cleared via `clear_timeout_N` command
- **Pump Coordination**: Pump only runs when valves are in PHASE_WATERING and sensor is dry
- **MQTT Failure Isolation**: Network issues never block watering algorithm

### Testing & Debugging

**Hardware test program**: `test-main.cpp.bak`
To use: rename `src/main.cpp` → `main.cpp.bak`, rename `test-main.cpp.bak` → `src/main.cpp`, then build/upload

Test menu commands (see README.md lines 334-377):
- `L`: Toggle LED
- `R`: Read all rain sensors once
- `M`: Monitor sensors continuously (press `S` to stop)
- `P`: Toggle pump
- `1-6`: Toggle individual valves
- `A`/`Z`: All valves on/off
- `F`: Full automatic test
- `X`: Emergency stop
- `H`: Show menu

**Debug Output**: All system events print to serial console at 115200 baud. Key debug patterns:
- `═══` headers mark major state changes (valve cycle start, sequential mode)
- `✓` prefix for successful operations
- `ERROR:` prefix for failures
- Phase transitions print with step numbers and explanations

**Common Debug Techniques**:
- Watch for "Timeout occurred!" messages during watering
- Check `lastStateJson` cache for web API state discrepancies
- Monitor MQTT connection status (system continues working if MQTT fails)
- Use `platformio device monitor --raw` if seeing garbage characters

## Making Code Changes

### Modifying Watering Logic
- **State machine**: `WateringSystemStateMachine.h` - `processValve()` method
- **Learning algorithm**: `WateringSystem.h` - `processLearningData()`, helper functions in `LearningAlgorithm` namespace
- **Skip cycle check**: `WateringSystem.h` - `startWatering()` method
- **Constants**: `config.h` - `LEARNING_*` constants
- Always test with hardware test program first before deploying full system

### Adding New MQTT Commands
1. Add command parsing in `NetworkManager.h` - `processCommand()` method
2. Create helper method if command is complex (see `handleSequenceCommand()`)
3. Call appropriate WateringSystem method
4. Test via MQTT: `mosquitto_pub -t "$devices/DEVICE_ID/commands" -m "your_command"`

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
- **Learning parameters**: `config.h` - Update `LEARNING_*` constants
- **MQTT settings**: `config.h` - Update `MQTT_*` constants
- **Credentials**: `include/secret.h` (never commit this file)

## Program Flow & Initialization

**setup() sequence** (main.cpp:45-85):
1. Initialize serial at 115200 baud
2. Print banner with version and device info
3. `wateringSystem.init()` - Initialize pins and hardware
4. `NetworkManager::setWateringSystem()` - Link network manager to watering system
5. `NetworkManager::init()` - Configure MQTT client
6. `NetworkManager::connectWiFi()` - Connect to WiFi (30 retries)
7. `NetworkManager::connectMQTT()` - Connect to MQTT broker
8. `setWateringSystemRef()` - **CRITICAL**: Set global pointer for web API
9. `setupOta()` - Initialize web server and OTA
10. API handlers registered via `registerApiHandlers()` (called from setupOta)

**loop() sequence** (main.cpp:91-107):
1. Check WiFi connection with `NetworkManager::isWiFiConnected()`, reconnect if needed
2. `NetworkManager::loopMQTT()` - Process MQTT messages and handle reconnection
3. `wateringSystem.processWateringLoop()` - Execute valve state machines
4. `loopOta()` - Handle web server requests
5. 10ms delay to prevent watchdog reset

## Known Issues & Gotchas

1. **Baud Rate**: MUST be 115200. Use `--raw` flag if output is gibberish
2. **Filesystem Upload**: Always use `buildfs` before `uploadfs`. Files go to `/web/` not `/data/web/`
3. **API vs Internal Indexing**: Web API uses 1-6, code uses 0-5 internally
4. **Sensor Power**: GPIO 18 must go HIGH before reading sensors (100ms stabilization required)
5. **Global Pointer**: `setWateringSystemRef()` MUST be called before `setupOta()` in setup() (main.cpp:836), otherwise web API will fail
6. **Include Structure**: API handlers defined in api_handlers.h but implemented at end of main.cpp (872-946)
7. **Network Independence**: Watering algorithm continues even if WiFi/MQTT disconnects (by design)

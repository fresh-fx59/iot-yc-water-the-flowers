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

**All classes defined in src/main.cpp** (947 lines total):

- **WateringSystem** (main.cpp:111-520): Main orchestrator managing 6 ValveController instances
  - `processWateringLoop()`: State machine executor called every loop, processes all valves
  - `startWatering(valveIndex)`: Initiates watering cycle for one valve
  - `startSequentialWatering()`: Waters all valves 5→0 in sequence
  - `startSequentialWateringCustom(indices[], count)`: Custom valve sequence
  - `publishCurrentState()`: MQTT state updates every 2 seconds (cached in `lastStateJson`)
  - `getLastState()`: Returns cached state JSON for web API
  - `clearTimeoutFlag(valveIndex)`: Clears timeout flag for recovery

- **ValveController** (main.cpp:87-120): Per-valve state tracking struct
  - Tracks: phase, state, rainDetected, valveOpenTime, wateringStartTime, timeoutOccurred
  - **Learning fields**: baselineFillTime, lastFillTime, skipCyclesRemaining, isCalibrated, totalWateringCycles
  - Each valve operates independently with its own state machine and learning algorithm

- **MQTTManager** (main.cpp:522-652): Yandex IoT Core communication
  - Subscribes to: `$devices/{DEVICE_ID}/commands`
  - Publishes state to: `$devices/{DEVICE_ID}/state`
  - Publishes events to: `$devices/{DEVICE_ID}/events`
  - Command parser in `processCommand()` handles all MQTT commands

- **WiFiManager** (main.cpp:654-752): Connection management with 30-attempt retry logic
  - Handles reconnection automatically in loop() if disconnected

**Separate header files**:
- **ota.h**: OTA updates, web server setup, LittleFS file serving
- **api_handlers.h**: API endpoint declarations (implementations at end of main.cpp:872-946)

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
- Core state machine: `processValve()` method in WateringSystem class (main.cpp:~490-650)
- Phase transitions happen in switch statement based on `valve->phase`
- Learning algorithm calculates in PHASE_CLOSING_VALVE (main.cpp:~536-605)
- Skip cycle check happens in `startWatering()` (main.cpp:~194-205)
- Always test with hardware test program first before deploying full system

### Adding New MQTT Commands
1. Add command string parsing in `MQTTManager::processCommand()` (main.cpp:~888-985)
2. Call appropriate WateringSystem method
3. Test via MQTT publish: `mosquitto_pub -t "$devices/DEVICE_ID/commands" -m "your_command"`
4. Update command documentation comment at top of processCommand()

### Adding New API Endpoints
1. Declare function in `api_handlers.h`
2. Implement at end of main.cpp (after line 872)
3. Register in `registerApiHandlers()` function (main.cpp:938-946)
4. Remember: API uses 1-indexed valves, convert to 0-indexed for internal calls

### Modifying Web Interface
1. Edit files in `/data` directory (NOT `/web`)
2. Always rebuild filesystem: `platformio run -t buildfs -e esp32-s3-devkitc-1`
3. Upload filesystem: `platformio run -t uploadfs -e esp32-s3-devkitc-1`
4. Changes won't appear until both buildfs and uploadfs complete

### Changing Pin Assignments
1. Update pin defines at top of main.cpp (lines 17-46)
2. Update arrays: VALVE_PINS[6] and RAIN_SENSOR_PINS[6] (lines 45-46)
3. Update hardware documentation in this file

## Program Flow & Initialization

**setup() sequence** (main.cpp:800-842):
1. Initialize serial at 115200 baud
2. Print banner with version and device info
3. `wateringSystem.init()` - Initialize pins and hardware
4. `MQTTManager::setWateringSystem()` - Link MQTT to watering system
5. `MQTTManager::init()` - Configure MQTT client
6. `WiFiManager::connect()` - Connect to WiFi (30 retries)
7. `MQTTManager::connect()` - Connect to MQTT broker
8. `setWateringSystemRef()` - **CRITICAL**: Set global pointer for web API
9. `setupOta()` - Initialize web server and OTA
10. API handlers registered via `registerApiHandlers()`

**loop() sequence** (main.cpp:844-864):
1. Check WiFi connection, reconnect if needed
2. `MQTTManager::loop()` - Process MQTT messages
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

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 smart watering system controlling 6 valves, 6 rain sensors, and 1 water pump. The system waters plants based on rain sensor feedback, automatically waters when trays are empty using time-based learning, publishes state to Yandex IoT Core MQTT, sends Telegram notifications for watering sessions with queue-based debug system, and provides a web interface for control.

**Key Technologies**:
- Platform: ESP32-S3-DevKitC-1 (Espressif32, Arduino framework)
- Filesystem: LittleFS (1MB partition for web UI and learning data persistence)
- Libraries: PubSubClient 2.8 (MQTT), ArduinoJson 6.21.0 (persistence), WiFiClientSecure (TLS), HTTPClient (Telegram), WebServer, mDNS
- Time Sync: NTP (pool.ntp.org, GMT+3 Moscow timezone)
- Current Version: 1.10.3 (defined in config.h:10)

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
- Test DS3231 RTC (I2C at GPIO 14/3)
- Test water level sensor (GPIO 19)
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

## Code Organization (v1.6.1 - Enhanced Telegram Debug)

The codebase follows standard C++ organization with inline implementations in headers:

### File Structure

```
include/
  ├── config.h                      # All constants and pin definitions
  ├── ValveController.h             # Valve state struct, enums, helper functions
  ├── WateringSystem.h              # Main watering logic + time-based learning
  ├── WateringSystemStateMachine.h  # State machine + MQTT state publishing
  ├── NetworkManager.h              # WiFi + MQTT management
  ├── TelegramNotifier.h            # Telegram Bot API integration
  ├── DebugHelper.h                 # Queue-based debug system with retry & grouping (v1.6.1)
  ├── api_handlers.h                # Web API endpoints
  ├── ota.h                         # OTA updates and web server
  └── secret.h                      # WiFi/MQTT/Telegram credentials (not in git)

src/
  ├── main.cpp                      # Production firmware entry point (~165 lines)
  └── test-main.cpp                 # Hardware test firmware with OTA (~650 lines)

data/
  └── web/                          # Shared filesystem (served via LittleFS)
      ├── prod/                     # Production web UI
      │   ├── index.html
      │   ├── css/style.css
      │   └── js/app.js
      └── test/                     # Test mode web UI
          ├── index.html            # Test mode home page
          └── firmware.html         # OTA upload page

platformio.ini                      # Two build environments:
                                    # - esp32-s3-devkitc-1 (production)
                                    # - esp32-s3-devkitc-1-test (testing)
```

### Key Design Principles

1. **Separation of Concerns**: Each header file has a single, well-defined responsibility
2. **Helper Functions**: Complex logic extracted into namespaces (e.g., `LearningAlgorithm`)
3. **Clear Documentation**: Each section is clearly marked with comments
4. **Standard Patterns**: Uses inline functions in headers (common for embedded C++)
5. **Persistence**: Learning data saved to LittleFS, survives reboots

### Working with the Refactored Code

**Adding new features**:
- Hardware config → `config.h`
- Valve logic → `WateringSystem.h` or `WateringSystemStateMachine.h`
- Network features → `NetworkManager.h`
- Web API → `api_handlers.h`
- Learning algorithm → `WateringSystem.h` (LearningAlgorithm namespace)

**The main.cpp file** is clean and minimal (~110 lines):
- Global object declarations
- LittleFS initialization
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

- **LearningAlgorithm** namespace (WateringSystem.h): Time-based learning helpers
  - `calculateWaterLevelBefore()`: Calculate water level from fill duration ratio
  - `calculateEmptyDuration()`: Estimate time until tray is empty
  - `formatDuration()`: Format milliseconds to human-readable (e.g., "2d 4h")

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

**Supported Command**:
- `start_all`: Start sequential watering of all valves (5→0 order)

**Notes**:
- All other commands have been removed for simplicity
- System automatically publishes state every 2 seconds
- Auto-watering and learning features work automatically in background
- No manual control of individual valves via MQTT

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
- `⚠️ TIMEOUT`: Exceeded maximum watering time (20s)
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

### Safety Features

- **Timeout Protection**: Max watering time 15 seconds per valve (MAX_WATERING_TIME)
- **Timeout Flags**: Persist until cleared via `clear_timeout_N` command
- **Pump Coordination**: Pump only runs when valves are in PHASE_WATERING and sensor is dry
- **MQTT Failure Isolation**: Network issues never block watering algorithm
- **Auto-Watering Safety**: Only triggers when calibrated AND enabled AND tray is calculated empty
- **Data Persistence**: Learning data survives reboots, preventing re-calibration

### Testing & Debugging

**Hardware test firmware**: `src/test-main.cpp`
Build and upload: `platformio run -t upload -e esp32-s3-devkitc-1-test`

**Test menu commands** (via serial at 115200 baud):
- **LED**: `L` - Toggle LED
- **Pump**: `P` - Toggle pump
- **Valves**: `1-6` - Toggle individual valves, `A` - All on, `Z` - All off
- **Rain Sensors**: `R` - Read once, `M` - Monitor continuous, `S` - Stop monitoring
- **Water Level**: `W` - Read once, `N` - Monitor continuous
- **DS3231 RTC**: `T` - Read time/temperature, `I` - Scan I2C bus
- **System**: `F` - Full sequence test, `X` - Emergency stop, `H` - Show menu

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
- **State machine**: `WateringSystemStateMachine.h` - `processValve()` method
- **Learning algorithm**: `WateringSystem.h` - `processLearningData()`, helper functions in `LearningAlgorithm` namespace
- **Time-based skip check**: `WateringSystem.h` - `startWatering()` method (checks time instead of cycles)
- **Auto-watering check**: `WateringSystem.h` - `checkAutoWatering()` method
- **Constants**: `config.h` - `LEARNING_*` constants
- Always test with hardware test program first before deploying full system

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
- **Baseline update logic**: `WateringSystem.h` - `processLearningData()` (lines ~602-608)
- **Consumption calculation**: `LearningAlgorithm::calculateEmptyDuration()` helper
- **Smoothing factor**: Line 641 (currently 70% old, 30% new)
- **Water level calculation**: `LearningAlgorithm::calculateWaterLevelBefore()` helper
- **Auto-watering decision**: `shouldWaterNow()` helper in `ValveController.h`

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
3. **Initialize LittleFS** for learning data persistence
4. `wateringSystem.init()` - Initialize pins and hardware, **loads learning data from flash**
5. `NetworkManager::setWateringSystem()` - Link network manager to watering system
6. `NetworkManager::init()` - Configure MQTT client
7. `NetworkManager::connectWiFi()` - Connect to WiFi (30 retries)
8. **`syncTime()`** - Synchronize time with NTP servers (GMT+3, pool.ntp.org)
9. `NetworkManager::connectMQTT()` - Connect to MQTT broker
10. `setWateringSystemRef()` - **CRITICAL**: Set global pointer for web API
11. `setupOta()` - Initialize web server and OTA
12. API handlers registered via `registerApiHandlers()` (called from setupOta)

**loop() sequence** (main.cpp):
1. Check WiFi connection with `NetworkManager::isWiFiConnected()`, reconnect if needed
2. `NetworkManager::loopMQTT()` - Process MQTT messages and handle reconnection
3. `wateringSystem.processWateringLoop()` - Execute valve state machines + **check auto-watering**
4. `loopOta()` - Handle web server requests
5. 10ms delay to prevent watchdog reset

**processWateringLoop() sequence** (WateringSystem.h):
1. **Check auto-watering** (if not in sequential mode): For each idle valve with auto-watering enabled, check if tray is empty based on time
2. Process each valve's state machine
3. Handle sequential watering transitions
4. Publish state every 2 seconds

## Known Issues & Gotchas

1. **Baud Rate**: MUST be 115200. Use `--raw` flag if output is gibberish
2. **Filesystem Upload**: Always use `buildfs` before `uploadfs`. Files go to `/web/` not `/data/web/`
3. **API vs Internal Indexing**: Web API uses 1-6, code uses 0-5 internally
4. **Sensor Power**: GPIO 18 must go HIGH before reading sensors (100ms stabilization required)
5. **Global Pointer**: `setWateringSystemRef()` MUST be called before `setupOta()` in setup(), otherwise web API will fail
6. **Network Independence**: Watering algorithm continues even if WiFi/MQTT disconnects (by design)
7. **LittleFS Initialization Order**: LittleFS MUST be initialized before `wateringSystem.init()` because init() loads learning data
8. **millis() Overflow**: System handles overflow every ~49 days by detecting when current < saved timestamp
9. **Auto-Watering Default**: Auto-watering is ENABLED by default for all valves
10. **Baseline Auto-Update**: Baseline updates automatically when longer fill observed (tray was emptier)
11. **First Watering Assumption**: First watering may not be from empty; system learns true capacity over time
12. **Consumption Smoothing**: Uses weighted average to prevent wild swings from single measurements

# Description

This code manages ESP32 device. It responsible for watering the flowers. The system consist of 6 valves, 6 rain sensors, 1 water pump, 1 water level sensor, and 1 master overflow sensor.

[Wiring diagram](https://app.cirkitdesigner.com/project/f27de802-dcdb-4096-ad3f-eae88aea3c3f)

[Induction copper plates water level](https://manus.im/share/TcqOH6i7AVr03pMNNCUGFN)

**Version 1.16.2** - GPIO Hardware Reinitialization & Learning Algorithm Improvements!

**Recent Updates:**
- **v1.16.2**: Added GPIO hardware reinitialization to fix stuck relay modules after emergency events. New `/reinit_gpio` command (Telegram/MQTT). Automatic GPIO reset after overflow/water level recovery. Learning algorithm threshold adjusted from 95% to 85% for better calibration.
- **v1.15.6**: Added 10-second confirmation delay to water level sensor - prevents false low-water alarms when water drains back from pipes to tank after pump stops
- **v1.15.5**: Reset learning data files to force immediate recalibration after bug fixes
- **v1.15.4**: Fixed overflow recovery learning + long outage boot detection - prevents interval doubling after overflow events and ensures immediate watering after extended power outages
- **v1.14.0**: Added water level sensor (GPIO 19) - automatically blocks watering when tank is empty, auto-resumes when refilled, sends Telegram notifications
- **v1.13.4**: Fixed rain sensor reading bug - production code now correctly powers both valve pin + GPIO 18 (not just GPIO 18)

## 🏗️ Architectural Improvements (v1.13.0)

### Extracted State Machine Logic

The state machine logic has been extracted into a separate, hardware-independent module for better testability and maintainability:

**StateMachineLogic.h** - Pure state machine logic:
- **Phase transitions**: Idle → Opening → Stabilization → Rain Check → Watering → Closing
- **Built-in safety**: Timeout handling (normal 25s, emergency 30s)
- **Hardware-independent**: No direct GPIO operations, returns actions to execute
- **Pure functions**: Easily testable without ESP32 hardware

**Benefits**:
- ✅ Unit testable on desktop (no ESP32 required)
- ✅ Clear separation of logic and hardware
- ✅ Easier to reason about state transitions
- ✅ Better reliability through comprehensive testing

### Extracted Learning Algorithm

The learning algorithm has been separated into reusable helper functions:

**LearningAlgorithm.h** - Time-based learning helpers:
- `calculateWaterLevelBefore()` - Calculate water level from fill duration ratio
- `calculateEmptyDuration()` - Estimate time until tray is empty based on consumption
- `formatDuration()` - Human-readable duration formatting (e.g., "2d 4h")

**Benefits**:
- ✅ Reusable across different components
- ✅ Independently testable
- ✅ No hardware dependencies
- ✅ Clear, documented algorithms

### Comprehensive Testing Infrastructure

New native testing framework allows testing logic without hardware:

**Test Files**:
- `test/test_native_all.cpp` - Combined test suite (20 tests)
- `test/test_state_machine.cpp` - State machine specific tests (17 tests)
- `test/test_learning_algorithm.cpp` - Learning algorithm tests (3 tests)
- `test/test_overwatering_scenarios.cpp` - Safety scenario tests

**Test Coverage**:
- ✅ All state machine phase transitions
- ✅ Timeout handling (normal & emergency)
- ✅ Full watering cycles from start to finish
- ✅ Learning algorithm calculations
- ✅ Overwatering scenarios and safety measures

**Run Tests**:
```bash
# Run all native tests on your computer (no ESP32 required)
pio test -e native
```

**Documentation**:
- `NATIVE_TESTING_PLAN.md` - Testing strategy and framework
- `OVERWATERING_RISK_ANALYSIS.md` - Safety analysis and mitigation
- `OVERWATERING_TEST_SUMMARY.md` - Test results and validation

## 🛡️ Safety Improvements (v1.11.0 - v1.14.0)

### Multi-Layer Timeout Protection

The system now includes **7 independent safety layers** to prevent overwatering and ensure safe operation:

**Layer 1: Master Overflow Sensor (v1.12.1)**
- **Hardware**: Rain sensor on GPIO 42 (2N2222 transistor circuit)
- **Detection**: LOW = overflow detected, HIGH = normal
- **Response Time**: 100ms polling (fastest safety check)
- **Emergency Actions**:
  - Immediate shutdown of all valves via direct GPIO control
  - Pump stopped immediately
  - All watering operations blocked
  - Telegram alert sent with emergency details
- **Recovery**: Manual intervention required, send `/reset_overflow` command

**Layer 2: Water Level Sensor (v1.14.0, v1.15.6 delay)**
- **Hardware**: Float switch on GPIO 19 (monitors water tank level)
- **Detection**: HIGH = water OK, LOW = tank empty
- **Response Time**: 100ms polling
- **Confirmation Delay** (v1.15.6): 10-second delay before blocking watering - prevents false alarms from pipe drainage after pump stops
- **Automatic Actions**:
  - Blocks all watering operations when tank is empty (after 10s confirmation)
  - Stops active watering immediately if water runs out
  - Sends Telegram notification on low water (after delay expires)
  - Sends Telegram notification when water is restored
  - Automatically cancels blocking if water level rises during 10s delay (pipe drainage detected)
- **Recovery**: Automatic - system resumes normal operation when tank is refilled (no manual intervention needed)

**Layer 3: Safety Timeouts** (v1.12.5)
- MAX_WATERING_TIME: **25 seconds** (normal watering timeout)
- ABSOLUTE_SAFETY_TIMEOUT: **30 seconds** (emergency hard limit)

**Layer 4: Two-Tier State Machine Timeouts**
- Normal timeout (25s): Standard valve closure with learning data processing
- Emergency cutoff (30s): Forces hardware shutdown via direct GPIO control

**Layer 5: Global Safety Watchdog**
- Runs independently every loop iteration
- Bypasses state machine if timeout exceeded
- Forces valves/pump OFF directly via GPIO
- Cannot be blocked by state machine issues

**Layer 6: Enhanced Sensor Logging**
- Logs raw GPIO values every 5 seconds during watering
- Tracks sensor readings for post-incident analysis
- Helps diagnose hardware failures

**Layer 7: Sensor Diagnostic Tools**
- `test_sensors` - Test all 6 sensors and generate report
- `test_sensor_N` (N=0-5) - Test individual sensor
- Detects pullup resistor failures
- Shows power-on/off readings to identify shorts

### 🛑 Emergency Halt Mode (v1.12.0)

**10-Second Boot Countdown for Emergency Updates:**

Every boot provides a safety window for firmware updates:
1. Device boots and connects to WiFi
2. Sends Telegram notification: **"Starting in 10 seconds... Send /halt to prevent operations"**
3. Polls for `/halt` command every 500ms
4. If `/halt` received → enters halt mode (blocks all watering)
5. If countdown expires → normal operation

**Halt Mode Features:**
- ✅ Blocks ALL watering operations (manual, sequential, auto-watering)
- ✅ Stops any active watering immediately
- ✅ Provides OTA firmware update URL
- ✅ Can be activated anytime (not just during boot)
- ✅ Exit with `/resume` command

**Use Cases:**
- Emergency firmware fix after discovering critical bug
- Quick access to OTA without waiting for watering cycle
- Block operations while testing/debugging remotely

**Commands:**
- `/halt` - Enter halt mode (via Telegram or MQTT)
- `/resume` - Exit halt mode (via Telegram or MQTT)

### 🚨 Master Overflow Sensor (v1.12.1)

**Hardware Circuit:**
- Rain sensor detects water overflow from trays
- Connected via 2N2222 transistor circuit to GPIO 42
- Pulls GPIO LOW when water detected (overflow condition)

**Safety Features:**
- ✅ **Highest priority check** - Runs first in every loop (100ms polling)
- ✅ **Immediate emergency stop** - Direct GPIO control bypasses all state machines
- ✅ **Comprehensive shutdown** - Closes all valves, stops pump, blocks all future watering
- ✅ **Telegram emergency alert** - Sends detailed notification with actions taken
- ✅ **Manual recovery required** - Prevents automatic restart after overflow

**Emergency Response:**
When overflow detected:
1. All valves closed immediately via direct GPIO writes
2. Pump stopped
3. LED turned off
4. Sequential mode terminated
5. All future watering attempts blocked
6. Telegram notification sent with timestamp and recovery instructions

**Recovery Process:**
1. Physically fix the overflow issue (empty trays, check for blockage)
2. Send MQTT command: `reset_overflow` or `/reset_overflow`
3. Or use Telegram: `/reset_overflow`
4. System resumes normal operation

**Example Telegram Alert:**
```
🚨🚨🚨 WATER OVERFLOW DETECTED 🚨🚨🚨

⏰ 10-01-2026 14:23:45
🔧 Master overflow sensor triggered
💧 Water is overflowing from tray!

✅ Emergency actions taken:
  • All valves CLOSED
  • Pump STOPPED
  • System LOCKED

⚠️ Manual intervention required!
Send /reset_overflow to resume operations
```

### 💧 Water Level Sensor (v1.14.0)

**Hardware Circuit:**
- Float switch in water tank connected to GPIO 19
- Pulls GPIO HIGH when water present (tank OK)
- Pulls GPIO LOW when no water (tank empty)

**Safety Features:**
- ✅ **High priority check** - Runs second in every loop (100ms polling, right after overflow sensor)
- ✅ **Automatic blocking** - Prevents watering when tank is empty
- ✅ **Emergency stop** - Stops active watering if water runs out mid-cycle
- ✅ **Telegram notifications** - Alerts on low water and when restored
- ✅ **Automatic recovery** - Resumes normal operation when tank is refilled (no manual intervention)

**Automatic Response:**
When water level low detected:
1. All future watering attempts blocked (manual, sequential, auto-watering)
2. If watering is active: valves closed immediately, pump stopped
3. Telegram notification sent with low water alert
4. System continuously monitors for water restoration
5. When water restored: Telegram notification sent, normal operation resumes automatically

**Example Telegram Alerts:**

**Low Water Alert:**
```
⚠️⚠️⚠️ WATER LEVEL LOW ⚠️⚠️⚠️

⏰ 12-01-2026 10:15:32
💧 Water tank is empty or low
🔧 Sensor GPIO 19
⏱️ Confirmed after 10s delay

✅ Actions taken:
  • All valves CLOSED
  • Pump STOPPED
  • Watering BLOCKED

🔄 System will resume automatically when water is refilled
```

**Water Restored:**
```
✅ WATER LEVEL RESTORED ✅

⏰ 12-01-2026 10:45:18
💧 Water tank refilled
🔄 System resuming normal operation

✓ Watering operations enabled
```

## Core Watering Algorithm

The system uses a 5-phase watering cycle per valve:

1. **Open Valve First** - Rain sensors require water flow to function accurately
2. **Wait for Stabilization** - 500ms delay for water to start flowing
3. **Check Rain Sensor** - Now accurate with flowing water
   - If already wet: Close valve, abort (pump never starts)
   - If dry: Proceed to watering
4. **Watering Phase** - Turn on pump, monitor sensor every 100ms
   - Stop when sensor detects water OR 15s timeout
5. **Close Valve** - Turn off pump if no other valves active

This algorithm is used separately for each of 6 valves. State publishes to MQTT topic on each state change. Errors in producing messages to MQTT topics don't affect the algorithm itself.

## 🧠 Time-Based Learning Algorithm (v1.5.0)

The system **automatically learns when each tray is empty** and waters accordingly:

### How It Works

**Time-Based Approach**:
- Tracks actual **time duration** instead of counting cycles
- Learns three key metrics per tray:
  - **Baseline fill time** - Time to fill from completely empty
  - **Empty-to-full duration** - How long tray takes to consume all water
  - **Current water level** - Estimated based on time elapsed

**Adaptive Baseline**:
- First watering establishes initial baseline
- Baseline auto-updates when a longer fill is observed (tray was emptier)
- Uses weighted averaging (70% old, 30% new) for stability

**Automatic Watering**:
- System checks each tray continuously
- When `time_since_last_watering >= empty_to_full_duration` → Auto-water
- Works independently for each valve
- Can be enabled/disabled per valve

**Example**:
```
Watering 1: Fill 5.0s → Baseline: 5.0s (initial calibration)
Watering 2: Fill 4.2s → Baseline: 5.0s, Water before: 16%, Empty time: 24h
Watering 3: Fill 3.8s → Baseline: 5.0s, Water before: 24%, Empty time: ~22h
Watering 4: Fill 5.2s → Baseline: 5.2s ✨ (tray was emptier, baseline updated)
Watering 5: Fill 3.8s → Baseline: 5.2s, Water before: 27%, System stable
```

**Benefits**:
- ✅ Automatic watering when trays are empty
- ✅ Adapts to different tray sizes (different baselines)
- ✅ Learns consumption rate (varying temperatures, plant needs)
- ✅ Each valve operates independently
- ✅ Data persists across reboots (saved to flash)
- ✅ Shows estimated water level percentage and time until empty

### Persistence

- Learning data automatically saved to LittleFS (`/learning_data.json`)
- Survives ESP32 reboots
- Handles millis() overflow (49-day wraparound)
- Manual trigger: Changes saved after each successful watering

**Learning data is published in MQTT state updates** under each valve's `learning` object.

### Learning Algorithm Improvements (v1.15.4)

**Overflow Recovery Protection:**
The system now intelligently handles scenarios where overflow blocks scheduled watering:

**Problem Scenario:**
1. Overflow detected (system locks)
2. Scheduled watering time passes (watering blocked by overflow)
3. User sends `/reset_overflow` command
4. Tray found wet (possibly from rain or manual watering during overflow period)
5. **OLD BEHAVIOR:** System incorrectly doubles watering interval (thinking plant consumed water slowly)
6. **NEW BEHAVIOR:** System detects recent overflow reset, skips learning without penalty

**Implementation:**
- Tracks `lastOverflowResetTime` when overflow is reset
- 2-hour grace period (`OVERFLOW_RECOVERY_THRESHOLD_MS`) after overflow reset
- If tray found wet within grace period: Skip cycle, no interval change
- Prevents incorrect learning from overflow-blocked watering cycles

**Debug Output:**
```
🧠 OVERFLOW RECOVERY DETECTION: Tray wet after overflow reset
  Time since overflow reset: 23s
  Skipping cycle (no interval change) - watering was blocked by overflow
```

**Long Outage Boot Detection:**
The system now correctly handles watering after extended power outages:

**Problem Scenario:**
1. System last watered on Day 0
2. Power outage for 3+ days (longer than millis() can represent)
3. System reboots on Day 3
4. **OLD BEHAVIOR:** `lastWateringCompleteTime` set to 0 (can't represent in millis), `hasOverdueValves()` returns false, no watering triggered
5. **NEW BEHAVIOR:** Stores `realTimeSinceLastWatering` duration, boot logic detects overdue, immediate catch-up watering

**Implementation:**
- New field: `ValveController.realTimeSinceLastWatering` (stores duration when timestamp can't fit in millis)
- `loadLearningData()`: When `currentMillis < timeSinceWatering`, stores real duration instead of 0
- `hasOverdueValves()`: Checks both `lastWateringCompleteTime` and `realTimeSinceLastWatering`
- `shouldWaterNow()`: Uses real duration if timestamp is 0

**Debug Output:**
```
Valve 2 is overdue (interval: 2d 0h)
Overdue valves detected - starting catch-up watering
```

**Benefits:**
- ✅ No false interval doubling after overflow events
- ✅ Reliable watering after extended power outages (days/weeks)
- ✅ Correct learning behavior in all edge cases
- ✅ Maintains schedule stability through disruptions

## 📱 Telegram Bot Notifications (v1.5.0)

The system sends automatic notifications to your Telegram bot during sequential watering sessions.

### Start Notification
Sent when watering begins:
```
🚿 Watering Started
⏰ Session 16-11-2025 19:28:35
🔧 Trigger: MQTT
🌱 Trays: All
```

### Completion Notification
Sent when all valves finish:
```
✅ Watering Complete

tray | duration(sec) | status
-----|---------------|-------
   6 |           3.2 | ✓ OK
   5 |           4.5 | ✓ OK
   4 |           0.5 | ⚠️ ALREADY_WET
   3 |          14.5 | ⚠️ TIMEOUT
   2 |           2.8 | ⚠️ MANUAL_STOP
   1 |           3.8 | ✓ OK
```

**Status Types** (v1.6.1 updated):
- `✓ OK` - Watering completed successfully (sensor became wet after pump started)
- `✓ FULL` - Tray was already full (sensor already wet before pump started)
- `⚠️ TIMEOUT` - Exceeded 25s maximum watering time
- `⚠️ STOPPED` - Watering stopped manually or other interruption

**Configuration** (in `include/secret.h`):
```cpp
#define TELEGRAM_BOT_TOKEN "your_bot_token"
#define TELEGRAM_CHAT_ID "your_chat_id"
```

**Features**:
- Real date/time via NTP sync (GMT+3 Moscow timezone)
- Only triggers during sequential watering (not individual valves)
- Works over WiFi using Telegram Bot API
- Properly aligned table in monospace format

## 🐛 Telegram Debug System (v1.6.1)

The system includes a sophisticated debug message delivery system with automatic retry and message grouping.

### Queue-Based Retry System
- **Circular buffer queue** - Holds up to 20 messages
- **Automatic retry** - Up to 5 attempts per message with 2-second delays
- **Non-blocking** - Processes one message per loop iteration
- **Failure handling** - Messages dropped after 5 failed attempts

### Message Grouping
Debug messages arriving close together are automatically grouped:
- **2-second grouping window** - Messages within 2s are batched
- **3-minute safety limit** - Groups flush after 3 minutes max (prevents infinite buffering)
- **Explicit flush on completion** - All buffered messages sent before watering complete notification
- **Timestamped** - Each message shows exact time: `[DD-MM-YYYY HH:MM:SS.mmm]`

### Configuration (in `include/config.h`)
```cpp
#define IS_DEBUG_TO_SERIAL_ENABLED false    // Enable serial console debug
#define IS_DEBUG_TO_TELEGRAM_ENABLED true   // Enable Telegram debug

const int TELEGRAM_QUEUE_SIZE = 20;
const int TELEGRAM_MAX_RETRY_ATTEMPTS = 5;
const unsigned long TELEGRAM_RETRY_DELAY_MS = 2000;
const unsigned long MESSAGE_GROUP_INTERVAL_MS = 2000;
const unsigned long MESSAGE_GROUP_MAX_AGE_MS = 180000;  // 3 minutes
```

### Example Debug Output
```
🐛 Debug
[17-11-2025 14:23:10.125] ✓ Valve 0 opened - waiting stabilization
[17-11-2025 14:23:10.625] Step 2: Checking rain sensor (water is flowing now)...
[17-11-2025 14:23:10.725] ✓ Sensor 0 is DRY - starting pump (timeout: 20s)
[17-11-2025 14:23:11.825] Valve 0: 1s/19s, Sensor: DRY
[17-11-2025 14:23:12.925] Valve 0: 2s/18s, Sensor: DRY
[17-11-2025 14:23:13.125] ✓ Valve 0 COMPLETE - Total: 3s (pump: 2s)
```

Code was generated in [Claude](https://claude.ai/chat/391e9870-78b7-48cb-8733-b0c53d5dfb42)

---

# Build System & Deployment

This project uses **two separate firmware builds** for clean separation between production and testing.

## Build Environments

| Environment | Source File | Purpose | Flash Usage |
|------------|-------------|---------|-------------|
| `esp32-s3-devkitc-1` | `src/main.cpp` | Production watering system | ~80% (1055 KB) |
| `esp32-s3-devkitc-1-test` | `src/test-main.cpp` | Hardware testing with OTA | ~63% (824 KB) |

## Production Firmware

**Build and upload:**
```bash
platformio run -t upload -e esp32-s3-devkitc-1
platformio device monitor -b 115200 --raw
```

**Features:**
- Full watering system with time-based learning
- WiFi, MQTT, Telegram notifications
- Web interface for control and OTA updates
- Persistent learning data in LittleFS

**Excludes:** Test code (saves ~750 KB flash, better stability)

## Test Firmware

**Build and upload:**
```bash
# Upload firmware (filesystem already contains test HTML)
platformio run -t upload -e esp32-s3-devkitc-1-test

# Monitor serial output
platformio device monitor -b 115200 --raw
```

**Note:** Both production and test firmware share the same filesystem. Upload filesystem once with production firmware, then switch modes without re-uploading filesystem to preserve learning data.

**Features:**
- **🌐 Web Dashboard** at `http://<device-ip>/dashboard`
  - Real-time serial output via WebSocket
  - Interactive test buttons for all commands
  - **Individual sensor testing** (R1-R6 for single read, M1-M6 for continuous monitor)
  - Auto-scrolling console with color-coded output
  - Connection status indicator
  - No serial cable needed for testing!
- Interactive serial menu (press `H` for help)
- Test all hardware: LED, pump, 6 valves, 6 rain sensors (individually or all at once)
- Test DS3231 RTC (I2C at GPIO 14/SDA, GPIO 3/SCL) with current time sync
- Test water level sensor (GPIO 19)
- Test master overflow sensor (GPIO 42)
- I2C bus scanner
- **WiFi & OTA support** - remotely switch back to production firmware

### Test Menu Commands

**Hardware Components:**
- `L` - Toggle LED
- `P` - Toggle pump
- `1-6` - Toggle individual valves
- `A` / `Z` - All valves on/off
- `X` - Emergency stop (turn everything off)

**Rain Sensors (All):**
- `R` - Read all sensors once
- `M` - Monitor all sensors continuously
- `S` - Stop monitoring

**Rain Sensors (Individual):**
- `R1-R6` - Read specific sensor once (e.g., `R1` = Sensor 1: Valve GPIO 5, Sensor GPIO 8)
- `M1-M6` - Monitor specific sensor continuously (e.g., `M6` = Sensor 6: Valve GPIO 17, Sensor GPIO 13)
- `S` - Stop monitoring and power off

**Individual Sensor GPIO Mapping:**
- Sensor 1: Valve GPIO 5, Sensor GPIO 8
- Sensor 2: Valve GPIO 6, Sensor GPIO 9
- Sensor 3: Valve GPIO 7, Sensor GPIO 10
- Sensor 4: Valve GPIO 15, Sensor GPIO 11
- Sensor 5: Valve GPIO 16, Sensor GPIO 12
- Sensor 6: Valve GPIO 17, Sensor GPIO 13

**Water Level Sensor (GPIO 19):**
- `W` - Read water level sensor once (HIGH = water OK, LOW = empty)
- `N` - Monitor water level sensor continuously
- `S` - Stop monitoring

**Master Overflow Sensor (GPIO 42):**
- `O` - Read overflow sensor once
- `V` - Monitor overflow sensor continuously
- `S` - Stop monitoring

**DS3231 RTC (I2C):**
- `T` - Read time/date/temperature
- `I` - Scan I2C bus for devices
- `U` - Set RTC to current time (use web dashboard)
- `K` - Reset RTC to epoch (2000-01-01 00:00:00)
- `B` - Read battery voltage (VBAT)

**System:**
- `F` - Full sequence test (all components including water level sensor)
- `H` - Show menu

## Switching Between Production/Test

### Via USB Cable
```bash
# Switch to test mode (firmware only - preserves learning data!)
platformio run -t upload -e esp32-s3-devkitc-1-test

# Switch back to production (firmware only - preserves learning data!)
platformio run -t upload -e esp32-s3-devkitc-1
```

**Important:** Only upload **firmware**, not filesystem! Both modes share the same filesystem, which contains your learning data.

### Via OTA (Remote)
**Both firmware modes support OTA!** You can switch remotely:

1. Build firmware: `platformio run -e <environment>`
2. Access OTA page:
   - Production mode: `http://<device-ip>/firmware` (login required)
   - Test mode: `http://<device-ip>/firmware` (login required)
3. Upload firmware file:
   - To switch to production: `.pio/build/esp32-s3-devkitc-1/firmware.bin`
   - To switch to test: `.pio/build/esp32-s3-devkitc-1-test/firmware.bin`
4. Device automatically reboots into new firmware

## Why Two Separate Builds?

✅ **Industry Best Practice** - Standard for embedded systems
✅ **No Code Bloat** - Production excludes test code (saves 750 KB)
✅ **Zero Risk** - Test code never runs in production
✅ **Better Stability** - Smaller binary = more reliable
✅ **Clear Separation** - Easier maintenance and debugging

## Shared Filesystem & Learning Data Preservation

**Both firmware modes share the same LittleFS filesystem:**
```
/web/
  ├── prod/                # Production UI
  │   ├── index.html
  │   ├── css/style.css
  │   └── js/app.js
  └── test/                # Test mode UI
      ├── index.html
      └── firmware.html
/learning_data_v1.8.7.json  # Learning data (preserved!)
```

**Key Benefits:**
- ✅ **Upload filesystem ONCE** (with production firmware)
- ✅ **Switch modes anytime** without losing learning data
- ✅ **Learning data persists** across firmware switches
- ⚠️ **Only re-upload filesystem** when updating web UI files

**When to Upload Filesystem:**
- Initial deployment
- Web UI updates (HTML/CSS/JS changes)
- After factory reset / flash erase

**When NOT to Upload Filesystem:**
- Switching between production/test modes
- Firmware updates only
- Any time you want to preserve learning data

---

# Deployment Checklist & Troubleshooting

## ✅ Pre-Deployment Checklist

### 1. File Structure Verification

```bash
# Verify all files exist
find . -type f -name "*.html" -o -name "*.css" -o -name "*.js" -o -name "*.h" -o -name "*.cpp"

# Expected structure:
data/web/index.html           ✓ MUST EXIST
data/web/css/style.css        ✓ MUST EXIST
data/web/js/app.js            ✓ MUST EXIST
include/secret.h              ✓ MUST EXIST (never commit)
include/ota.h                 ✓ MUST EXIST
src/main.cpp                  ✓ MUST EXIST
platformio.ini                ✓ MUST EXIST
```

### 2. Configuration Check

Edit `include/secret.h`:
```cpp
#define SSID "your_wifi_name"
#define SSID_PASSWORD "your_wifi_password"
#define YC_DEVICE_ID "your_device_id"
#define MQTT_PASSWORD "your_mqtt_password"
#define OTA_USER "admin"
#define OTA_PASSWORD "your_ota_password"
#define TELEGRAM_BOT_TOKEN "your_bot_token"
#define TELEGRAM_CHAT_ID "your_chat_id"
```

## 🚀 Step-by-Step Deployment

### Step 1: Clean Previous Build
```bash
platformio run -t clean -e esp32-s3-devkitc-1
```

### Step 2: Build Filesystem Bundle
```bash
# This creates the LittleFS image from /data folder
platformio run -t buildfs -e esp32-s3-devkitc-1
```

Output should show:
```
LittleFS Image Generator
  Data directory: data/
  Resolving files...
    web/index.html (8.2 KB)
    web/css/style.css (6.1 KB)
    web/js/app.js (8.3 KB)
  Size: 22.6 KB / 1536 KB (1.47%)
```

### Step 3: Erase Device Flash
```bash
platformio run -t erase -e esp32-s3-devkitc-1
```

### Step 4: Upload Filesystem
```bash
platformio run -t uploadfs -e esp32-s3-devkitc-1
```

Expected output:
```
esptool.py v3.x.x
Uploading .pio/build/esp32-s3-devkitc-1/littlefs.bin to address 0x3c0000 ...
Writing at 0x3c0000... (X%)
Wrote 24576 bytes at 0x3c0000 in Y seconds...
```

### Step 5: Upload Firmware
```bash
platformio run -t upload -e esp32-s3-devkitc-1
```

### Step 6: Monitor Serial Output
```bash
platformio device monitor -b 115200 --raw
```

Expected startup sequence:
```
=================================
Smart Watering System
Platform: ESP32-S3-DevKitC-1
Version: watering_system_1.4.0
Device ID: [your_device_id]
Valves: 6
=================================

WateringSystem initialized with 6 valves
...
LittleFS mounted successfully
Files in LittleFS:
  - /web (directory)
  - /web/index.html (8192 bytes)
  - /web/css/style.css (6144 bytes)
  - /web/js/app.js (8320 bytes)

Connecting to WiFi...
WiFi Connected!
IP Address: 192.168.x.x

Connecting to Yandex IoT Core...
MQTT Connected!
Setting up OTA...
Control Panel: http://esp32-watering.local/
```

## 🔧 Troubleshooting

### Issue 1: Serial Output Gibberish

**Symptoms:**
```
ÿÿÿÿýþ½ÿÿþ¾ûþýÿÿþ
```

**Solutions:**
1. Check baud rate: Must be **115200**
```bash
platformio device monitor -b 115200
```

2. Try with `--raw` flag:
```bash
platformio device monitor -b 115200 --raw
```

3. Reset device manually and watch output
4. In VS Code PlatformIO: Check Monitor Speed = 115200

### Issue 2: "Not found: /data/web/index.html"

**Symptoms:**
```
Attempting to serve: /web/index.html
ERROR: File not found: /web/index.html
```

**Solutions:**

1. **Verify file exists locally:**
```bash
ls -la data/web/
# Should show: index.html, css/, js/

ls -la data/web/css/
# Should show: style.css

ls -la data/web/js/
# Should show: app.js
```

2. **Rebuild filesystem image:**
```bash
platformio run -t buildfs -e esp32-s3-devkitc-1 --verbose
```

3. **Re-upload filesystem (erase first):**
```bash
platformio run -t erase -e esp32-s3-devkitc-1
platformio run -t uploadfs -e esp32-s3-devkitc-1
```

4. **Check what's actually on device:**
   - Monitor serial output shows file listing:
   ```
   Files in LittleFS:
     - /web/index.html (8192 bytes)
   ```

### Issue 3: LittleFS Not Mounting

**Symptoms:**
```
ERROR: LittleFS Mount Failed
Attempting to format and remount...
```

**Solutions:**

1. Erase entire flash:
```bash
esptool.py --chip esp32s3 erase_flash
```

2. Build everything fresh:
```bash
platformio run -t clean -e esp32-s3-devkitc-1
platformio run -t erase -e esp32-s3-devkitc-1
platformio run -t uploadfs -e esp32-s3-devkitc-1
platformio run -t upload -e esp32-s3-devkitc-1
```

### Issue 4: WiFi Connection Issues

**Symptoms:**
```
WiFi Connection Failed!
```

**Solutions:**
1. Verify credentials in `secret.h`:
   - SSID must match exactly (case-sensitive)
   - Password must be correct
   
2. Check WiFi network availability:
   - Ensure 2.4 GHz (not 5 GHz only)
   - Check SSID is broadcasting

3. Monitor logs:
```bash
platformio run -t monitor -e esp32-s3-devkitc-1
# Look for: "Connecting to WiFi", "WiFi Connected"
```

### Issue 5: MQTT Connection Fails

**Symptoms:**
```
MQTT connection failed, rc=...
```

**Solutions:**
1. Verify WiFi connects first
2. Check MQTT server address and port
3. Verify credentials (YC_DEVICE_ID, MQTT_PASSWORD)
4. Check firewall allows port 8883

## 📊 Verification Commands

### Check File System Size
```bash
platformio run -t buildfs -e esp32-s3-devkitc-1 --verbose 2>&1 | grep -i size
```

### Monitor in Real-Time
```bash
platformio device monitor -b 115200 --pattern=.*
```

### Get Device Info
```bash
platformio run -t info -e esp32-s3-devkitc-1
```

## 🌐 Access System After Deployment

Once fully deployed and connected:

1. **Open control panel:**
   - Browser: `http://esp32-watering.local/`
   - Or: `http://[device_ip]/`

2. **Firmware update:**
   - URL: `http://esp32-watering.local/firmware`
   - Username: `admin`
   - Password: (from secret.h OTA_PASSWORD)

3. **Check status via API:**
   - `http://esp32-watering.local/api/status`

## 📝 Common Commands Cheat Sheet

```bash
# Full deploy (clean → build → upload)
platformio run -t clean -e esp32-s3-devkitc-1 && \
platformio run -t upload -e esp32-s3-devkitc-1 && \
platformio run -t buildfs -e esp32-s3-devkitc-1 && \
platformio run -t uploadfs -e esp32-s3-devkitc-1 && \
platformio device monitor -b 115200 --raw

# Erase and redeploy (nuclear option)
platformio run -t erase -e esp32-s3-devkitc-1 && \
platformio run -t buildfs -e esp32-s3-devkitc-1 && \
platformio run -t uploadfs -e esp32-s3-devkitc-1 && \
platformio run -t upload -e esp32-s3-devkitc-1 && \
platformio device monitor -b 115200 --raw

# Rdeploy (clean → upload)
platformio run -t clean -e esp32-s3-devkitc-1 && \
platformio run -t upload -e esp32-s3-devkitc-1 && \
platformio device monitor -b 115200 --raw
```

## ✨ When Everything Works

Expected signs of success:
- ✅ Serial output readable at 115200 baud
- ✅ WiFi connects automatically
- ✅ MQTT connects
- ✅ Web interface loads at `http://esp32-watering.local/`
- ✅ Can start/stop watering from web UI
- ✅ Status updates in real-time
- ✅ Activity log shows commands
- ✅ Learning algorithm adapting watering frequency

# 🧠 Using the Learning Algorithm

## First Time Setup (Calibration)

After deploying the system for the first time, all valves are **uncalibrated**. Run the first watering cycle to establish baselines:

### Via MQTT:
```bash
mosquitto_pub -h mqtt.cloud.yandex.net -p 8883 --capath /etc/ssl/certs/ \
  -u DEVICE_ID -P MQTT_PASSWORD \
  -t '$devices/DEVICE_ID/commands' -m 'start_all'
```

### Via Web Interface:
- Open `http://esp32-watering.local/`
- Click "Water All Valves"

**What happens during first watering:**
- Each tray fills from "empty" to full
- System records fill time as baseline
- Serial output shows: `🎯 First watering - Establishing baseline`
- After completion: `🎯 BASELINE ESTABLISHED: X.Xs`

## Monitoring Learning Status

### Via Serial Console:
Send MQTT command:
```bash
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'learning_status'
```

Serial output shows:
```
╔═══════════════════════════════════════════╗
║         LEARNING SYSTEM STATUS            ║
╚═══════════════════════════════════════════╝

📊 Valve 0:
  Status: ✓ Calibrated
  Baseline: 30.5s
  Last fill: 10.2s
  Skip cycles: 2
  Total cycles: 5
  Last ratio: 0.33 (33% of baseline)
```

### Via MQTT State:
Subscribe to state topic:
```bash
mosquitto_sub -t '$devices/DEVICE_ID/state' -v
```

Each valve includes time-based learning data in MQTT state (see "Learning Data Structure" section above for full details).

## Understanding Time-Based Learning Output

When a valve is **skipped** because tray is not empty yet (serial console):
```
═══════════════════════════════════════
🧠 SMART SKIP: Valve 0
  Tray not empty yet (water level: ~45%)
  Time since last watering: 12h 0m 0s
  Time until empty: 12h 0m 0s
═══════════════════════════════════════
```

When learning data is **calculated** (after successful watering):
```
🧠 TIME-BASED LEARNING:
  Fill duration: 4.2s
  📏 Baseline: 5.0s (adaptive)
  Water level before: 16%
  Tray state was: empty
  Estimated empty time: 1d 0h
  Learning cycles: 5
  ⏰ Auto-watering enabled - will water when empty
```

When **auto-watering triggers**:
```
⏰ AUTO-WATERING TRIGGERED: Valve 0
  Tray is empty - starting automatic watering
```

## Typical Learning Behavior (Time-Based)

**Scenario 1: Fast Consumption (Summer)**
- Day 1: Fill 5.0s → Baseline: 5.0s, Empty time unknown
- Day 2: Fill 4.8s → Water before: 4%, Empty: 1 day → Auto-waters next day
- Day 3: Fill 5.0s → Water before: 0%, Empty: 1 day → Stable pattern
- System auto-waters daily

**Scenario 2: Slow Consumption (Winter)**
- Day 1: Fill 5.0s → Baseline: 5.0s
- Day 4: Fill 4.2s → Water before: 16%, Empty: ~4 days
- Day 8: Fill 3.8s → Water before: 24%, Empty: ~4 days → Auto-waters every 4 days
- System adapts to slower consumption

**Scenario 3: Different Tray Sizes**
- Valve 0: Baseline 2.0s (small tray) → Empty: 12h → Waters twice daily
- Valve 1: Baseline 8.0s (large tray) → Empty: 3 days → Waters every 3 days
- Valve 2: Baseline 5.0s (medium tray) → Empty: 1 day → Waters daily
- Each learns capacity and consumption independently


# 📋 Quick Reference: MQTT Commands

Replace `DEVICE_ID` with your actual device ID from `secret.h`.

## Watering Commands

**Start Sequential Watering:**
```bash
# Start all valves sequentially (5→0) with Telegram notifications
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'start_all'
```

**What happens**:
- ✅ Waters all 6 trays in sequence (tray 6 → 5 → 4 → 3 → 2 → 1)
- ✅ Sends Telegram start notification with timestamp
- ✅ Tracks duration and status for each tray
- ✅ Sends Telegram completion table when done
- ✅ Updates learning data for each tray
- ✅ Publishes MQTT state every 2 seconds

**Individual Valve Control**:
- Use the web interface at `http://DEVICE_IP/` for manual control
- Auto-watering handles trays automatically when empty
- Learning algorithm adapts to each tray independently

## Safety & Control Commands (v1.11.0+)

**Emergency Halt Mode:**
```bash
# Enter halt mode (blocks all watering operations)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'halt'

# Exit halt mode (resume normal operations)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'resume'
```

**Master Overflow Sensor (v1.12.1):**
```bash
# Reset overflow flag after fixing overflow issue (auto-reinitializes GPIO)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'reset_overflow'

# Or via Telegram: /reset_overflow
```

**GPIO Hardware Reinitialization (v1.16.2):**
```bash
# Force GPIO hardware reinitialization (fixes stuck relays)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'reinit_gpio'

# Or via Telegram: /reinit_gpio
```

**Note:** `/reset_overflow` and water level recovery now automatically reinitialize GPIO hardware. Manual `/reinit_gpio` is useful if relays get stuck without triggering these events.

**Sensor Diagnostics:**
```bash
# Test all 6 sensors and generate diagnostic report
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'test_sensors'

# Test individual sensor (N = 0-5)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'test_sensor_0'
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'test_sensor_1'
# ... etc
```

**Sensor Test Output Example:**
```
═══════════════════════════════════════
🔍 TESTING ALL 6 SENSORS
═══════════════════════════════════════

📊 SENSOR TEST SUMMARY:
Tray | GPIO | Power OFF | Power ON | Status
-----|------|-----------|----------|-------
   1 |    8 | HIGH(DRY) | HIGH(DRY) | ☀️ DRY
   2 |    9 | HIGH(DRY) | HIGH(DRY) | ☀️ DRY
   3 |   10 | HIGH(DRY) | LOW(WET)  | 💧 WET
   4 |   11 | HIGH(DRY) | HIGH(DRY) | ☀️ DRY
   5 |   12 | HIGH(DRY) | HIGH(DRY) | ☀️ DRY
   6 |   13 | LOW(WET)  | LOW(WET)  | 💧 WET ⚠️  <-- HARDWARE FAULT!
```

**Note**: ⚠️ indicates sensor reads WET even when power is OFF (hardware failure)

## Monitoring

```bash
# Subscribe to state updates (includes learning data)
mosquitto_sub -t '$devices/DEVICE_ID/state' -v

# Subscribe to events
mosquitto_sub -t '$devices/DEVICE_ID/events' -v

# Subscribe to all device topics
mosquitto_sub -t '$devices/DEVICE_ID/#' -v
```

## Learning Data Structure (in MQTT State)

Each valve in the state includes a `learning` object:

```json
{
  "pump": "off",
  "sequential_mode": false,
  "water_level": {
    "status": "ok",
    "blocked": false
  },
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

**Water Level Sensor Fields (v1.14.0):**
- `water_level.status`: Current tank water level - "ok" (water present) or "low" (tank empty)
- `water_level.blocked`: Whether watering is blocked due to low water - true (blocked) or false (normal)

**Time-Based Learning Fields (v1.5.0):**
- `calibrated`: Has the valve completed first baseline calibration?
- `auto_watering`: Is automatic watering enabled for this valve?
- `baseline_fill_ms`: Time (ms) to fill tray from completely empty
- `last_fill_ms`: Most recent fill time (ms)
- `empty_duration_ms`: Learned time for tray to go from full to empty (consumption time)
- `total_cycles`: Total successful watering cycles completed
- `water_level_pct`: Current estimated water level (0-100%)
- `tray_state`: Current state: "empty", "full", or "between"
- `time_since_watering_ms`: Time elapsed since last watering
- `time_until_empty_ms`: Estimated time until tray is empty (0 if already empty)
- `last_water_level_pct`: Water level before last watering

---

**Version:** 1.16.2
**Platform:** ESP32-S3-N8R2 (ESP32-S3-DevKitC-1 compatible)
**Framework:** Arduino + PlatformIO
**Features:** Extracted State Machine, Comprehensive Testing, DS3231 RTC, Water Level Sensor with Smart Delay, Master Overflow Sensor, Emergency Halt Mode, 7-Layer Safety System, Time-Based Learning, Overflow Recovery Protection, Long Outage Detection
**Testing:** 20 native tests (no hardware required)
**New in v1.16.2:** GPIO hardware reinitialization to fix stuck relay modules, `/reinit_gpio` command, learning algorithm threshold improved (95% → 85%)

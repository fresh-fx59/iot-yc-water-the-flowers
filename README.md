# Description

This code manages ESP32 device. It responsible for watering the flowers. The system consist of 6 valves, 6 rain sensors and 1 water pump.

**Version 1.10.1** - OTA support added to test firmware for remote hardware debugging!

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
- `⚠️ TIMEOUT` - Exceeded 20s maximum watering time
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
# Upload filesystem first (for OTA web UI)
platformio run -t uploadfs -e esp32-s3-devkitc-1-test

# Upload firmware
platformio run -t upload -e esp32-s3-devkitc-1-test

# Monitor serial output
platformio device monitor -b 115200 --raw
```

**Features:**
- Interactive serial menu (press `H` for help)
- Test all hardware: LED, pump, 6 valves, 6 rain sensors
- Test DS3231 RTC (I2C at GPIO 14/SDA, GPIO 3/SCL)
- Test water level sensor (GPIO 19)
- I2C bus scanner
- **WiFi & OTA support** - remotely switch back to production firmware
- Web interface at `http://<device-ip>/` (no MQTT/Telegram)

### Test Menu Commands

**Hardware Components:**
- `L` - Toggle LED
- `P` - Toggle pump
- `1-6` - Toggle individual valves
- `A` / `Z` - All valves on/off
- `X` - Emergency stop (turn everything off)

**Rain Sensors:**
- `R` - Read all sensors once
- `M` - Monitor continuously
- `S` - Stop monitoring

**Water Level Sensor (GPIO 19):**
- `W` - Read once
- `N` - Monitor continuously

**DS3231 RTC (I2C):**
- `T` - Read time/date/temperature
- `I` - Scan I2C bus for devices

**System:**
- `F` - Full sequence test (all components)
- `H` - Show menu

## Switching Between Production/Test

### Via USB Cable
```bash
# Switch to test mode
platformio run -t uploadfs -e esp32-s3-devkitc-1-test  # Upload test web UI
platformio run -t upload -e esp32-s3-devkitc-1-test    # Upload firmware

# Switch back to production
platformio run -t upload -e esp32-s3-devkitc-1
```

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

## Watering Command (v1.5.0)

**Note**: v1.5.0 simplified MQTT interface to single command. Auto-watering handles individual valves automatically.

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

**Version:** 1.10.1
**Platform:** ESP32-S3-DevKitC-1
**Framework:** Arduino + PlatformIO

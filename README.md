# Description

This code manages ESP32 device. It responsible for watering the flowers. The system consist of 6 valves, 6 rain sensors and 1 water pump.

**Version 1.5.0** - Now with **Time-Based Learning Algorithm** that automatically waters when trays are empty, plus **Telegram notifications** for watering sessions!

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

**Status Types**:
- `✓ OK` - Watering completed successfully
- `⚠️ TIMEOUT` - Exceeded 20s maximum watering time
- `⚠️ ALREADY_WET` - Sensor was already wet when valve opened
- `⚠️ MANUAL_STOP` - Watering stopped manually

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

Code was generated in [Claude](https://claude.ai/chat/391e9870-78b7-48cb-8733-b0c53d5dfb42)


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

# How to Test Your Hardware
Step 1: Upload the Test Program

rename src/main.cpp to main.cpp.bak test-main.cpp.bak to test-main.cpp
Build and upload to your ESP32:

bash   platformio run -t upload -e esp32-s3-devkitc-1
   platformio device monitor -b 115200 --raw
```

### Step 2: Systematic Testing

Once uploaded, you'll see a menu. Test components in this order:

#### **Test 1: LED (Verify ESP32 is working)**
- Type: `L`
- **Expected:** Onboard LED toggles ON/OFF
- **If fails:** ESP32 issue or GPIO 2 problem

#### **Test 2: Rain Sensors (Read First)**
- Type: `R`
- **Expected:** Shows readings for all 6 sensors
  - `HIGH (1) = DRY ☀`
  - `LOW (0) = WET ☔`
- **Test:** Touch each sensor with wet finger to see value change
- **If always LOW:** Check pull-up resistors or wiring
- **If always HIGH:** Sensor not connected or broken

#### **Test 3: Monitor Rain Sensors Continuously**
- Type: `M`
- **Expected:** Live updating display every 500ms
- **Test:** Touch sensors with wet finger - should see bars change
- Type: `S` to stop monitoring

#### **Test 4: Pump**
- Type: `P`
- **Expected:** Relay clicks, pump turns ON
- Type: `P` again to turn OFF
- **⚠ WARNING:** Make sure pump has water!

#### **Test 5: Individual Valves**
- Type: `1`, `2`, `3`, `4`, `5`, `6`
- **Expected:** Each valve relay clicks and opens/closes
- Toggle each one ON and OFF to verify

#### **Test 6: All Valves Together**
- Type: `A` - Opens all valves
- Type: `Z` - Closes all valves
- **⚠ WARNING:** Need good water pressure!

#### **Test 7: Full Automatic Sequence**
- Type: `F`
- **Runs:** LED → Pump → Each valve individually → Rain sensors
- **Great for:** Complete system verification

### Emergency Commands
- `X` - **EMERGENCY STOP** - Turns everything OFF immediately
- `H` - Show menu again

## What to Check

### ✅ Rain Sensors (Most Important!)
```
Dry sensor should read: 1 (HIGH)
Wet sensor should read: 0 (LOW)
If stuck at one value:

Always LOW: Wiring short or no pull-up
Always HIGH: Sensor disconnected

---

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

**Version:** 1.5.0
**Platform:** ESP32-S3-DevKitC-1
**Framework:** Arduino + PlatformIO

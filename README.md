# Description

This code manages ESP32 device. It responsible for watering the flowers. The system consist of 6 valves, 6 rain sensors and 1 water pump.

**Version 1.4.0** - Now with **Dynamic Learning Algorithm** that automatically adapts watering frequency based on consumption patterns!

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

## 🧠 Dynamic Learning Algorithm (NEW in v1.4.0)

The system now **automatically learns each tray's capacity and consumption rate** to optimize watering:

### How It Works

**First Watering (Baseline)**:
- System assumes tray is empty
- Records fill time as baseline (e.g., Tray 1: 30s, Tray 2: 90s, Tray 3: 60s)
- Each tray learns its own capacity

**Subsequent Waterings (Smart Skip)**:
- Measures current fill time
- Compares to baseline: `fill_ratio = current_time / baseline_time`
- If ratio ≈ 1.0: Tray was empty → water every cycle
- If ratio < 1.0: Tray had water remaining → skip cycles

**Example**:
```
Cycle 1 (Calibration):
  Tray 1: 30s → Baseline = 30s
  Tray 2: 90s → Baseline = 90s
  Tray 3: 60s → Baseline = 60s

Cycle 2 (Learning):
  Tray 1: 10s (33% of baseline) → Tray had 67% water → Skip next 2 cycles
  Tray 2: 80s (89% of baseline) → Tray had 11% water → Skip next 8 cycles
  Tray 3: 60s (100% of baseline) → Tray was empty → Water every cycle
```

**Benefits**:
- ✅ Saves water by not watering full trays
- ✅ Adapts to different tray sizes automatically
- ✅ Adjusts to varying consumption (temperature, humidity, plant needs)
- ✅ Each valve operates independently
- ✅ Safety limits: Max 15 cycles skip

### Learning Commands (via MQTT)

- `reset_calibration_N` - Reset valve N calibration (re-learn baseline)
- `reset_all_calibrations` - Reset all valves to uncalibrated state
- `learning_status` - Print detailed learning status to serial console
- `set_skip_cycles_N_X` - Manually override: set valve N to skip X cycles

**Learning data is published in MQTT state updates** under each valve's `learning` object.

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

Each valve includes learning data:
```json
{
  "valve": 0,
  "learning": {
    "calibrated": true,
    "baseline_ms": 30500,
    "last_fill_ms": 10200,
    "skip_cycles": 2,
    "total_cycles": 5,
    "fill_ratio": 0.33
  }
}
```

## Understanding Learning Output

When a valve is **skipped** (serial console):
```
═══════════════════════════════════════
🧠 SMART SKIP: Valve 0
  Tray not empty yet based on consumption pattern
  Baseline fill time: 30s
  Last fill time: 10s
  Cycles remaining to skip: 1
═══════════════════════════════════════
```

When learning data is **calculated** (after successful watering):
```
🧠 LEARNING DATA:
  Fill time this cycle: 10.5s
  Baseline: 30.0s
  Fill ratio: 0.35 (1.0 = tray was empty)
  Tray had ~65% water remaining
  Consumption per cycle: ~35%
  Cycles to empty: 1.9
  Action: Skip next 0 cycle(s)
```

## Resetting Calibration

**Reset single valve** (useful when changing tray):
```bash
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'reset_calibration_0'
```

**Reset all valves** (fresh start):
```bash
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'reset_all_calibrations'
```

**Manually override skip cycles** (valve 0, skip 3 cycles):
```bash
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'set_skip_cycles_0_3'
```

## Typical Learning Behavior

**Scenario 1: Stable Consumption**
- Cycle 1: Baseline 30s → calibrated
- Cycle 2: Fill 30s (100%) → Skip 0 cycles (water every time)
- System learns plants consume all water between cycles

**Scenario 2: Slow Consumption**
- Cycle 1: Baseline 30s → calibrated
- Cycle 2: Fill 10s (33%) → Skip 2 cycles
- Cycle 5: Fill 5s (17%) → Skip 5 cycles
- System adapts to slower consumption (cooler weather, less sunlight)

**Scenario 3: Different Tray Sizes**
- Valve 0: Baseline 20s (small tray) → waters every cycle
- Valve 1: Baseline 60s (large tray) → skips 3 cycles
- Valve 2: Baseline 45s (medium tray) → skips 1 cycle
- Each learns independently

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

## Basic Watering Commands

```bash
# Start single valve (0-5)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'start_valve_0'

# Stop single valve
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'stop_valve_0'

# Start all valves sequentially (5→0)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'start_all'

# Start custom sequence (valves 0, 2, 4)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'start_sequence_0,2,4'

# Emergency stop all
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'stop_all'

# Force state publish
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'state'

# Clear timeout flag for valve 0
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'clear_timeout_0'
```

## Learning Algorithm Commands (v1.4.0+)

```bash
# Reset calibration for single valve (valve 0)
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'reset_calibration_0'

# Reset all valves to uncalibrated state
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'reset_all_calibrations'

# Print detailed learning status to serial console
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'learning_status'

# Manually set valve 0 to skip 5 cycles
mosquitto_pub -t '$devices/DEVICE_ID/commands' -m 'set_skip_cycles_0_5'
```

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
  "valves": [
    {
      "id": 0,
      "state": "closed",
      "phase": "idle",
      "rain": false,
      "timeout": false,
      "learning": {
        "calibrated": true,
        "baseline_ms": 30500,
        "last_fill_ms": 10200,
        "skip_cycles": 2,
        "total_cycles": 5,
        "fill_ratio": 0.33
      }
    }
  ]
}
```

**Learning Fields Explained:**
- `calibrated`: Has the valve completed first baseline calibration?
- `baseline_ms`: Time (ms) to fill tray from empty to full
- `last_fill_ms`: Most recent fill time (ms)
- `skip_cycles`: How many watering cycles to skip before next watering
- `total_cycles`: Total successful watering cycles completed
- `fill_ratio`: `last_fill_ms / baseline_ms` (1.0 = tray was empty, 0.0 = tray was full)

---

**Version:** 1.4.0
**Platform:** ESP32-S3-DevKitC-1
**Framework:** Arduino + PlatformIO

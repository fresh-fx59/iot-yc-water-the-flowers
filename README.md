# Description

This code manages ESP32 device. It responsible for watering the flowers. The system consist of 6 valves, 6 rain sensors and 1 water pump.

Algorithm to water the flowers is as follows:
* rain sensor on
* get data if rain sensor is wet
* if rain sensor is wet, end algorithm
* if rain sensor is dry, then open valve
* turn on pump and monitor rain sensor
* if rain sensor wet, then close the valve and turn off the pump
* end algorithm

This algorithm is used separately for each of 6 valves. State produces to MQTT topic on each state changes. Errors in producing messages to MQTT topics don't affect the algorithm itself.

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
Version: watering_system_1.0.0
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
# CLAUDE.md

ESP32-S3 smart watering system: 6 valves, 6 rain sensors, 1 pump. Time-based learning, MQTT state publishing, Telegram notifications, web interface.

**Stack**: ESP32-S3-N8R2, LittleFS, PubSubClient 2.8, ArduinoJson 6.21.0, DS3231 RTC (GPIO 14/3), Adafruit NeoPixel 1.15.2
**Version**: 1.19.3 (config.h:10)
**Testing**: 30 native unit tests (desktop, no hardware)

## Build & Deploy

**Environments**: `esp32-s3-devkitc-1` (prod, src/main.cpp) | `esp32-s3-devkitc-1-test` (test, src/test-main.cpp)

```bash
# Production
platformio run -t upload -e esp32-s3-devkitc-1
platformio device monitor -b 115200 --raw

# Test (web dashboard at http://<ip>/dashboard, serial menu 'H', OTA, no prod logic)
platformio run -t upload -e esp32-s3-devkitc-1-test

# Filesystem (shared between prod/test)
platformio run -t buildfs -e esp32-s3-devkitc-1
platformio run -t uploadfs -e esp32-s3-devkitc-1

# Full clean deploy
platformio run -t erase -e esp32-s3-devkitc-1 && platformio run -t buildfs -e esp32-s3-devkitc-1 && platformio run -t uploadfs -e esp32-s3-devkitc-1 && platformio run -t upload -e esp32-s3-devkitc-1 && platformio device monitor -b 115200 --raw

# Quick redeploy
platformio run -t clean -e esp32-s3-devkitc-1 && platformio run -t upload -e esp32-s3-devkitc-1 && platformio device monitor -b 115200 --raw
```

## File Structure (v1.13.0)

```
include/
  config.h, DS3231RTC.h, StateMachineLogic.h, LearningAlgorithm.h, ValveController.h,
  WateringSystem.h, WateringSystemStateMachine.h, NetworkManager.h, TelegramNotifier.h,
  DebugHelper.h, api_handlers.h, ota.h, TestConfig.h, secret.h
src/
  main.cpp (prod), test-main.cpp (test)
test/
  test_native_all.cpp, test_state_machine.cpp, test_learning_algorithm.cpp, test_overwatering_scenarios.cpp
data/web/prod/ data/web/test/
platformio.ini (3 envs: esp32-s3-devkitc-1, esp32-s3-devkitc-1-test, native)
```

**Design**: Separation of concerns, hardware-independent logic (StateMachineLogic.h, LearningAlgorithm.h), inline headers, LittleFS persistence

**Feature placement**: config.h (hw), StateMachineLogic.h (state), LearningAlgorithm.h (learning), WateringSystem.h (valve logic), NetworkManager.h (network), api_handlers.h (API), test/ (tests)

**Tests**: `pio test -e native` (20 tests, no ESP32 required)

## Architecture

### StateMachineLogic.h (v1.13.0)
Pure, testable state machine returning actions instead of executing hardware ops.
- `ProcessResult`: phase, action, timestamps
- `Action`: OPEN_VALVE, CLOSE_VALVE, TURN_PUMP_ON/OFF, READ_SENSOR, EMERGENCY_STOP
- `processValveLogic()`: pure state transition function (17 tests)

Usage: `result = StateMachineLogic::processValveLogic(...)` → execute `result.action` → update state

### LearningAlgorithm.h (v1.13.0)
Pure time-based learning helpers (3 tests):
- `calculateWaterLevelBefore(fillDuration, baselineFillDuration)`: water level %
- `calculateEmptyDuration(fillDuration, baselineFillDuration, timeSinceLastWatering)`: time until empty
- `formatDuration(ms)`: human-readable format

### Testing (v1.13.0)
`pio test -e native` - 20 tests (desktop, no ESP32): state transitions, timeouts, learning calcs, safety scenarios
Docs: NATIVE_TESTING_PLAN.md, OVERWATERING_RISK_ANALYSIS.md, OVERWATERING_TEST_SUMMARY.md

### Safety Features (v1.11.0-v1.14.0)

**L1: Master Overflow Sensor** (v1.15.1, GPIO 42, 100ms poll, software debounced 5/7 threshold, WateringSystem.h:510-595): LOW=overflow → `emergencyStopAll()`, blocks all watering. Debouncing prevents false triggers from electrical noise. Recovery: `reset_overflow`

**L2: Water Level Sensor** (v1.14.0, GPIO 19, 100ms poll, 11s continuation time v1.15.7): HIGH=water OK, LOW=empty → blocks all watering, auto-resume when refilled. 11s continuation allows active watering to finish when tank runs low. Telegram notifications on low/restored

**L3: Timeouts** (config.h:69-105, v1.16.0 per-valve):
- Normal timeout: Valve-specific (25-40s based on flow characteristics)
- Emergency timeout: 5s higher than normal per valve
- Valve 0 (Tray 1): 40s normal / 45s emergency (slower flow rate)
- Valves 1-5: 25s normal / 30s emergency (standard flow)

**L4: Two-Tier SM Timeouts** (WateringSystemStateMachine.h:77-106): Per-valve normal (learning data), per-valve emergency (force GPIO)

**L5: Global Watchdog** (WateringSystem.h:479-520): `globalSafetyWatchdog()` every loop, bypasses SM, force GPIO off

**L6: Sensor Logging** (WateringSystem.h:832-838): Log GPIO every 5s during watering

**L7: Diagnostics** (WateringSystem.h:1530-1627): `testSensor(idx)`, `testAllSensors()`, MQTT: `test_sensors`, `test_sensor_N`

### Halt Mode (v1.12.0)

**Boot Countdown** (main.cpp:92-180): 10s window, polls Telegram every 500ms for `/halt`, then normal operation

**Implementation** (WateringSystem.h:133-206): `haltMode` flag blocks all watering (startWatering, startSequentialWatering, checkAutoWatering). Commands: `/halt`, `/resume` (Telegram+MQTT)

**Polling** (TelegramNotifier.h:182-232): `checkForCommands()` uses Telegram getUpdates, offset for dedupe, 1s timeout

### Per-Valve Timeout Configuration (v1.16.0)

**Configuration** (config.h:71-105):
- Defined in `VALVE_NORMAL_TIMEOUTS[]` and `VALVE_EMERGENCY_TIMEOUTS[]` arrays
- Access via `getValveNormalTimeout(index)` and `getValveEmergencyTimeout(index)`
- Compile-time validation ensures emergency > normal + 5s
- Different trays have different water flow rates/sensor characteristics
- Tray 1 requires longer timeout (40s vs 25s) due to hardware differences

**Migration from v1.15.x:**
- Global timeouts (MAX_WATERING_TIME, ABSOLUTE_SAFETY_TIMEOUT) still exist for backward compatibility
- No learning data migration required (timeouts are not persisted)
- If you had custom timeout values in config.h, update corresponding array entries

### Watering Flow

5-phase cycle: IDLE → OPENING_VALVE → WAITING_STABILIZATION (500ms) → CHECKING_INITIAL_RAIN (wet=abort, dry=continue) → WATERING (pump on, 100ms sensor poll) → CLOSING_VALVE (process learning)

**Critical**: Valve opens BEFORE sensor check (sensors need water flow). Pump only on if initially dry.
**Sequential**: `startSequentialWatering()` waters 5→0, one at a time.
**Auto**: Every loop checks if tray empty (time-based) AND auto-watering enabled.

### Core Classes

**WateringSystem** (WateringSystem.h): 6 ValveControllers, processWateringLoop(), checkAutoWatering(), startWatering(), startSequentialWatering(), publishCurrentState() (2s, cached), saveLearningData(), loadLearningData(), resetCalibration(), setAutoWatering()

**ValveController** (ValveController.h): phase, state, rainDetected, timestamps, learning fields (lastWateringCompleteTime, emptyToFullDuration, baselineFillDuration, lastFillDuration, isCalibrated, autoWateringEnabled). Helpers: phaseToString(), calculateCurrentWaterLevel(), getTrayState(), shouldWaterNow()

**NetworkManager** (NetworkManager.h): WiFi (connectWiFi, 30 retries), MQTT (connectMQTT, loopMQTT), sub: `$devices/{ID}/commands`, pub: `$devices/{ID}/state`, processCommand()

**LearningAlgorithm** namespace: calculateWaterLevelBefore(), calculateEmptyDuration(), formatDuration()

**StateMachineLogic** namespace: processValveLogic() → ProcessResult

**Support**: config.h (constants, pins), ota.h, api_handlers.h, secret.h, TestConfig.h

### Hardware (ESP32-S3-N8R2)

**Pins**: Pump=4, Valves=5/6/7/15/16/17, Rain Sensors=8/9/10/11/12/13 (INPUT_PULLUP, LOW=wet), Sensor Power=18, Overflow=42 (INPUT_PULLUP, LOW=overflow), Water Level=19 (INPUT_PULLUP, HIGH=water, LOW=empty), LED=48, RTC I2C SDA=14/SCL=3/0x68, Battery ADC=1/Ctrl=2

**Relay Module** (v1.14.1): 6-channel relay with automatic sensor control (https://aliexpress.ru/item/32831141593.html). Hardware-level safety: when rain sensor detects WET, relay automatically opens (cuts power) regardless of GPIO state. No GPIO verification performed after digitalWrite due to this feature - read-back would show LOW when sensor is wet (expected behavior).

**Sensor Logic (CRITICAL, fixed v1.13.4)**: TWO power signals required: (1) Valve pin HIGH, (2) GPIO 18 HIGH. Sequence: valve HIGH → GPIO 18 HIGH → delay 100ms → read → power off. LOW=WET, HIGH=DRY. v1.13.4 fixed readRainSensor() to power both signals correctly.

**Overflow** (v1.12.1): 2N2222 circuit, GPIO 42, 100ms poll, LOW=emergency

**Water Level** (v1.14.0): Float switch, GPIO 19, 100ms poll, HIGH=water OK, LOW=empty. Auto-blocks watering, sends Telegram, auto-resumes when refilled. v1.15.7: 11s continuation time (WATER_LEVEL_LOW_DELAY) - allows active watering to finish when tank runs low, prevents debug spam

**DS3231 RTC** (v1.10.0+): Time source (no NTP), CR2032, temp sensor, 100kHz I2C, DS3231RTC.h

**LED**: GPIO 48, status indication, off in halt mode

### Adaptive Learning (v1.8.0+)

Binary search/gradient ascent for optimal watering interval (max fill time). Per-tray learning.

**Phases**:
1. Exponential: full→double(2x), fill<95%baseline→+1x, fill>baseline→update+1x
2. Binary: fill≈baseline→-0.5x, fill↓→+0.25x, fill↑→+0.25x

**Constants** (processLearningData, WateringSystem.h:1285-1297): BASELINE_TOLERANCE=0.70 (v1.17.4), FILL_STABLE_TOLERANCE_MS=500, BASE_INTERVAL_MS=86400000 (24h), MIN_INTERVAL_MULTIPLIER=1.0, adjustments: INTERVAL_DOUBLE=2.0, INTERVAL_INCREMENT_LARGE=1.0, INTERVAL_DECREMENT_BINARY=0.5, INTERVAL_INCREMENT_FINE=0.25. Also AUTO_WATERING_MIN_INTERVAL_MS=86400000 (config.h:130) enforces 24h floor in shouldWaterNow()

**Interval Tuning Guide** (v1.17.4): `emptyToFullDuration = BASE_INTERVAL_MS × intervalMultiplier`. Algorithm paths: fill < BASELINE_TOLERANCE×baseline → +1.0x (penalty); fill > baseline → new baseline + 1.0x; fill ≈ baseline & stable → -0.5x (only decrease path); fill ≈ baseline & changed → +0.25x. To water MORE often: lower BASELINE_TOLERANCE (shrinks penalty zone), lower INTERVAL_INCREMENT_LARGE (less aggressive growth), increase INTERVAL_DECREMENT_BINARY (faster recovery), lower MIN_INTERVAL_MULTIPLIER or BASE_INTERVAL_MS (lower floor). To water LESS often: reverse. Reset calibration by swapping LEARNING_DATA_FILE filenames in WateringSystem.h:26-29

**Example**: 15s→baseline15s/1x(24h) → full→2x(48h) → 10s→3x(72h) → 16s→baseline16s/4x(96h) → 16s→3.5x(84h) → 15s→3.75x(90h) → 16s→3.25x(78h) ✓optimal

**Features**: Self-adjusting, adaptive baseline, persists `/learning_data.json`, handles millis overflow, two-file migration

**Boot Logic** (v1.8.1+, fixed v1.8.4/v1.8.5): First boot→water all | Overdue→water | Schedule→skip. v1.8.4: safe default no-water. v1.8.5: detect calibrated+zero timestamps as overdue

**Restart Detection** (v1.15.2): Prevents interval explosion after power outages/restarts. If tray is found wet within 2 hours (RECENT_WATERING_THRESHOLD_MS) of last watering, skip cycle without punishing (no interval doubling). Only doubles interval if tray genuinely stayed wet for >2 hours (slow consumption). Critical for stability with frequent power cycles.

**Overflow Recovery Protection** (v1.15.4): Prevents incorrect interval doubling after overflow events. When overflow is reset and tray found wet within 2 hours (OVERFLOW_RECOVERY_THRESHOLD_MS), skips learning without punishment. Addresses scenario: overflow blocks watering → scheduled time passes → overflow reset → tray still wet (rain/manual refill) → system now correctly skips cycle instead of doubling interval. Tracks `lastOverflowResetTime` in WateringSystem.h.

**Long Outage Boot Detection** (v1.15.4): Fixes missed watering after extended power outages (>49 days or when millis() can't represent timestamp). Previously: after reboot, `loadLearningData()` set `lastWateringCompleteTime=0` when time offset exceeded millis range → `hasOverdueValves()` skipped check → no watering. Now: stores `realTimeSinceLastWatering` duration in ValveController when timestamp can't be represented → boot logic detects overdue valves using real duration → immediate catch-up watering. Critical fix for reliable operation after long outages.

**Schedule Stability After Reboot** (v1.15.8): Fixes watering schedule display shifting to boot time after power outages. Previously: `sendWateringSchedule()` calculated time since last watering using `currentTime - lastWateringCompleteTime`, but after reboot `lastWateringCompleteTime=0` when millis() couldn't represent the timestamp → schedule displayed as if watering happened at boot. Now: schedule calculation checks for `lastWateringCompleteTime==0 && realTimeSinceLastWatering>0` and uses real time duration instead. Schedule now remains stable across reboots, showing correct planned watering time based on actual last watering timestamp.

**Timeout Retry for Uncalibrated Valves** (v1.16.3): Automatic 24-hour retry when timeout occurs on first watering. Previously: timeout on uncalibrated valve → skip learning → no auto-retry → manual intervention required. Now: timeout on uncalibrated valve → sets `lastWateringAttemptTime=currentTime`, `emptyToFullDuration=86400000` (24h), keeps `isCalibrated=false` → auto-watering triggers retry after 24h → if successful, establishes baseline. Multiple timeouts reschedule 24h retries. Implemented in WateringSystem.h:1397-1419 (processLearningData) and ValveController.h:175-180 (shouldWaterNow). Maintains 24h minimum interval safety, persistent across reboots, no interference with calibrated valves.

**Schedule Display Fix** (v1.16.5): Fixed mismatch between displayed watering schedule and actual auto-watering trigger time. Previously: schedule showed planned time based on learned `emptyToFullDuration` (e.g., 19.6h), but auto-watering enforced 24h minimum safety interval (AUTO_WATERING_MIN_INTERVAL_MS), causing valves to skip scheduled waterings. Now: `sendWateringSchedule()` checks both learned interval and 24h minimum, displays the later of the two → schedule accurately reflects when watering will actually occur. Fixes issue where fast-consuming trays (< 24h) showed incorrect schedules and missed waterings. Implemented in WateringSystem.h:2007-2036.

**Thread-Safe MQTT Publishing** (v1.16.6): Fixed MQTT disconnection blocking auto-watering. Previously: `publishCurrentState()` and `publishStateChange()` called `mqttClient.publish()`/`mqttClient.connected()` from Core 1 (watering loop), while `loopMQTT()`/`connectMQTT()` ran on Core 0 (network task). PubSubClient is not thread-safe → concurrent access during MQTT reconnection (TLS handshake, blocking delays) could hang the main loop → `checkAutoWatering()` never triggered. Now: Core 1 only caches state JSON and sets `volatile bool mqttPublishPending` flag. Core 0 calls `publishPendingMQTTState()` to publish via MQTT. `publishStateChange()` no longer accesses mqttClient (state changes captured in periodic 2s state updates). Boot watering logic also no longer requires WiFi. Implemented in WateringSystemStateMachine.h:355-380, WateringSystem.h:90, main.cpp:78-80.

**Thread-Safe Telegram Notifications** (v1.17.0): Removed all network calls from Core 1 watering loop. Previously: `checkMasterOverflowSensor()`, `checkWaterLevelSensor()`, `checkAutoWatering()`, `startSequentialWatering()`, `startNextInSequence()`, `sendScheduleUpdateIfNeeded()`, and `processValve()` made direct HTTPClient/TelegramNotifier calls with 10s timeouts from Core 1 → if WiFi down or Telegram API slow, sensor monitoring, auto-watering, and safety watchdogs stalled. Now: all Telegram notifications go through `notificationQueue` (8-slot ring buffer). Core 1 calls `queueTelegramNotification()` with pre-formatted messages (using `TelegramNotifier::formatWateringStarted/Complete/Schedule()`). Core 0 calls `processPendingNotifications()` in networkTask to send queued messages via `sendTelegramDebug()`. Also removed `delay(500)` from `sendScheduleUpdateIfNeeded()` and WiFi check from `sendWateringSchedule()`. Implemented in TelegramNotifier.h (format methods), WateringSystem.h (queue + all call sites), WateringSystemStateMachine.h (processValve), main.cpp:82.

**Auto-Watering After Reboot Fix** (v1.17.2): Fixed auto-watering never triggering after reboot when millis() can't represent the timestamp. Previously: `loadLearningData()` stored `realTimeSinceLastWatering` as a frozen snapshot at boot time when `millis()` was too small to represent the original timestamp. `shouldWaterNow()` compared this frozen value against `emptyToFullDuration` without adding elapsed `millis()` → auto-watering never triggered because the snapshot never grew. Now: all 4 locations that use `realTimeSinceLastWatering` (shouldWaterNow, sendWateringSchedule x2, hasOverdueValves) add `currentTime` (millis()) to the frozen boot snapshot to get the actual elapsed time since last watering. Fixed in ValveController.h:197 and WateringSystem.h:1844,1894,1998.

**MQTT Outage Notification Throttling** (v1.17.1): Suppresses Telegram spam for brief MQTT disconnections. Previously: every MQTT disconnect/reconnect cycle sent 5 messages to Telegram (disconnect, connecting, connected, subscribed, published). Now: outage tracking in NetworkManager.h with `mqttDisconnectedSince` timestamp. Short outages (<10 min, `MQTT_OUTAGE_NOTIFY_THRESHOLD_MS=600000` in config.h) are completely silent in Telegram. Long outages (≥10 min) send one notification when threshold reached, then one summary with duration on reconnect. Initial boot connection always logged. `mqttSilentReconnect` flag suppresses `connectMQTT()` and `publishConnectionEvent()` Telegram output during short outages.

**WiFi & MQTT Reconnection with Backoff** (v1.17.3): Fixed WiFi driver corruption and MQTT blocking that caused complete system failure after long outages. Two problems: (1) `connectWiFi()` called `WiFi.begin()` without `WiFi.disconnect()` first → WiFi driver corruption after thousands of calls. (2) `connectMQTT()` had 5-attempt retry loop with 30s TLS timeout per attempt → blocked Core 0 for up to 175s per cycle → starved Telegram, OTA, and debug output. Now: `NetworkManager::loopWiFi()` method with `WiFi.disconnect(true)` cleanup + exponential backoff (5s→300s). `connectMQTT()` reduced to single attempt (max 5s via `wifiClient.setTimeout(5)` + `mqttClient.setSocketTimeout(5)`), backoff logic moved to `loopMQTT()` (5s→300s). Both WiFi and MQTT use same pattern: single short attempt + exponential backoff between attempts. Core 0 max blocking reduced from 175s to ~5s per network operation. `WiFi.setAutoReconnect(true)` after successful connections. Implemented in NetworkManager.h, config.h (`WIFI_RECONNECT_*`, `MQTT_RECONNECT_*` constants).

### MQTT

**Commands** (`$devices/{ID}/commands`): `start_all` (seq 5→0), `halt`/`resume`, `test_sensors`, `test_sensor_N`, `reset_overflow` (clear overflow + reinit GPIO), `reinit_gpio` (force GPIO hardware reset for stuck relays)

**State** (`$devices/{ID}/state`, 2s): JSON with pump, sequential_mode, water_level{status, blocked}, valves[id, state, phase, rain, timeout, learning{calibrated, auto_watering, baseline_fill_ms, last_fill_ms, empty_duration_ms, total_cycles, water_level_pct, tray_state, time_since_watering_ms, time_until_empty_ms, last_water_level_pct}]

**Notes**: Auto-publish 2s, individual valve via web only, Telegram also accepts `/halt`/`resume`/`reset_overflow`/`reinit_gpio`

### Telegram

**Config** (secret.h): TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID

**Commands** (v1.16.2):
- `/halt` - Enter halt mode (blocks all watering, OTA ready)
- `/resume` - Exit halt mode (restore normal operation)
- `/time` - Show current RTC time, temperature, battery
- `/settime` - Auto-sync time from NTP (Moscow UTC+3)
- `/settime YYYY-MM-DD HH:MM:SS` - Manual time set (e.g. `/settime 2026-01-12 14:30:00`)
- `/reset_overflow` - Clear overflow sensor flag and reinitialize GPIO hardware
- `/reinit_gpio` - Force GPIO hardware reinitialization (for stuck relay recovery)
- `/lamp` or `/lamp_status` - Show plant lamp state, mode, GPIO, and schedule
- `/lamp_on` - Turn plant lamp ON manually (pauses auto schedule)
- `/lamp_off` - Turn plant lamp OFF manually (pauses auto schedule)
- `/lamp_auto` - Return plant lamp to automatic schedule (22:00 -> 07:00)

**Dashboard/API** (v1.18.1):
- `/api/lamp?action=on|off|auto` - Web dashboard/manual HTTP control for plant lamp
- Plant lamp relay is configured active-low in `config.h` (`PLANT_LIGHT_ACTIVE_HIGH = false`)

**Notifications** (sequential watering only): Start (session ID, trigger, trays) + Completion table (tray, duration, status: OK/FULL/TIMEOUT/STOPPED). TelegramNotifier.h, HTTPS api.telegram.org, WateringSessionData

**Debug** (v1.6.1, DebugHelper.h, config.h): Circular buffer (20 msgs), retry (5×, 2s delay), grouping (2s window, 3min max), timestamped [DD-MM-YYYY HH:MM:SS.mmm]
Usage: `DebugHelper::debug()`, `debugImportant()`, `flushBuffer()`, `loop()`

### Web Interface

**Files** (/data/web/): index.html, css/style.css, js/app.js
**API**: /api/water?valve=N (N=1-6), /api/stop?valve=N|all, /api/status, /api/lamp?action=on|off|auto, /api/reset_calibration?valve=N|all, /firmware (auth: admin/OTA_PASSWORD)
**Note**: API 1-indexed (1-6), internal 0-indexed (0-5)
**Dashboard**: Shows pump, system, and plant lamp status; supports manual lamp ON/OFF/AUTO plus valve watering controls

### Config (secret.h, never commit)
SSID, SSID_PASSWORD, YC_DEVICE_ID, MQTT_PASSWORD, OTA_USER, OTA_PASSWORD, TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID

### Safety Summary
Timeout (25s), emergency cutoff (30s), pump in PHASE_WATERING only, MQTT isolation, auto-water (calibrated+enabled+empty), persistence, 30 tests, auto-retry on timeout (24h)

### Testing & Debug

**Native**: `pio test -e native` (desktop, no hw)
**HW test**: test-main.cpp, `platformio run -t upload -e esp32-s3-devkitc-1-test`

**Serial Menu** (115200): L (LED), P (pump), 1-6/A/Z (valves), R/M/S (all sensors), R1-R6/M1-M6/S (individual sensors, 500ms), W/N (water level), T/I (RTC), F (full seq), X (emergency), H (help)

**Individual sensors**: R1-R6 (GPIO 5/6/7/15/16/17→8/9/10/11/12/13), powers specific valve+GPIO18 only

**Firmware switch**: Test: esp32-s3-devkitc-1-test | Prod: esp32-s3-devkitc-1 | OTA: upload .bin from .pio/build/

**Debug patterns** (115200): ═══ (state), 🧠 (learning), ✨ (baseline), ⏰ (auto), ✓ (success), ERROR (fail)

**Techniques**: Check timeouts, lastStateJson, MQTT status, learning_status, /learning_data.json, --raw flag

## Code Changes

**Watering**: StateMachineLogic.h (processValveLogic), WateringSystemStateMachine.h (processValve), LearningAlgorithm.h (helpers), WateringSystem.h (processLearningData, startWatering, checkAutoWatering), config.h (LEARNING_* constants). Test: `pio test -e native` first

**MQTT**: NetworkManager.h processCommand(), test: `mosquitto_pub -t "$devices/ID/commands" -m "start_all"`. MQTT simplified (start_all only), use web API or auto-watering for individual control

**API**: api_handlers.h (inline), register in main.cpp registerApiHandlers(), API 1-indexed→internal 0-indexed, access g_wateringSystem_ptr

**Web**: Edit `data/web/prod/` files, then `pio run -t buildfs -e esp32-s3-devkitc-1` and `pio run -t uploadfs -e esp32-s3-devkitc-1`

**Config**: config.h (pins, timing, learning, MQTT), secret.h (creds, never commit)

**Learning**: LearningAlgorithm.h (pure funcs), WateringSystem.h (processLearningData, smoothing 70/30), ValveController.h (shouldWaterNow), test_learning_algorithm.cpp

**Persistence**: `/learning_data_v1.17.4.json` active, `/learning_data_v1.16.2.json` auto-deleted on boot, save after watering/reset, load on init(), swap filenames in `WateringSystem.h` to reset all calibrations

## Program Flow

**setup()**: Serial 115200 → banner → battery pins → initializeRTC() → LittleFS → wateringSystem.init() (loads data) → NetworkManager::setWateringSystem/init → migration → load data → connectWiFi (30 retries) → connectMQTT → setWateringSystemRef (CRITICAL for API) → setupOta → bootCountdown (10s, /halt poll) → registerApiHandlers

**loop()**: First loop (WiFi+no halt): schedule→smart boot watering (first boot|overdue) | Every loop: WiFi check/reconnect → loopMQTT (halt/resume) → processWateringLoop (SM+auto, blocked if halt) → loopOta → DebugHelper::loop → 10ms delay

**processWateringLoop()**: Watchdog (ABSOLUTE_SAFETY_TIMEOUT) → auto-water check (idle+enabled+empty, no seq/halt) → process each valve SM → seq transitions → publish state (2s)

## Gotchas

1. Baud 115200, --raw if gibberish
2. For UI changes, edit `data/web/prod/` then run `buildfs` before `uploadfs`
3. API 1-6, internal 0-5
4. **CRITICAL**: Sensors need TWO power: valve pin HIGH + GPIO 18 HIGH. Fixed v1.14.2: GPIO 18 stays HIGH continuously during PHASE_WATERING (not pulsed). Previous bug: GPIO 18 turned off after each read → sensors blind 90% of time. Fixed v1.13.4: readRainSensor() powers both. Test mode: powers all valves+GPIO18 continuously
5. setWateringSystemRef() BEFORE setupOta() or API fails
6. Watering continues if WiFi/MQTT down (design)
7. LittleFS before wateringSystem.init() (loads data)
8. millis() overflow ~49d handled
9. Auto-watering default ON
10. Baseline auto-updates (longer fill)
11. First watering may not be empty, learns over time
12. Consumption smoothing (weighted avg)
13. 10s boot countdown for /halt
14. Halt NOT persistent (send during countdown/after boot)
15. Timeout hierarchy: 25s < 30s < watchdog
16. DS3231 sole time source (no NTP)
17. LED GPIO 48 (not 2)
18. Overflow NOT persistent (flag resets on boot)
19. Overflow blocks all watering until reset_overflow
20. Native tests: `pio test -e native` (20 tests, no hw)
21. Test R1-R6/M1-M6 power specific valve+GPIO18 only, M*=continuous until 'S'
22. **v1.15.1**: Overflow sensor uses software debouncing (5/7 readings must be LOW). Test mode shows raw single readings for diagnostics. Production requires 5 out of 7 consecutive LOW readings to trigger overflow, filtering electrical noise from pump/valve switching
23. **v1.15.7**: Water level sensor has 11s continuation time before blocking watering. Allows active watering cycles to complete when tank runs low. Debug message only logs once (not every 100ms) to prevent spam. System blocks watering only if LOW persists for full 11 seconds
24. **v1.16.2**: GPIO hardware reinitialization fixes stuck relay modules. After emergency stops (overflow, water level), `resetOverflowFlag()` and water level recovery automatically call `reinitializeGPIOHardware()` which reinitializes all valve/pump pins to known good state. Manual trigger: `/reinit_gpio` (Telegram/MQTT). Fixes issue where relay modules stay stuck after emergency events and require physical power cycle
25. **v1.16.6**: NEVER access `mqttClient` from Core 1 (watering loop). PubSubClient is not thread-safe. All MQTT publishing goes through `mqttPublishPending` flag → Core 0 calls `publishPendingMQTTState()`. Watering must never depend on network connectivity
26. **v1.17.0**: NEVER make HTTP/Telegram calls from Core 1 (watering loop). All Telegram notifications go through `notificationQueue` → Core 1 calls `queueTelegramNotification(message)` → Core 0 calls `processPendingNotifications()`. Use `TelegramNotifier::formatWateringStarted/Complete/Schedule()` to build messages without network calls
27. **v1.17.1**: MQTT disconnect/reconnect messages are suppressed from Telegram for outages < 10 minutes (`MQTT_OUTAGE_NOTIFY_THRESHOLD_MS`). Only long outages get Telegram notifications. Adjust threshold in config.h if needed
28. **v1.17.3**: WiFi reconnection uses `loopWiFi()` with exponential backoff (5s→300s) and `WiFi.disconnect(true)` cleanup. NEVER call `WiFi.begin()` repeatedly without `WiFi.disconnect(true)` first — it corrupts the ESP32 WiFi driver. Initial boot `connectWiFi()` (30 retries) is only for first connection

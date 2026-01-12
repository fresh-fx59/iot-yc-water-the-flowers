# CLAUDE.md

ESP32-S3 smart watering system: 6 valves, 6 rain sensors, 1 pump. Time-based learning, MQTT state publishing, Telegram notifications, web interface.

**Stack**: ESP32-S3-N8R2, LittleFS, PubSubClient 2.8, ArduinoJson 6.21.0, DS3231 RTC (GPIO 14/3), Adafruit NeoPixel 1.15.2
**Version**: 1.13.5 (config.h:10)
**Testing**: 20 native unit tests (desktop, no hardware)

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

### Safety Features (v1.11.0-v1.12.5)

**L1: Master Overflow Sensor** (GPIO 42, 100ms poll, WateringSystem.h:530-595): LOW=overflow → `emergencyStopAll()`, blocks all watering. Recovery: `reset_overflow`

**L2: Timeouts** (config.h:66-67): MAX_WATERING_TIME=25s, ABSOLUTE_SAFETY_TIMEOUT=30s

**L3: Two-Tier SM Timeouts** (WateringSystemStateMachine.h:77-106): 25s (normal, learning data), 30s (emergency, force GPIO)

**L4: Global Watchdog** (WateringSystem.h:479-520): `globalSafetyWatchdog()` every loop, bypasses SM, force GPIO off

**L5: Sensor Logging** (WateringSystem.h:832-838): Log GPIO every 5s during watering

**L6: Diagnostics** (WateringSystem.h:1530-1627): `testSensor(idx)`, `testAllSensors()`, MQTT: `test_sensors`, `test_sensor_N`

### Halt Mode (v1.12.0)

**Boot Countdown** (main.cpp:92-180): 10s window, polls Telegram every 500ms for `/halt`, then normal operation

**Implementation** (WateringSystem.h:133-206): `haltMode` flag blocks all watering (startWatering, startSequentialWatering, checkAutoWatering). Commands: `/halt`, `/resume` (Telegram+MQTT)

**Polling** (TelegramNotifier.h:182-232): `checkForCommands()` uses Telegram getUpdates, offset for dedupe, 1s timeout

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

**Pins**: Pump=4, Valves=5/6/7/15/16/17, Rain Sensors=8/9/10/11/12/13 (INPUT_PULLUP, LOW=wet), Sensor Power=18, Overflow=42 (INPUT_PULLUP, LOW=overflow), LED=48, RTC I2C SDA=14/SCL=3/0x68, Battery ADC=1/Ctrl=2

**Sensor Logic (CRITICAL)**: TWO power signals required: (1) Valve pin HIGH, (2) GPIO 18 HIGH. Sequence: valve HIGH → GPIO 18 HIGH → delay 100ms → read → power off. LOW=WET, HIGH=DRY.

**Overflow** (v1.12.1): 2N2222 circuit, GPIO 42, 100ms poll, LOW=emergency

**DS3231 RTC** (v1.10.0+): Time source (no NTP), CR2032, temp sensor, 100kHz I2C, DS3231RTC.h

**LED**: GPIO 48, status indication, off in halt mode

### Adaptive Learning (v1.8.0+)

Binary search/gradient ascent for optimal watering interval (max fill time). Per-tray learning.

**Phases**:
1. Exponential: full→double(2x), fill<95%baseline→+1x, fill>baseline→update+1x
2. Binary: fill≈baseline→-0.5x, fill↓→+0.25x, fill↑→+0.25x

**Constants** (processLearningData): BASELINE_TOLERANCE=0.95, FILL_STABLE_TOLERANCE_MS=500, adjustments: 2.0/1.0/0.5/0.25

**Example**: 15s→baseline15s/1x(24h) → full→2x(48h) → 10s→3x(72h) → 16s→baseline16s/4x(96h) → 16s→3.5x(84h) → 15s→3.75x(90h) → 16s→3.25x(78h) ✓optimal

**Features**: Self-adjusting, adaptive baseline, persists `/learning_data.json`, handles millis overflow, two-file migration

**Boot Logic** (v1.8.1+, fixed v1.8.4/v1.8.5): First boot→water all | Overdue→water | Schedule→skip. v1.8.4: safe default no-water. v1.8.5: detect calibrated+zero timestamps as overdue

### MQTT

**Commands** (`$devices/{ID}/commands`): `start_all` (seq 5→0), `halt`/`resume`, `test_sensors`, `test_sensor_N`, `reset_overflow`

**State** (`$devices/{ID}/state`, 2s): JSON with pump, sequential_mode, valves[id, state, phase, rain, timeout, learning{calibrated, auto_watering, baseline_fill_ms, last_fill_ms, empty_duration_ms, total_cycles, water_level_pct, tray_state, time_since_watering_ms, time_until_empty_ms, last_water_level_pct}]

**Notes**: Auto-publish 2s, individual valve via web only, Telegram also accepts `/halt`/`resume`/`reset_overflow`

### Telegram

**Config** (secret.h): TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID

**Notifications** (sequential watering only): Start (session ID, trigger, trays) + Completion table (tray, duration, status: OK/FULL/TIMEOUT/STOPPED). TelegramNotifier.h, HTTPS api.telegram.org, WateringSessionData

**Debug** (v1.6.1, DebugHelper.h, config.h): Circular buffer (20 msgs), retry (5×, 2s delay), grouping (2s window, 3min max), timestamped [DD-MM-YYYY HH:MM:SS.mmm]
Usage: `DebugHelper::debug()`, `debugImportant()`, `flushBuffer()`, `loop()`

### Web Interface

**Files** (/data/web/): index.html, css/style.css, js/app.js
**API**: /api/water?valve=N (N=1-6), /api/stop?valve=N|all, /api/status, /firmware (auth: admin/OTA_PASSWORD)
**Note**: API 1-indexed (1-6), internal 0-indexed (0-5)

### Config (secret.h, never commit)
SSID, SSID_PASSWORD, YC_DEVICE_ID, MQTT_PASSWORD, OTA_USER, OTA_PASSWORD, TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID

### Safety Summary
Timeout (25s), emergency cutoff (30s), pump in PHASE_WATERING only, MQTT isolation, auto-water (calibrated+enabled+empty), persistence, 20 tests

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

**Web**: Edit /data (NOT /web), buildfs then uploadfs

**Config**: config.h (pins, timing, learning, MQTT), secret.h (creds, never commit)

**Learning**: LearningAlgorithm.h (pure funcs), WateringSystem.h (processLearningData, smoothing 70/30), ValveController.h (shouldWaterNow), test_learning_algorithm.cpp

**Persistence**: /learning_data.json (active), /learning_data_v5.json (auto-deleted), save after watering/reset, load on init(), swap filenames to reset

## Program Flow

**setup()**: Serial 115200 → banner → battery pins → initializeRTC() → LittleFS → wateringSystem.init() (loads data) → NetworkManager::setWateringSystem/init → migration → load data → connectWiFi (30 retries) → connectMQTT → setWateringSystemRef (CRITICAL for API) → setupOta → bootCountdown (10s, /halt poll) → registerApiHandlers

**loop()**: First loop (WiFi+no halt): schedule→smart boot watering (first boot|overdue) | Every loop: WiFi check/reconnect → loopMQTT (halt/resume) → processWateringLoop (SM+auto, blocked if halt) → loopOta → DebugHelper::loop → 10ms delay

**processWateringLoop()**: Watchdog (ABSOLUTE_SAFETY_TIMEOUT) → auto-water check (idle+enabled+empty, no seq/halt) → process each valve SM → seq transitions → publish state (2s)

## Gotchas

1. Baud 115200, --raw if gibberish
2. buildfs before uploadfs, files→/web/ not /data/web/
3. API 1-6, internal 0-5
4. **CRITICAL**: Sensors need TWO power: valve pin HIGH + GPIO 18 HIGH. Without valve pin→always WET. Prod: valve open during read, test: power both
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

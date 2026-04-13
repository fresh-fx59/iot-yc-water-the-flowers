# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ESP32-S3 smart watering system: 6 valves, 6 rain sensors, 1 pump, 1 plant lamp. Time-based learning, Telegram notifications, web interface.

**Stack**: ESP32-S3-N8R2, LittleFS, ArduinoJson 6.21.0, DS3231 RTC (GPIO 14/3), Adafruit NeoPixel
**Version**: 1.20.6 (config.h:10)
**Testing**: 36 native unit tests (desktop, no hardware)

## Build & Deploy

**Environments**: `esp32-s3-devkitc-1` (prod, src/main.cpp) | `esp32-s3-devkitc-1-test` (test, src/test-main.cpp) | `native` (tests)

```bash
# Tests (run before any logic changes)
pio test -e native

# Production build + flash + monitor
platformio run -t upload -e esp32-s3-devkitc-1
platformio device monitor -b 115200 --raw

# Test firmware (web dashboard, serial menu 'H', OTA, no prod logic)
platformio run -t upload -e esp32-s3-devkitc-1-test

# Filesystem (required after editing data/web/prod/ files)
platformio run -t buildfs -e esp32-s3-devkitc-1
platformio run -t uploadfs -e esp32-s3-devkitc-1

# Full clean deploy
platformio run -t erase -e esp32-s3-devkitc-1 && platformio run -t buildfs -e esp32-s3-devkitc-1 && platformio run -t uploadfs -e esp32-s3-devkitc-1 && platformio run -t upload -e esp32-s3-devkitc-1 && platformio device monitor -b 115200 --raw

# Quick redeploy
platformio run -t clean -e esp32-s3-devkitc-1 && platformio run -t upload -e esp32-s3-devkitc-1 && platformio device monitor -b 115200 --raw
```

## Commit Style

Version-prefixed, imperative, short: `v1.19.3: increase tray 2 watering timeout to 35s`

## Architecture

### Dual-Core Design (CRITICAL)

The ESP32-S3 runs two tasks on separate cores with strict isolation:

- **Core 0** (networkTask, 8KB stack, 100ms loop): WiFi, Telegram, OTA, web server, MetricsPusher. Owns all network I/O.
- **Core 1** (loop, 10ms loop): Watering state machine, sensor polling, safety watchdogs, auto-watering. NEVER touches network.

**Thread-safety rules**:
- NEVER make HTTP/Telegram/metrics calls from Core 1. Use `queueTelegramNotification(message)` which goes through a 16-slot FreeRTOS queue (`notificationQueue`). Core 0 drains it via `processPendingNotifications()`.
- `MetricsPusher::log()` is safe to call from Core 1 — it only writes to a circular buffer (no network).
- Use `TelegramNotifier::formatWateringStarted/Complete/Schedule()` to build messages without network calls.

### Header-Only Design

All logic lives in `include/*.h` as inline headers. Source files (`src/main.cpp`, `src/test-main.cpp`) are thin entry points.

**Key modules**:
- `config.h` — pins, timing constants, per-valve timeouts
- `StateMachineLogic.h` — pure state machine, returns actions (no hardware ops), 36 tests
- `LearningAlgorithm.h` — pure learning math helpers (water level, empty duration)
- `WateringSystemStateMachine.h` — executes SM actions on hardware (processValve)
- `WateringSystem.h` — orchestrator: 6 ValveControllers, auto-watering, learning, persistence, safety
- `ValveController.h` — per-valve state: phase, learning data, shouldWaterNow()
- `NetworkManager.h` — WiFi management with exponential backoff reconnection
- `TelegramNotifier.h` — bot commands, notification formatting, debug messaging
- `PlantLightController.h` — relay control with auto schedule (22:00-07:00)
- `api_handlers.h` — web API endpoints (inline, registered in main.cpp)
- `DS3231RTC.h` — RTC time source (no NTP), battery voltage, temperature
- `MetricsPusher.h` — Prometheus metrics + Loki log push (Core 0)
- `DebugHelper.h` — debug logging: `debug()` routes to Loki via `g_metricsLog` callback, `debugImportant()` routes to both Telegram and Loki
- `ota.h` — OTA firmware updates
- `secret.h` — credentials (never commit)
- `TestConfig.h` — test environment stubs

**Feature placement**: config.h (hw constants), StateMachineLogic.h (state transitions), LearningAlgorithm.h (learning math), WateringSystem.h (orchestration + learning policy), NetworkManager.h (network), api_handlers.h (API), MetricsPusher.h (monitoring), test/ (tests)

### Watering Flow

5-phase cycle: IDLE → OPENING_VALVE → WAITING_STABILIZATION (500ms) → CHECKING_INITIAL_RAIN (wet=abort, dry=continue) → WATERING (pump on, 100ms sensor poll) → CLOSING_VALVE (process learning)

**Critical**: Valve opens BEFORE sensor check (sensors need water flow). Pump only runs if initially dry.
**Sequential**: `startSequentialWatering()` waters 5→0, one at a time.
**Auto**: Every loop checks if tray empty (time-based) AND auto-watering enabled.

### Adaptive Learning

Binary search/gradient ascent for optimal watering interval per tray. `emptyToFullDuration = BASE_INTERVAL_MS x intervalMultiplier`.

**Algorithm paths** (processLearningData in WateringSystem.h):
- fill < BASELINE_TOLERANCE(0.70) x baseline → +1.0x (penalty, waters less often)
- fill > baseline → new baseline + 1.0x
- fill ≈ baseline & stable → -0.5x (only decrease path, waters more often)
- fill ≈ baseline & changed → +0.25x (fine tune)

**Key constants**: BASE_INTERVAL_MS=86400000 (24h), AUTO_WATERING_MIN_INTERVAL_MS=86400000 (24h floor), BASELINE_TOLERANCE=0.70, consumption smoothing 70/30 weighted avg.

**Persistence**: Active file `/learning_data_v1.19.12.json`, old file auto-deleted on boot. Reset calibration by swapping filenames in WateringSystem.h:27-30.

### Safety Layers

1. **L1: Overflow Sensor** (GPIO 42, software debounced 5/7 threshold) → emergencyStopAll(), blocks all watering until `reset_overflow`
2. **L2: Water Level Sensor** (GPIO 19, 11s continuation delay) → blocks watering when tank empty, auto-resumes
3. **L3: Per-Valve Timeouts** (config.h) — Valve 0: 40s/45s, Valves 1-5: 25s/30s (normal/emergency)
4. **L4: Two-Tier SM Timeouts** (WateringSystemStateMachine.h) — normal (learning) + emergency (force GPIO)
5. **L5: Global Watchdog** (WateringSystem.h) — `globalSafetyWatchdog()` every loop, bypasses SM
6. **L6: GPIO Reinitialization** — automatic after emergency events, manual via `/reinit_gpio`

### Program Flow

**setup()**: Serial → RTC → LittleFS → wateringSystem.init() → MetricsPusher::init() → NetworkManager init → connectWiFi → setWateringSystemRef (MUST be before setupOta) → bootCountdown (10s /halt poll) → create networkTask on Core 0

**Core 1 loop()**: First loop: schedule + smart boot watering (first boot | overdue) | Every loop: processWateringLoop (SM + auto, blocked if halt) → 10ms delay

**Core 0 networkTask()**: loopOta → NetworkManager::loopWiFi → if WiFi connected: ensureBotCommands → checkTelegramCommands → processPendingNotifications → DebugHelper::loop → MetricsPusher::loop → 100ms delay

**processWateringLoop()**: Watchdog → overflow/water-level checks → auto-water check → process each valve SM → sequential transitions → publish state (2s)

## Cloud.ru VPS Infrastructure (45.151.30.146)

All external services run on a single VPS. SSH: `ssh user1@45.151.30.146`

**Nginx** (:16443, TLS) — single entry point for ESP32. Routes by URL path:
- `/v1/telegram/*` → localhost:18085 (telegram_bot_api_proxy.py)
- `/v1/metrics/*`, `/v1/logs/*` → localhost:18086 (esp32_metrics_proxy.py)
- `/health` → localhost:18085
- Config: `/etc/nginx/conf.d/water-the-flowers-proxy.conf`
- Cert: Let's Encrypt for `water-the-flowers-proxy.aiengineerhelper.com`

**Systemd services** (on host):
- `telegram-bot-api-proxy.service` — Telegram API forwarder (port 18085)
- `esp32-metrics-proxy.service` — metrics/logs receiver (port 18086)
- `xray-client.service` — SOCKS5 proxy for Telegram API (127.0.0.1:1080)

**Docker stack** (`/home/claude-developer/monitoring/docker-compose.yml`):
- Prometheus (:9090) — scrapes metrics from esp32_metrics_proxy, node_exporter, cAdvisor
- Loki (:3100) — receives logs from esp32_metrics_proxy
- Grafana (:3000) — dashboards (admin/admin)
- Tempo (:3200) — tracing (not used by ESP32)
- OTel Collector (:14317/:14318) — not used by ESP32
- node_exporter (:9100), cAdvisor (:8080)

**Prometheus config**: `/home/claude-developer/monitoring/prometheus/prometheus.yml` (mounted read-only into container). Job `esp32_watering` scrapes `host.docker.internal:18086` every 15s.

**Contabo VPS** (31.220.78.216) — runs 3x-ui with VLESS inbound (:8443) for Telegram API routing. Docker: `3x-ui-proxy`.

## Telegram

**Commands**: `/help`, `/start_all`, `/halt`, `/resume`, `/time`, `/settime [YYYY-MM-DD HH:MM:SS]`, `/test_sensors`, `/test_sensor_N`, `/reset_overflow`, `/reinit_gpio`, `/overflow_status`, `/lamp_status`, `/lamp_on`, `/lamp_off`, `/lamp_auto`

**State JSON** (via `/api/status`): pump, sequential_mode, water_level{status,blocked}, overflow{detected,raw_value,trigger_streak}, plant_light{state,mode,schedule_on/off}, valves[]{id, state, phase, rain, timeout, learning{calibrated, auto_watering, baseline_fill_ms, last_fill_ms, empty_duration_ms, total_cycles, water_level_pct, tray_state, time_since_watering_ms, time_until_empty_ms, last_water_level_pct}}

**Proxy** (v1.18.5+): Telegram Bot API proxy on Cloud.ru VPS (45.151.30.146). Proxy script: `tools/telegram_bot_api_proxy.py`, systemd: `telegram-bot-api-proxy.service`, env: `/etc/default/telegram-bot-api-proxy`. Nginx TLS termination on :16443 → localhost:18085.

**SOCKS5 routing** (v1.20.0+): Telegram API is ISP-blocked on Cloud.ru. Traffic routes through a VLESS tunnel: Cloud.ru xray client (SOCKS5 on 127.0.0.1:1080) → Contabo (31.220.78.216) xray/3x-ui VLESS inbound (:8443) → api.telegram.org. Env var `SOCKS5_PROXY=127.0.0.1:1080` in `/etc/default/telegram-bot-api-proxy`. Systemd: `xray-client.service` (auto-restart, enabled on boot). Xray config: `/etc/xray/config.json`, client UUID `cloudru-telegram-proxy` in 3x-ui.

**Telegram not working? Checklist**:
1. SSH to Cloud.ru: `ssh user1@45.151.30.146`
2. Check xray client: `sudo systemctl status xray-client` → restart: `sudo systemctl restart xray-client`
3. Check proxy: `sudo systemctl status telegram-bot-api-proxy` → restart: `sudo systemctl restart telegram-bot-api-proxy`
4. Test SOCKS5: `curl -s --socks5-hostname 127.0.0.1:1080 https://api.telegram.org/` (should return HTML)
5. Test proxy: `curl -s "http://127.0.0.1:18085/v1/telegram/getUpdates?bot_token=<TOKEN>&offset=0&timeout=0" -H "Authorization: Bearer <TOKEN>"` (should return `{"ok":true}`)
6. Check logs: `sudo journalctl -u xray-client -n 20` and `sudo journalctl -u telegram-bot-api-proxy -n 20`
7. If Contabo xray is down: `docker restart 3x-ui-proxy` on Contabo (31.220.78.216)
8. No ESP32 firmware update needed — ESP32 connects to the same nginx endpoint regardless of backend routing

## Monitoring (Prometheus + Loki + Grafana)

**Architecture**: ESP32 pushes metrics JSON (10s active / 60s idle) and debug logs (only when buffer non-empty) to `esp32_metrics_proxy.py` on Cloud.ru via nginx TLS :16443. Proxy stores metrics for Prometheus scraping and forwards logs to Loki.

**Proxy**: `tools/esp32_metrics_proxy.py`, systemd: `esp32-metrics-proxy.service`, env: `/etc/default/esp32-metrics-proxy`, port 18086. Nginx routes `/v1/metrics/` and `/v1/logs/` to this proxy.

**ESP32 firmware**: `MetricsPusher.h` runs on Core 0 in networkTask. Log buffer: 64 entries circular. Push interval: 10s when any valve active, 60s idle. Uses same proxy URL and auth token as Telegram proxy.

**Grafana dashboard**: "ESP32 Watering System" (uid: `esp32-watering`) -- 4 rows: Watering Intervals & Learning, Events & Sensors, System Health, Debug Logs. Dashboard JSON: `tools/grafana-dashboard-esp32.json`.

**Metrics endpoint**: Prometheus scrapes `host.docker.internal:18086/metrics` every 15s (job: `esp32_watering`).

**Log capture points**: State transitions, learning decisions, safety events (overflow/timeout/watchdog), auto-watering triggers, Telegram success/failure, WiFi connect/disconnect. Levels: debug/info/warn/error.

**Monitoring not working? Checklist**:
1. Check proxy: `sudo systemctl status esp32-metrics-proxy` → restart: `sudo systemctl restart esp32-metrics-proxy`
2. Test proxy health: `curl -s http://127.0.0.1:18086/health`
3. Test metrics endpoint: `curl -s http://127.0.0.1:18086/metrics`
4. Check Prometheus targets: `curl -s http://localhost:9090/api/v1/targets | grep esp32`
5. Check Loki: `curl -sG 'http://localhost:3100/loki/api/v1/query' --data-urlencode 'query={job="esp32"}' --data-urlencode 'limit=1'`
6. Check nginx routing: `curl -sk https://localhost:16443/v1/metrics/push` (should return 401, not 404)
7. Check ESP32 serial for `[MetricsPusher]` messages

## Web Interface

**Files**: `data/web/prod/` (index.html, css/style.css, js/app.js)
**API**: `/api/water?valve=N` (1-6), `/api/stop?valve=N|all`, `/api/start_all`, `/api/status`, `/api/lamp?action=on|off|auto`, `/api/reset_calibration?valve=N|all`, `/firmware` (auth: admin/OTA_PASSWORD)
**Note**: API is 1-indexed (1-6), internal code is 0-indexed (0-5)

## Hardware

**Pins**: Pump=4, Valves=5/6/7/15/16/17, Rain Sensors=8/9/10/11/12/13 (INPUT_PULLUP, LOW=wet), Sensor Power=18, Overflow=42 (INPUT_PULLUP, LOW=overflow), Water Level=19 (INPUT_PULLUP, HIGH=water, LOW=empty), Plant Light=41 (active-low relay), LED=48, RTC I2C SDA=14/SCL=3, Battery ADC=1/Ctrl=2

**Sensor Logic (CRITICAL)**: TWO power signals required: (1) Valve pin HIGH, (2) GPIO 18 HIGH. Sequence: valve HIGH → GPIO 18 HIGH → delay 100ms → read → power off. LOW=WET, HIGH=DRY.

**Relay Module**: 6-channel relay with automatic sensor interlock — when rain sensor detects WET, relay opens regardless of GPIO state. No GPIO read-back verification (would show LOW when sensor wet, expected behavior).

## Code Changes Guide

**Watering logic**: StateMachineLogic.h → WateringSystemStateMachine.h → WateringSystem.h. Always run `pio test -e native` first.

**Telegram commands**: main.cpp `checkTelegramCommands()`

**API endpoints**: api_handlers.h (inline), register in main.cpp `registerApiHandlers()`. API 1-indexed → internal 0-indexed. Access `g_wateringSystem_ptr`.

**Web UI**: Edit `data/web/prod/` → `pio run -t buildfs` → `pio run -t uploadfs`

**Learning tuning**: LearningAlgorithm.h (math), WateringSystem.h (policy in processLearningData), ValveController.h (shouldWaterNow trigger)

**Persistence**: Save after watering/reset, load on init(). Swap filenames in WateringSystem.h:27-30 to reset all calibrations.

**Monitoring**: MetricsPusher.h (ESP32 push logic), tools/esp32_metrics_proxy.py (server-side proxy). Add log capture with `MetricsPusher::logInfo/logWarn/logError()`. Metrics JSON format must match proxy's `_build_prometheus_metrics()`. Dashboard: edit `tools/grafana-dashboard-esp32.json` then re-import via Grafana API.

**Config**: config.h (pins, timing), secret.h (credentials, never commit)

## Gotchas

1. Baud 115200, use `--raw` if gibberish
2. API 1-6, internal 0-5
3. Sensors need TWO power signals: valve pin HIGH + GPIO 18 HIGH
4. `setWateringSystemRef()` BEFORE `setupOta()` or API fails
5. Watering continues if WiFi/MQTT down (by design)
6. LittleFS must init before `wateringSystem.init()` (loads persisted data)
7. millis() overflow ~49 days is handled throughout
8. Halt mode is NOT persistent across reboots
9. Overflow flag resets on boot (not persistent)
10. DS3231 is sole time source (no NTP dependency)
11. LED is GPIO 48 (not 2)
12. Plant light relay is active-low (`PLANT_LIGHT_ACTIVE_HIGH = false`)
13. WiFi reconnection must use `WiFi.disconnect(true)` before `WiFi.begin()` — omitting cleanup corrupts the ESP32 WiFi driver
14. Overflow sensor uses software debouncing (5/7 readings must be LOW) to filter electrical noise
15. Water level sensor has 11s continuation delay before blocking (allows active cycles to finish)
16. MQTT outage notifications suppressed for < 10 min (`MQTT_OUTAGE_NOTIFY_THRESHOLD_MS`)
17. MetricsPusher.h MUST be included LAST in main.cpp (depends on WateringSystem.h). Log routing uses `g_metricsLog` function pointer callback (set in `MetricsPusher::init()`) so headers included before MetricsPusher.h can log without compile-time dependency.
18. esp32_metrics_proxy.py must bind to 0.0.0.0 (not 127.0.0.1) — Prometheus runs in Docker and reaches it via `host.docker.internal`

## Testing & Debug

**Native tests**: `pio test -e native` — 36 tests covering state transitions, timeouts, learning math, safety scenarios, plant light schedule, per-valve config
**HW test firmware**: `platformio run -t upload -e esp32-s3-devkitc-1-test`
**Serial menu** (115200): H=help, L=LED, P=pump, 1-6/A/Z=valves, R/M/S=sensors, W/N=water level, T/I=RTC, F=full sequence, X=emergency
**Debug patterns**: ═══ (state), brain (learning), sparkle (baseline), clock (auto), check (success), ERROR (fail)

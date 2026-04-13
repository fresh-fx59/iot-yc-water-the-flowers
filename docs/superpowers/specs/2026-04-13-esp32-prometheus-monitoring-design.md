# ESP32 Prometheus + Loki Monitoring Design

## Goal

Replace guesswork debugging with real device telemetry. ESP32 pushes metrics and debug logs to the Cloud.ru monitoring server, where Prometheus stores metrics, Loki stores logs, and Grafana visualizes both.

## Architecture

```
ESP32 (Core 0, networkTask)
  |
  |-- POST /v1/metrics/push (JSON, 10s active / 60s idle)
  |-- POST /v1/logs/push    (JSON, only when buffer non-empty)
  |
  v
nginx :16443 (existing TLS termination, existing auth)
  |
  |-- /v1/telegram/*  -->  localhost:18085 (telegram_bot_api_proxy.py, unchanged)
  |-- /v1/metrics/*   -->  localhost:18086 (esp32_metrics_proxy.py, NEW)
  |-- /v1/logs/*      -->  localhost:18086 (esp32_metrics_proxy.py, NEW)
  |
  v
esp32_metrics_proxy.py :18086
  |
  |-- GET /metrics         -->  Prometheus scrapes (localhost only)
  |-- POST /v1/metrics/push -->  stores latest gauge values in memory
  |-- POST /v1/logs/push   -->  forwards to Loki HTTP API
  |
  v
Prometheus :9090  (scrapes /metrics every 15s)
Loki :3100        (receives log push via HTTP API)
Grafana :3000     (dashboards for both)
```

**No new ports opened.** ESP32 uses existing :16443. Prometheus scrapes localhost:18086 directly.

## Components

### 1. esp32_metrics_proxy.py (new, tools/)

Standalone Python HTTP server. Same pattern as telegram_bot_api_proxy.py: stdlib only, no pip dependencies, systemd managed.

**Endpoints:**

- `POST /v1/metrics/push` -- receives JSON from ESP32, stores latest values in memory
- `POST /v1/logs/push` -- receives Loki-format JSON from ESP32, forwards to localhost:3100/loki/api/v1/push
- `GET /metrics` -- returns Prometheus text exposition format (scraped by Prometheus)
- `GET /health` -- health check

**Auth:** Bearer token via `METRICS_PROXY_AUTH_TOKEN` env var, checked on all POST endpoints. GET /metrics has no auth (localhost only, Prometheus scrapes it).

**Env vars:**
- `METRICS_PROXY_HOST` (default: 0.0.0.0)
- `METRICS_PROXY_PORT` (default: 18086)
- `METRICS_PROXY_AUTH_TOKEN` (required)
- `LOKI_PUSH_URL` (default: http://localhost:3100/loki/api/v1/push)

### 2. ESP32 Firmware Changes (include/)

**New file: `MetricsPusher.h`** -- header-only, runs on Core 0 in networkTask.

Responsibilities:
- Collect metrics from WateringSystem, WiFi, heap stats
- Maintain a circular log buffer for debug messages
- Push metrics JSON every 10s (any valve active) or 60s (idle)
- Push logs only when buffer is non-empty
- Adaptive interval: checks `isAnyValveActive()` to decide 10s vs 60s

**Integration points:**
- Called from networkTask loop (Core 0), after Telegram processing
- Reads WateringSystem state via `g_wateringSystem_ptr` (same as api_handlers.h)
- Log capture: `MetricsPusher::log(level, message)` called alongside Serial.println in key locations
- Uses same WiFiClient / HTTPClient pattern as TelegramNotifier

**Log buffer:** Circular buffer of 64 entries. Each entry: timestamp (nanosecond epoch from DS3231 RTC + millis sub-second), level (debug/info/warn/error), message string. Flushed on push, dropped if full (oldest first). Timestamp computed as: `epoch_seconds * 1000000000 + (millis() % 1000) * 1000000` to match Loki's nanosecond format.

### 3. Nginx Config Update

Split routing in existing server block on :16443:

```nginx
location /v1/telegram/ {
    proxy_pass http://127.0.0.1:18085;
    # ... existing headers
}

location /v1/metrics/ {
    proxy_pass http://127.0.0.1:18086;
    # ... same headers
}

location /v1/logs/ {
    proxy_pass http://127.0.0.1:18086;
    # ... same headers
}
```

Existing `/health` stays on Telegram proxy. New proxy gets its own `/health` on :18086.

### 4. Prometheus Config Update

Add scrape job:

```yaml
- job_name: "esp32_watering"
  scrape_interval: 15s
  static_configs:
    - targets: ["host.docker.internal:18086"]
      labels:
        alias: "ESP32-Watering-System"
        environment: "production"
```

### 5. Systemd Service

`esp32-metrics-proxy.service` -- same pattern as telegram-bot-api-proxy.service. Restart=always, runs as dedicated unprivileged user.

## Metrics

### System metrics
| Metric | Type | Description |
|--------|------|-------------|
| `esp32_uptime_seconds` | gauge | Device uptime |
| `esp32_free_heap_bytes` | gauge | Free heap memory |
| `esp32_wifi_rssi_dbm` | gauge | WiFi signal strength |
| `esp32_last_push_timestamp` | gauge | Unix epoch of last push |

### Watering metrics
| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `esp32_pump_active` | gauge | | Pump on/off (0/1) |
| `esp32_valve_state` | gauge | valve | Valve open/closed (0/1) |
| `esp32_valve_phase` | gauge | valve | SM phase (0=idle..5=closing) |
| `esp32_valve_rain_detected` | gauge | valve | Rain sensor wet (0/1) |
| `esp32_valve_watering_seconds` | gauge | valve | Current watering elapsed |
| `esp32_valve_water_level_pct` | gauge | valve | Tray water level 0-100 |
| `esp32_valve_calibrated` | gauge | valve | Baseline established (0/1) |
| `esp32_valve_auto_watering` | gauge | valve | Auto-watering enabled (0/1) |
| `esp32_valve_interval_multiplier` | gauge | valve | Learning multiplier |
| `esp32_valve_total_cycles` | counter | valve | Watering cycle count |
| `esp32_valve_time_since_watering_ms` | gauge | valve | ms since last watering |
| `esp32_valve_time_until_empty_ms` | gauge | valve | Predicted ms until dry |
| `esp32_valve_baseline_fill_ms` | gauge | valve | Baseline fill duration |
| `esp32_valve_last_fill_ms` | gauge | valve | Last fill duration |
| `esp32_valve_empty_duration_ms` | gauge | valve | Learned consumption time |

### Safety metrics
| Metric | Type | Description |
|--------|------|-------------|
| `esp32_overflow_detected` | gauge | Overflow sensor triggered (0/1) |
| `esp32_overflow_trigger_streak` | gauge | Consecutive overflow reads |
| `esp32_water_tank_ok` | gauge | Tank has water (0/1) |
| `esp32_plant_light_active` | gauge | Plant light on (0/1) |
| `esp32_telegram_failures_total` | counter | Telegram send failures |

## Logs

### ESP32 log levels
- **error**: Emergency stops, overflow triggers, safety timeouts
- **warn**: Tank empty, Telegram failures, WiFi disconnect
- **info**: Watering start/complete, learning decisions, auto-watering triggers, sensor reads
- **debug**: State transitions, GPIO operations, phase changes

### Push format (Loki native)
```json
{
  "streams": [{
    "stream": {"job": "esp32", "device": "watering-system", "level": "info"},
    "values": [
      ["1681234567000000000", "Valve 2: IDLE -> OPENING_VALVE"],
      ["1681234567100000000", "Rain sensor 2: DRY"]
    ]
  }]
}
```

Logs are grouped by level in the push payload. Proxy forwards as-is to Loki.

### Log capture points
- State machine transitions (StateMachineLogic actions)
- Sensor reads (rain, overflow, water level)
- Learning algorithm decisions (processLearningData)
- Safety events (emergency stop, timeout, GPIO reinit)
- Auto-watering triggers (shouldWaterNow)
- WiFi/Telegram connectivity changes
- Boot sequence milestones

## Grafana Dashboard

One dashboard: "ESP32 Watering System"

### Row 1: Watering Intervals & Learning
- **Watering timeline**: State timeline panel, horizontal bars per valve showing watering events, color by outcome (green=normal, red=timeout, yellow=rain-abort)
- **Interval multiplier**: Time series, line per valve, shows learning algorithm adjustments over days/weeks
- **Water level %**: Time series, line per valve, shows tray drain/fill patterns
- **Time until empty**: Gauge panels per valve, predicted next watering
- **Total cycles**: Stat panels per valve

### Row 2: Watering Events & Sensors
- **Watering duration**: Bar chart per event, easy to spot timeouts vs normal
- **Rain sensor**: State timeline per valve (dry/wet)
- **Pump activity**: State timeline (on/off)

### Row 3: System Health
- **Overflow events**: Stat panel with alert coloring
- **Water tank**: State timeline (ok/empty)
- **WiFi RSSI**: Time series
- **Free heap**: Time series
- **Uptime**: Stat panel
- **Telegram failures**: Counter
- **Plant light**: State timeline with mode annotation

### Row 4: Debug Logs
- **Log viewer**: Loki logs panel, full-text search, level filter
- **Log volume**: Bar chart over time, correlates with watering activity

## Push Interval Logic

```
if any_valve_active:
    push_interval = 10 seconds
else:
    push_interval = 60 seconds

// Logs: push only when buffer non-empty, piggyback on metrics cycle
if log_buffer_count > 0:
    push_logs()
```

No push when WiFi is disconnected. Logs buffer locally and flush on reconnect (up to 64 entries, then oldest dropped).

## Deployment Order

1. Create and deploy `esp32_metrics_proxy.py` + systemd service on Cloud.ru
2. Update nginx config to split routing
3. Add Prometheus scrape job for esp32_watering
4. Verify proxy receives test pushes (curl)
5. Update ESP32 firmware: add MetricsPusher.h, integrate into networkTask
6. Flash ESP32, verify metrics flow
7. Provision Grafana dashboard
8. Iterate on dashboard based on user feedback

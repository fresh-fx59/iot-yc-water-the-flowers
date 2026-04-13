# ESP32 Prometheus + Loki Monitoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Push ESP32 metrics and debug logs to Prometheus and Loki via a new Python proxy on Cloud.ru, visualized in Grafana.

**Architecture:** ESP32 pushes JSON metrics (10s/60s adaptive) and log batches (only when non-empty) to a new `esp32_metrics_proxy.py` on Cloud.ru via existing nginx TLS on :16443. The proxy stores latest metrics for Prometheus scraping and forwards logs to Loki. A Grafana dashboard visualizes watering intervals, learning data, sensor events, system health, and debug logs.

**Tech Stack:** Python 3 stdlib (proxy), ESP32 Arduino C++ (firmware), Prometheus, Loki, Grafana, nginx, systemd

**Spec:** `docs/superpowers/specs/2026-04-13-esp32-prometheus-monitoring-design.md`

---

## File Structure

### New files
| File | Responsibility |
|------|---------------|
| `tools/esp32_metrics_proxy.py` | Standalone HTTP server: receives ESP32 metrics+logs, exposes /metrics for Prometheus, forwards logs to Loki |
| `include/MetricsPusher.h` | ESP32 header: collects metrics, buffers logs, pushes to proxy on Core 0 |
| `tools/grafana-dashboard-esp32.json` | Grafana dashboard definition (importable) |

### Modified files
| File | Changes |
|------|---------|
| `include/config.h` | Add METRICS_PROXY_BASE_URL, push interval constants, log buffer size |
| `src/main.cpp` | Add `#include <MetricsPusher.h>`, call `MetricsPusher::loop()` in networkTask, add log calls in boot sequence |
| `include/WateringSystem.h` | Add `MetricsPusher::log()` calls at key watering/learning/safety points |
| `include/WateringSystemStateMachine.h` | Add `MetricsPusher::log()` calls at state transitions |
| `include/TelegramNotifier.h` | Add `MetricsPusher::log()` on send success/failure |
| `include/NetworkManager.h` | Add `MetricsPusher::log()` on WiFi connect/disconnect |

### Remote files (Cloud.ru VPS 45.151.30.146)
| File | Changes |
|------|---------|
| `/opt/iot-yc-water-the-flowers/tools/esp32_metrics_proxy.py` | Deploy new proxy script |
| `/etc/systemd/system/esp32-metrics-proxy.service` | New systemd service |
| `/etc/default/esp32-metrics-proxy` | Env vars for metrics proxy |
| nginx config for :16443 | Split routing: `/v1/telegram/` → :18085, `/v1/metrics/` + `/v1/logs/` → :18086 |
| `/home/claude-developer/monitoring/prometheus/prometheus.yml` | Add esp32_watering scrape job |

---

## Task 1: Create esp32_metrics_proxy.py

**Files:**
- Create: `tools/esp32_metrics_proxy.py`

- [ ] **Step 1: Create the proxy script**

```python
#!/usr/bin/env python3
"""
ESP32 Metrics & Logs Proxy for Prometheus + Loki.

Exposes:
  POST /v1/metrics/push   — ESP32 pushes JSON metrics, stored in memory
  POST /v1/logs/push      — ESP32 pushes Loki-format logs, forwarded to Loki
  GET  /metrics            — Prometheus scrapes latest metrics (text exposition)
  GET  /health             — Health check

Auth:
  METRICS_PROXY_AUTH_TOKEN=<token>
  Header: Authorization: Bearer <token>
"""

from __future__ import annotations

import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse
from urllib.request import Request, urlopen

HOST = os.getenv("METRICS_PROXY_HOST", "0.0.0.0")
PORT = int(os.getenv("METRICS_PROXY_PORT", "18086"))
AUTH_TOKEN = os.getenv("METRICS_PROXY_AUTH_TOKEN", "").strip()
LOKI_PUSH_URL = os.getenv("LOKI_PUSH_URL", "http://localhost:3100/loki/api/v1/push")

# Thread-safe storage for latest metrics
_metrics_lock = threading.Lock()
_metrics: dict = {}
_last_push_time: float = 0.0


def _require_auth(handler: BaseHTTPRequestHandler) -> bool:
    if not AUTH_TOKEN:
        return True
    auth = (handler.headers.get("Authorization") or "").strip()
    return auth == f"Bearer {AUTH_TOKEN}"


def _json_response(handler: BaseHTTPRequestHandler, code: int, payload: dict) -> None:
    raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


def _text_response(handler: BaseHTTPRequestHandler, code: int, text: str) -> None:
    raw = text.encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "text/plain; charset=utf-8")
    handler.send_header("Content-Length", str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


def _build_prometheus_metrics() -> str:
    """Convert stored JSON metrics to Prometheus text exposition format."""
    with _metrics_lock:
        m = _metrics.copy()
        push_ts = _last_push_time

    if not m:
        return "# No metrics received yet\n"

    lines: list[str] = []

    def gauge(name: str, help_text: str, value, labels: str = "") -> None:
        lines.append(f"# HELP {name} {help_text}")
        lines.append(f"# TYPE {name} gauge")
        lbl = f"{{{labels}}}" if labels else ""
        lines.append(f"{name}{lbl} {value}")

    def gauge_only(name: str, value, labels: str = "") -> None:
        lbl = f"{{{labels}}}" if labels else ""
        lines.append(f"{name}{lbl} {value}")

    # System metrics
    gauge("esp32_uptime_seconds", "Device uptime in seconds", m.get("uptime_s", 0))
    gauge("esp32_free_heap_bytes", "Free heap memory in bytes", m.get("free_heap", 0))
    gauge("esp32_wifi_rssi_dbm", "WiFi signal strength in dBm", m.get("wifi_rssi", 0))
    gauge("esp32_last_push_timestamp", "Unix epoch of last metrics push", f"{push_ts:.0f}")

    # Pump
    gauge("esp32_pump_active", "Pump on (1) or off (0)", m.get("pump", 0))

    # Safety
    gauge("esp32_overflow_detected", "Overflow sensor triggered (1/0)", m.get("overflow", 0))
    gauge("esp32_overflow_trigger_streak", "Consecutive overflow reads", m.get("overflow_streak", 0))
    gauge("esp32_water_tank_ok", "Water tank has water (1/0)", m.get("water_tank_ok", 1))
    gauge("esp32_plant_light_active", "Plant light on (1/0)", m.get("plant_light", 0))
    gauge("esp32_telegram_failures_total", "Total Telegram send failures", m.get("telegram_failures", 0))

    # Per-valve metrics
    valves = m.get("valves", [])
    if valves:
        lines.append("# HELP esp32_valve_state Valve open (1) or closed (0)")
        lines.append("# TYPE esp32_valve_state gauge")
        for v in valves:
            vid = v.get("id", 0)
            gauge_only("esp32_valve_state", v.get("state", 0), f'valve="{vid}"')

        lines.append("# HELP esp32_valve_phase State machine phase (0=idle,1=opening,2=stabilization,3=checking_rain,4=watering,5=closing)")
        lines.append("# TYPE esp32_valve_phase gauge")
        for v in valves:
            gauge_only("esp32_valve_phase", v.get("phase", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_rain_detected Rain sensor wet (1/0)")
        lines.append("# TYPE esp32_valve_rain_detected gauge")
        for v in valves:
            gauge_only("esp32_valve_rain_detected", v.get("rain", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_watering_seconds Current watering elapsed seconds")
        lines.append("# TYPE esp32_valve_watering_seconds gauge")
        for v in valves:
            gauge_only("esp32_valve_watering_seconds", v.get("watering_s", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_water_level_pct Tray water level 0-100")
        lines.append("# TYPE esp32_valve_water_level_pct gauge")
        for v in valves:
            gauge_only("esp32_valve_water_level_pct", v.get("water_level_pct", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_calibrated Baseline established (1/0)")
        lines.append("# TYPE esp32_valve_calibrated gauge")
        for v in valves:
            gauge_only("esp32_valve_calibrated", v.get("calibrated", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_auto_watering Auto-watering enabled (1/0)")
        lines.append("# TYPE esp32_valve_auto_watering gauge")
        for v in valves:
            gauge_only("esp32_valve_auto_watering", v.get("auto_watering", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_interval_multiplier Learning interval multiplier")
        lines.append("# TYPE esp32_valve_interval_multiplier gauge")
        for v in valves:
            gauge_only("esp32_valve_interval_multiplier", v.get("interval_mult", 1.0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_total_cycles Total watering cycles")
        lines.append("# TYPE esp32_valve_total_cycles gauge")
        for v in valves:
            gauge_only("esp32_valve_total_cycles", v.get("total_cycles", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_time_since_watering_ms Milliseconds since last watering")
        lines.append("# TYPE esp32_valve_time_since_watering_ms gauge")
        for v in valves:
            gauge_only("esp32_valve_time_since_watering_ms", v.get("time_since_ms", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_time_until_empty_ms Predicted ms until tray empty")
        lines.append("# TYPE esp32_valve_time_until_empty_ms gauge")
        for v in valves:
            gauge_only("esp32_valve_time_until_empty_ms", v.get("time_until_empty_ms", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_baseline_fill_ms Baseline fill duration ms")
        lines.append("# TYPE esp32_valve_baseline_fill_ms gauge")
        for v in valves:
            gauge_only("esp32_valve_baseline_fill_ms", v.get("baseline_fill_ms", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_last_fill_ms Last fill duration ms")
        lines.append("# TYPE esp32_valve_last_fill_ms gauge")
        for v in valves:
            gauge_only("esp32_valve_last_fill_ms", v.get("last_fill_ms", 0), f'valve="{v.get("id", 0)}"')

        lines.append("# HELP esp32_valve_empty_duration_ms Learned consumption time ms")
        lines.append("# TYPE esp32_valve_empty_duration_ms gauge")
        for v in valves:
            gauge_only("esp32_valve_empty_duration_ms", v.get("empty_duration_ms", 0), f'valve="{v.get("id", 0)}"')

    lines.append("")
    return "\n".join(lines)


def _forward_to_loki(body: bytes) -> tuple[int, str]:
    """Forward log payload to Loki HTTP API."""
    req = Request(
        LOKI_PUSH_URL,
        method="POST",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urlopen(req, timeout=5) as resp:
            return resp.status, resp.read().decode("utf-8", errors="replace")
    except Exception as exc:
        return 502, str(exc)


class Handler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:
        parsed = urlparse(self.path)

        if parsed.path == "/v1/metrics/push":
            if not _require_auth(self):
                _json_response(self, 401, {"ok": False, "error": "Unauthorized"})
                return
            self._handle_metrics_push()

        elif parsed.path == "/v1/logs/push":
            if not _require_auth(self):
                _json_response(self, 401, {"ok": False, "error": "Unauthorized"})
                return
            self._handle_logs_push()

        else:
            _json_response(self, 404, {"ok": False, "error": "Not found"})

    def do_GET(self) -> None:
        parsed = urlparse(self.path)

        if parsed.path == "/health":
            _json_response(self, 200, {"status": "ok"})

        elif parsed.path == "/metrics":
            text = _build_prometheus_metrics()
            _text_response(self, 200, text)

        else:
            _json_response(self, 404, {"ok": False, "error": "Not found"})

    def _handle_metrics_push(self) -> None:
        global _metrics, _last_push_time
        content_length = int(self.headers.get("Content-Length", "0"))
        if content_length == 0:
            _json_response(self, 400, {"ok": False, "error": "Empty body"})
            return

        try:
            body = self.rfile.read(content_length)
            data = json.loads(body)
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            _json_response(self, 400, {"ok": False, "error": f"Invalid JSON: {exc}"})
            return

        with _metrics_lock:
            _metrics = data
            _last_push_time = time.time()

        _json_response(self, 200, {"ok": True})

    def _handle_logs_push(self) -> None:
        content_length = int(self.headers.get("Content-Length", "0"))
        if content_length == 0:
            _json_response(self, 400, {"ok": False, "error": "Empty body"})
            return

        body = self.rfile.read(content_length)

        # Validate JSON before forwarding
        try:
            json.loads(body)
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            _json_response(self, 400, {"ok": False, "error": f"Invalid JSON: {exc}"})
            return

        status, resp_body = _forward_to_loki(body)
        if 200 <= status < 300:
            _json_response(self, 200, {"ok": True})
        else:
            _json_response(self, 502, {"ok": False, "error": f"Loki error {status}: {resp_body}"})

    def log_message(self, fmt: str, *args) -> None:
        print(f"[esp32-metrics-proxy] {self.address_string()} - {fmt % args}")


def main() -> None:
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"ESP32 metrics proxy listening on {HOST}:{PORT}")
    print(f"  Loki push URL: {LOKI_PUSH_URL}")
    print(f"  Auth: {'enabled' if AUTH_TOKEN else 'disabled'}")
    server.serve_forever()


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Test locally**

Run: `python3 tools/esp32_metrics_proxy.py &`

Test health:
```bash
curl -s http://localhost:18086/health
```
Expected: `{"status": "ok"}`

Test metrics push:
```bash
curl -s -X POST http://localhost:18086/v1/metrics/push \
  -H "Content-Type: application/json" \
  -d '{"uptime_s":123,"free_heap":200000,"wifi_rssi":-45,"pump":0,"overflow":0,"overflow_streak":0,"water_tank_ok":1,"plant_light":0,"telegram_failures":0,"valves":[{"id":0,"state":0,"phase":0,"rain":0,"watering_s":0,"water_level_pct":75,"calibrated":1,"auto_watering":1,"interval_mult":1.5,"total_cycles":12,"time_since_ms":3600000,"time_until_empty_ms":82800000,"baseline_fill_ms":15000,"last_fill_ms":14200,"empty_duration_ms":86400000}]}'
```
Expected: `{"ok": true}`

Test Prometheus scrape:
```bash
curl -s http://localhost:18086/metrics | head -20
```
Expected: Prometheus text format with `esp32_uptime_seconds 123` etc.

Kill test: `kill %1`

- [ ] **Step 3: Commit**

```bash
git add tools/esp32_metrics_proxy.py
git commit -m "v1.20.1: add ESP32 metrics proxy for Prometheus + Loki"
```

---

## Task 2: Deploy metrics proxy to Cloud.ru

**Files:**
- Remote create: `/etc/systemd/system/esp32-metrics-proxy.service`
- Remote create: `/etc/default/esp32-metrics-proxy`
- Remote copy: `/opt/iot-yc-water-the-flowers/tools/esp32_metrics_proxy.py`

- [ ] **Step 1: Copy proxy script to server**

```bash
scp tools/esp32_metrics_proxy.py user1@45.151.30.146:/tmp/esp32_metrics_proxy.py
ssh user1@45.151.30.146 "sudo cp /tmp/esp32_metrics_proxy.py /opt/iot-yc-water-the-flowers/tools/esp32_metrics_proxy.py && sudo chmod 644 /opt/iot-yc-water-the-flowers/tools/esp32_metrics_proxy.py"
```

- [ ] **Step 2: Create env file**

```bash
ssh user1@45.151.30.146 "sudo tee /etc/default/esp32-metrics-proxy > /dev/null << 'ENVEOF'
METRICS_PROXY_HOST=127.0.0.1
METRICS_PROXY_PORT=18086
METRICS_PROXY_AUTH_TOKEN=774b44668b94a589c3792e6069f0df1fe75d1c927d4332075b7e58bedf2f4611
LOKI_PUSH_URL=http://localhost:3100/loki/api/v1/push
ENVEOF"
```

Note: Reuses same auth token as Telegram proxy for simplicity (ESP32 already has it configured).

- [ ] **Step 3: Create systemd service**

```bash
ssh user1@45.151.30.146 "sudo tee /etc/systemd/system/esp32-metrics-proxy.service > /dev/null << 'SVCEOF'
[Unit]
Description=ESP32 Metrics Proxy for Prometheus + Loki
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=user1
Group=user1
WorkingDirectory=/opt/iot-yc-water-the-flowers
EnvironmentFile=/etc/default/esp32-metrics-proxy
ExecStart=/usr/bin/python3 /opt/iot-yc-water-the-flowers/tools/esp32_metrics_proxy.py
Restart=always
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true
ReadWritePaths=/var/log

[Install]
WantedBy=multi-user.target
SVCEOF"
```

- [ ] **Step 4: Enable and start the service**

```bash
ssh user1@45.151.30.146 "sudo systemctl daemon-reload && sudo systemctl enable esp32-metrics-proxy && sudo systemctl start esp32-metrics-proxy && sudo systemctl status esp32-metrics-proxy"
```

Expected: Active (running)

- [ ] **Step 5: Verify health endpoint**

```bash
ssh user1@45.151.30.146 "curl -s http://127.0.0.1:18086/health"
```

Expected: `{"status": "ok"}`

---

## Task 3: Update nginx routing

**Files:**
- Remote modify: nginx config for :16443

- [ ] **Step 1: Find and read current config file**

```bash
ssh user1@45.151.30.146 "sudo grep -rl '16443' /etc/nginx/sites-enabled/ /etc/nginx/conf.d/ 2>/dev/null"
```

This returns the config file path. Read it to get the exact filename.

- [ ] **Step 2: Update nginx config to split routing**

Replace the single `location /` block with path-based routing. The full server block becomes:

```nginx
server {
    listen 16443 ssl http2;
    server_name water-the-flowers-proxy.aiengineerhelper.com;

    ssl_certificate /etc/letsencrypt/live/water-the-flowers-proxy.aiengineerhelper.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/water-the-flowers-proxy.aiengineerhelper.com/privkey.pem;
    include /etc/letsencrypt/options-ssl-nginx.conf;
    ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;

    # Telegram proxy (existing)
    location /v1/telegram/ {
        proxy_pass http://127.0.0.1:18085;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";
    }

    # Telegram proxy health check (existing)
    location = /health {
        proxy_pass http://127.0.0.1:18085;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header Connection "";
    }

    # ESP32 metrics proxy (new)
    location /v1/metrics/ {
        proxy_pass http://127.0.0.1:18086;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";
    }

    # ESP32 logs proxy (new)
    location /v1/logs/ {
        proxy_pass http://127.0.0.1:18086;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";
    }
}
```

Write this using `sudo tee` via SSH, replacing the existing file.

- [ ] **Step 3: Test and reload nginx**

```bash
ssh user1@45.151.30.146 "sudo nginx -t && sudo systemctl reload nginx"
```

Expected: `nginx: configuration file /etc/nginx/nginx.conf syntax is ok`

- [ ] **Step 4: Verify routing works for both backends**

Test Telegram proxy still works:
```bash
ssh user1@45.151.30.146 "curl -s -k https://localhost:16443/health"
```
Expected: `{"status": "ok"}`

Test metrics proxy through nginx:
```bash
ssh user1@45.151.30.146 "curl -s -k -X POST https://localhost:16443/v1/metrics/push -H 'Authorization: Bearer 774b44668b94a589c3792e6069f0df1fe75d1c927d4332075b7e58bedf2f4611' -H 'Content-Type: application/json' -d '{\"uptime_s\":1}'"
```
Expected: `{"ok": true}`

---

## Task 4: Add Prometheus scrape job

**Files:**
- Remote modify: `/home/claude-developer/monitoring/prometheus/prometheus.yml`

- [ ] **Step 1: Add esp32_watering scrape job**

Append to the `scrape_configs` section of prometheus.yml:

```yaml
  - job_name: "esp32_watering"
    scrape_interval: 15s
    static_configs:
      - targets: ["host.docker.internal:18086"]
        labels:
          alias: "ESP32-Watering-System"
          environment: "production"
```

Use `sudo tee -a` or read+modify+write via SSH.

- [ ] **Step 2: Reload Prometheus**

```bash
ssh user1@45.151.30.146 "sudo docker exec prometheus kill -SIGHUP 1"
```

Or restart:
```bash
ssh user1@45.151.30.146 "sudo docker restart prometheus"
```

- [ ] **Step 3: Verify scrape target appears**

```bash
ssh user1@45.151.30.146 "curl -s http://localhost:9090/api/v1/targets | python3 -m json.tool | grep -A5 esp32_watering"
```

Expected: Target with state "up" (or "unknown" if no metrics pushed yet)

---

## Task 5: Verify server-side setup end-to-end

- [ ] **Step 1: Push test metrics through nginx TLS**

```bash
ssh user1@45.151.30.146 "curl -s -k -X POST https://localhost:16443/v1/metrics/push \
  -H 'Authorization: Bearer 774b44668b94a589c3792e6069f0df1fe75d1c927d4332075b7e58bedf2f4611' \
  -H 'Content-Type: application/json' \
  -d '{\"uptime_s\":42,\"free_heap\":180000,\"wifi_rssi\":-55,\"pump\":0,\"overflow\":0,\"overflow_streak\":0,\"water_tank_ok\":1,\"plant_light\":1,\"telegram_failures\":2,\"valves\":[{\"id\":0,\"state\":0,\"phase\":0,\"rain\":0,\"watering_s\":0,\"water_level_pct\":80,\"calibrated\":1,\"auto_watering\":1,\"interval_mult\":1.5,\"total_cycles\":10,\"time_since_ms\":7200000,\"time_until_empty_ms\":79200000,\"baseline_fill_ms\":15000,\"last_fill_ms\":14500,\"empty_duration_ms\":86400000}]}'"
```
Expected: `{"ok": true}`

- [ ] **Step 2: Verify Prometheus can scrape**

```bash
ssh user1@45.151.30.146 "curl -s http://localhost:9090/api/v1/query?query=esp32_uptime_seconds | python3 -m json.tool"
```
Expected: JSON with `"value"` containing `"42"`

- [ ] **Step 3: Push test log to Loki**

```bash
ssh user1@45.151.30.146 "curl -s -k -X POST https://localhost:16443/v1/logs/push \
  -H 'Authorization: Bearer 774b44668b94a589c3792e6069f0df1fe75d1c927d4332075b7e58bedf2f4611' \
  -H 'Content-Type: application/json' \
  -d '{\"streams\":[{\"stream\":{\"job\":\"esp32\",\"device\":\"watering-system\",\"level\":\"info\"},\"values\":[[\"$(date +%s)000000000\",\"Test log from setup verification\"]]}]}'"
```
Expected: `{"ok": true}`

- [ ] **Step 4: Verify log in Loki**

```bash
ssh user1@45.151.30.146 "curl -s 'http://localhost:3100/loki/api/v1/query_range?query={job=\"esp32\"}&limit=5' | python3 -m json.tool | head -20"
```
Expected: JSON with the test log entry

---

## Task 6: Add metrics constants to config.h

**Files:**
- Modify: `include/config.h`

- [ ] **Step 1: Add metrics push constants**

Add after the WiFi configuration section (after line ~183):

```cpp
// ============================================
// Metrics Push Configuration
// ============================================
const unsigned long METRICS_PUSH_INTERVAL_ACTIVE_MS = 10000;  // 10s when watering
const unsigned long METRICS_PUSH_INTERVAL_IDLE_MS = 60000;    // 60s when idle
const int METRICS_LOG_BUFFER_SIZE = 64;                        // Circular log buffer entries
const unsigned long METRICS_HTTP_TIMEOUT_MS = 4000;            // HTTP timeout for proxy
```

- [ ] **Step 2: Add METRICS_PROXY_BASE_URL define**

Add after the TELEGRAM_PROXY_AUTH_TOKEN define (after line ~196):

```cpp
// Metrics proxy uses same base URL and auth as Telegram proxy.
// Endpoints: /v1/metrics/push, /v1/logs/push
#ifndef METRICS_PROXY_BASE_URL
#define METRICS_PROXY_BASE_URL TELEGRAM_PROXY_BASE_URL
#endif
```

- [ ] **Step 3: Verify build**

```bash
pio run -e esp32-s3-devkitc-1 2>&1 | tail -5
```
Expected: `SUCCESS`

- [ ] **Step 4: Commit**

```bash
git add include/config.h
git commit -m "v1.20.1: add metrics push configuration constants"
```

---

## Task 7: Create MetricsPusher.h

**Files:**
- Create: `include/MetricsPusher.h`

- [ ] **Step 1: Create the header file**

```cpp
#ifndef METRICS_PUSHER_H
#define METRICS_PUSHER_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "config.h"
#include "secret.h"

// Forward declarations
class WateringSystem;
extern WateringSystem* g_wateringSystem_ptr;

// ============================================
// Log Entry Structure
// ============================================
struct MetricsLogEntry {
    String message;
    String level;        // "debug", "info", "warn", "error"
    unsigned long epochSeconds;
    unsigned long millisFraction;
    bool valid;
};

// ============================================
// MetricsPusher - Collects and pushes ESP32 telemetry
// ============================================
class MetricsPusher {
private:
    static MetricsLogEntry logBuffer[METRICS_LOG_BUFFER_SIZE];
    static int logHead;
    static int logTail;
    static int logCount;

    static unsigned long lastPushTime;
    static unsigned long lastLogPushTime;
    static bool initialized;
    static int telegramFailureCount;

    // Get current epoch seconds from system time (DS3231 synced)
    static unsigned long getEpochSeconds() {
        time_t now;
        time(&now);
        return (unsigned long)now;
    }

    // Build nanosecond timestamp string for Loki
    static String buildNanoTimestamp(unsigned long epochSec, unsigned long millisFrac) {
        // Format: epoch_seconds + milliseconds + padding = nanoseconds
        char ts[21];
        snprintf(ts, sizeof(ts), "%lu%03lu000000", epochSec, millisFrac % 1000);
        return String(ts);
    }

    // Escape string for JSON
    static String jsonEscape(const String& input) {
        String output;
        output.reserve(input.length() + 16);
        for (unsigned int i = 0; i < input.length(); i++) {
            char c = input.charAt(i);
            switch (c) {
                case '"': output += "\\\""; break;
                case '\\': output += "\\\\"; break;
                case '\n': output += "\\n"; break;
                case '\r': output += "\\r"; break;
                case '\t': output += "\\t"; break;
                default: output += c; break;
            }
        }
        return output;
    }

    static String getProxyBaseUrl() {
        String base = String(METRICS_PROXY_BASE_URL);
        while (base.endsWith("/")) {
            base.remove(base.length() - 1);
        }
        return base;
    }

    static bool isProxyConfigured() {
        return String(METRICS_PROXY_BASE_URL).length() > 0;
    }

    static void applyAuthHeader(HTTPClient& http) {
        String token = String(TELEGRAM_PROXY_AUTH_TOKEN);
        token.trim();
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }
    }

    static bool beginHttpClient(HTTPClient& http, const String& url,
                                WiFiClientSecure& secureClient, WiFiClient& plainClient) {
        if (url.startsWith("https://")) {
            secureClient.setInsecure();
            return http.begin(secureClient, url);
        }
        return http.begin(plainClient, url);
    }

    // Push metrics JSON to proxy
    static bool pushMetrics() {
        if (!isProxyConfigured() || !WiFi.isConnected() || !g_wateringSystem_ptr) {
            return false;
        }

        String json = buildMetricsJson();
        if (json.length() == 0) return false;

        HTTPClient http;
        WiFiClientSecure secureClient;
        WiFiClient plainClient;

        String url = getProxyBaseUrl() + "/v1/metrics/push";
        if (!beginHttpClient(http, url, secureClient, plainClient)) {
            return false;
        }

        http.addHeader("Content-Type", "application/json");
        applyAuthHeader(http);
        http.setTimeout(METRICS_HTTP_TIMEOUT_MS);

        int httpCode = http.POST(json);
        bool success = (httpCode == 200);
        http.end();

        if (!success) {
            Serial.printf("[MetricsPusher] Push failed, HTTP %d\n", httpCode);
        }
        return success;
    }

    // Push buffered logs to proxy (Loki format)
    static bool pushLogs() {
        if (!isProxyConfigured() || !WiFi.isConnected() || logCount == 0) {
            return false;
        }

        String json = buildLogsJson();
        if (json.length() == 0) return false;

        HTTPClient http;
        WiFiClientSecure secureClient;
        WiFiClient plainClient;

        String url = getProxyBaseUrl() + "/v1/logs/push";
        if (!beginHttpClient(http, url, secureClient, plainClient)) {
            return false;
        }

        http.addHeader("Content-Type", "application/json");
        applyAuthHeader(http);
        http.setTimeout(METRICS_HTTP_TIMEOUT_MS);

        int httpCode = http.POST(json);
        bool success = (httpCode == 200);
        http.end();

        if (success) {
            // Clear all buffered logs on success
            clearLogBuffer();
        } else {
            Serial.printf("[MetricsPusher] Log push failed, HTTP %d\n", httpCode);
        }
        return success;
    }

    static void clearLogBuffer() {
        for (int i = 0; i < METRICS_LOG_BUFFER_SIZE; i++) {
            logBuffer[i].valid = false;
            logBuffer[i].message = "";
        }
        logHead = 0;
        logTail = 0;
        logCount = 0;
    }

    // Build metrics JSON from WateringSystem state
    static String buildMetricsJson();

    // Build Loki-format JSON from log buffer
    static String buildLogsJson();

public:
    // Initialize (call once in setup)
    static void init() {
        clearLogBuffer();
        lastPushTime = 0;
        lastLogPushTime = 0;
        initialized = true;
    }

    // Buffer a log entry
    static void log(const String& level, const String& message) {
        if (!initialized) return;

        // Store in circular buffer
        logBuffer[logHead].message = message;
        logBuffer[logHead].level = level;
        logBuffer[logHead].epochSeconds = getEpochSeconds();
        logBuffer[logHead].millisFraction = millis() % 1000;
        logBuffer[logHead].valid = true;

        logHead = (logHead + 1) % METRICS_LOG_BUFFER_SIZE;

        if (logCount >= METRICS_LOG_BUFFER_SIZE) {
            // Buffer full - overwrite oldest
            logTail = (logTail + 1) % METRICS_LOG_BUFFER_SIZE;
        } else {
            logCount++;
        }
    }

    // Convenience log methods
    static void logDebug(const String& msg) { log("debug", msg); }
    static void logInfo(const String& msg)  { log("info", msg); }
    static void logWarn(const String& msg)  { log("warn", msg); }
    static void logError(const String& msg) { log("error", msg); }

    // Track Telegram failures (called from TelegramNotifier)
    static void recordTelegramFailure() { telegramFailureCount++; }

    // Main loop - call from networkTask on Core 0
    static void loop() {
        if (!initialized || !isProxyConfigured() || !WiFi.isConnected()) {
            return;
        }

        unsigned long currentTime = millis();
        unsigned long pushInterval = isAnyValveActive()
            ? METRICS_PUSH_INTERVAL_ACTIVE_MS
            : METRICS_PUSH_INTERVAL_IDLE_MS;

        // Handle millis() overflow
        if (currentTime - lastPushTime >= pushInterval || lastPushTime == 0) {
            pushMetrics();
            lastPushTime = currentTime;

            // Push logs on same cycle (only if buffer non-empty)
            if (logCount > 0) {
                pushLogs();
            }
        }
    }

    // Check if any valve is currently active
    static bool isAnyValveActive();
};

// ============================================
// Static Member Initialization
// ============================================
MetricsLogEntry MetricsPusher::logBuffer[METRICS_LOG_BUFFER_SIZE];
int MetricsPusher::logHead = 0;
int MetricsPusher::logTail = 0;
int MetricsPusher::logCount = 0;
unsigned long MetricsPusher::lastPushTime = 0;
unsigned long MetricsPusher::lastLogPushTime = 0;
bool MetricsPusher::initialized = false;
int MetricsPusher::telegramFailureCount = 0;

// ============================================
// Implementation (needs WateringSystem access)
// ============================================
#include "WateringSystem.h"

inline bool MetricsPusher::isAnyValveActive() {
    if (!g_wateringSystem_ptr) return false;
    for (int i = 0; i < NUM_VALVES; i++) {
        if (g_wateringSystem_ptr->valves[i]->phase != PHASE_IDLE) {
            return true;
        }
    }
    return false;
}

inline String MetricsPusher::buildMetricsJson() {
    if (!g_wateringSystem_ptr) return "";

    WateringSystem* ws = g_wateringSystem_ptr;
    unsigned long currentTime = millis();

    String j = "{";
    j += "\"uptime_s\":" + String(currentTime / 1000);
    j += ",\"free_heap\":" + String(ESP.getFreeHeap());
    j += ",\"wifi_rssi\":" + String(WiFi.RSSI());
    j += ",\"pump\":" + String(ws->pumpState == PUMP_ON ? 1 : 0);
    j += ",\"overflow\":" + String(ws->overflowDetected ? 1 : 0);
    j += ",\"overflow_streak\":" + String(ws->overflowDetectionStreak);
    j += ",\"water_tank_ok\":" + String(ws->waterLevelLow ? 0 : 1);
    j += ",\"plant_light\":" + String(ws->plantLight.isOn() ? 1 : 0);
    j += ",\"telegram_failures\":" + String(telegramFailureCount);

    j += ",\"valves\":[";
    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* v = ws->valves[i];
        if (i > 0) j += ",";
        j += "{";
        j += "\"id\":" + String(i);
        j += ",\"state\":" + String(v->state == VALVE_OPEN ? 1 : 0);
        j += ",\"phase\":" + String((int)v->phase);
        j += ",\"rain\":" + String(v->rainDetected ? 1 : 0);

        // Watering elapsed
        unsigned long wateringSec = 0;
        if (v->phase == PHASE_WATERING && v->wateringStartTime > 0) {
            wateringSec = (currentTime - v->wateringStartTime) / 1000;
        }
        j += ",\"watering_s\":" + String(wateringSec);

        // Learning data
        if (v->isCalibrated && v->emptyToFullDuration > 0 && v->lastWateringCompleteTime > 0) {
            float waterLevel = calculateCurrentWaterLevel(v, currentTime);
            unsigned long timeSince = currentTime - v->lastWateringCompleteTime;
            unsigned long timeUntilEmpty = 0;
            if (waterLevel > 0 && timeSince < v->emptyToFullDuration) {
                timeUntilEmpty = v->emptyToFullDuration - timeSince;
            }
            j += ",\"water_level_pct\":" + String((int)waterLevel);
            j += ",\"time_since_ms\":" + String(timeSince);
            j += ",\"time_until_empty_ms\":" + String(timeUntilEmpty);
        } else {
            j += ",\"water_level_pct\":0";
            j += ",\"time_since_ms\":0";
            j += ",\"time_until_empty_ms\":0";
        }

        j += ",\"calibrated\":" + String(v->isCalibrated ? 1 : 0);
        j += ",\"auto_watering\":" + String(v->autoWateringEnabled ? 1 : 0);
        j += ",\"interval_mult\":" + String(v->intervalMultiplier, 2);
        j += ",\"total_cycles\":" + String(v->totalWateringCycles);
        j += ",\"baseline_fill_ms\":" + String(v->baselineFillDuration);
        j += ",\"last_fill_ms\":" + String(v->lastFillDuration);
        j += ",\"empty_duration_ms\":" + String(v->emptyToFullDuration);
        j += "}";
    }
    j += "]}";
    return j;
}

inline String MetricsPusher::buildLogsJson() {
    if (logCount == 0) return "";

    // Group logs by level for Loki streams
    // Collect all entries first
    struct LogGroup {
        String level;
        String values; // JSON array entries
        int count;
    };
    LogGroup groups[4] = {
        {"debug", "", 0},
        {"info", "", 0},
        {"warn", "", 0},
        {"error", "", 0}
    };

    int idx = logTail;
    for (int i = 0; i < logCount; i++) {
        MetricsLogEntry& entry = logBuffer[idx];
        if (entry.valid) {
            String ts = buildNanoTimestamp(entry.epochSeconds, entry.millisFraction);
            String value = "[\"" + ts + "\",\"" + jsonEscape(entry.message) + "\"]";

            // Find matching group
            for (int g = 0; g < 4; g++) {
                if (groups[g].level == entry.level) {
                    if (groups[g].count > 0) groups[g].values += ",";
                    groups[g].values += value;
                    groups[g].count++;
                    break;
                }
            }
        }
        idx = (idx + 1) % METRICS_LOG_BUFFER_SIZE;
    }

    // Build Loki push JSON
    String j = "{\"streams\":[";
    bool firstStream = true;
    for (int g = 0; g < 4; g++) {
        if (groups[g].count == 0) continue;
        if (!firstStream) j += ",";
        firstStream = false;
        j += "{\"stream\":{\"job\":\"esp32\",\"device\":\"watering-system\",\"level\":\"";
        j += groups[g].level;
        j += "\"},\"values\":[";
        j += groups[g].values;
        j += "]}";
    }
    j += "]}";
    return j;
}

#endif // METRICS_PUSHER_H
```

- [ ] **Step 2: Verify build compiles**

```bash
pio run -e esp32-s3-devkitc-1 2>&1 | tail -5
```

Expected: `SUCCESS` (MetricsPusher.h compiles but isn't included yet, so no effect)

- [ ] **Step 3: Commit**

```bash
git add include/MetricsPusher.h
git commit -m "v1.20.1: add MetricsPusher header for metrics + log collection"
```

---

## Task 8: Integrate MetricsPusher into main.cpp

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add include**

Add after `#include <ota.h>` (line 33 area):

```cpp
#include <MetricsPusher.h>
```

- [ ] **Step 2: Add MetricsPusher::init() in setup()**

Add after `wateringSystem.init();` in setup():

```cpp
    // Initialize metrics pusher
    MetricsPusher::init();
```

- [ ] **Step 3: Add MetricsPusher::loop() in networkTask**

Add after `DebugHelper::loop();` inside the `if (NetworkManager::isWiFiConnected())` block in networkTask:

```cpp
            // Push metrics and logs to monitoring proxy
            MetricsPusher::loop();
```

- [ ] **Step 4: Add boot log entries**

Add after the boot banner in setup():

```cpp
    MetricsPusher::logInfo("Boot start, version: " + String(VERSION));
```

Add at end of setup() (after "Setup completed" debug line):

```cpp
    MetricsPusher::logInfo("Setup completed, entering main loop");
```

- [ ] **Step 5: Verify build**

```bash
pio run -e esp32-s3-devkitc-1 2>&1 | tail -10
```

Expected: `SUCCESS`

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "v1.20.1: integrate MetricsPusher into networkTask and boot sequence"
```

---

## Task 9: Add log capture points in firmware

**Files:**
- Modify: `include/WateringSystem.h`
- Modify: `include/WateringSystemStateMachine.h`
- Modify: `include/TelegramNotifier.h`
- Modify: `include/NetworkManager.h`

- [ ] **Step 1: Add MetricsPusher include to WateringSystem.h**

MetricsPusher.h already includes WateringSystem.h, so to avoid circular deps, use a forward declaration. Instead, add log calls only in methods that execute AFTER MetricsPusher is included in main.cpp. Since all headers are inline, the include order in main.cpp matters. MetricsPusher.h must be included AFTER WateringSystem.h.

The log calls use `MetricsPusher::logInfo(...)` etc. Since MetricsPusher.h is included after WateringSystem.h in main.cpp, and the WateringSystem methods are `inline` (compiled at call site), the compiler will find MetricsPusher when these methods are actually instantiated. Add a forward declaration at the top of WateringSystem.h:

```cpp
// Forward declaration for metrics logging
class MetricsPusher;
```

- [ ] **Step 2: Add watering event logs in WateringSystemStateMachine.h**

In `processValve()`, add logging at key state transitions. Find the phase transition handlers and add:

After valve opens (PHASE_OPENING_VALVE success):
```cpp
MetricsPusher::logInfo("Valve " + String(valveIndex) + ": opened, checking sensors");
```

After rain check result (PHASE_CHECKING_INITIAL_RAIN):
```cpp
MetricsPusher::logInfo("Valve " + String(valveIndex) + ": rain=" + String(rainDetected ? "WET" : "DRY"));
```

At watering start (PHASE_WATERING begin):
```cpp
MetricsPusher::logInfo("Valve " + String(valveIndex) + ": watering started, pump ON");
```

At watering complete/timeout (PHASE_CLOSING_VALVE):
```cpp
MetricsPusher::logInfo("Valve " + String(valveIndex) + ": closing, duration=" + String(elapsed / 1000) + "s" + (timeoutOccurred ? " TIMEOUT" : ""));
```

- [ ] **Step 3: Add learning algorithm logs in WateringSystem.h**

In `processLearningData()`, add logging at each decision point:

```cpp
MetricsPusher::logInfo("Learning valve " + String(valveIndex) + ": fill=" + String(fillDuration) + "ms, baseline=" + String(baseline) + "ms, mult=" + String(multiplier, 2));
```

In `checkAutoWatering()` when auto-watering triggers:

```cpp
MetricsPusher::logInfo("Auto-watering triggered: valve " + String(i) + ", timeSince=" + String(timeSince / 1000) + "s, emptyDuration=" + String(valve->emptyToFullDuration / 1000) + "s");
```

- [ ] **Step 4: Add safety event logs in WateringSystem.h**

In `checkMasterOverflowSensor()` when overflow detected:
```cpp
MetricsPusher::logError("OVERFLOW detected! Streak=" + String(overflowDetectionStreak) + ", emergency stop ALL");
```

In `checkWaterLevelSensor()` when water level low:
```cpp
MetricsPusher::logWarn("Water tank LOW - blocking watering");
```

In `globalSafetyWatchdog()` when timeout triggers:
```cpp
MetricsPusher::logError("SAFETY WATCHDOG: valve " + String(i) + " forced stop after " + String(elapsed / 1000) + "s");
```

- [ ] **Step 5: Add network event logs**

In `TelegramNotifier.h` sendMessage():
After success: `MetricsPusher::logDebug("Telegram sent OK");`
After failure: `MetricsPusher::logWarn("Telegram send failed, HTTP " + String(httpCode)); MetricsPusher::recordTelegramFailure();`

In `NetworkManager.h`:
On WiFi connected: `MetricsPusher::logInfo("WiFi connected, RSSI=" + String(WiFi.RSSI()));`
On WiFi disconnected: `MetricsPusher::logWarn("WiFi disconnected");`

- [ ] **Step 6: Handle include ordering**

Ensure `src/main.cpp` includes in this order (MetricsPusher.h last because it includes WateringSystem.h):
```cpp
#include <config.h>
#include <DS3231RTC.h>
#include <ValveController.h>
#include <WateringSystem.h>
#include <NetworkManager.h>
#include <TelegramNotifier.h>
#include <DebugHelper.h>
#include <api_handlers.h>
#include <ota.h>
#include <MetricsPusher.h>
```

For headers that call MetricsPusher::log*() but are included before MetricsPusher.h, add at the top:
```cpp
// Forward declaration
class MetricsPusher;
```

Since MetricsPusher methods are static and the headers are all inline (compiled at the translation unit level in main.cpp), as long as MetricsPusher.h is included before main.cpp's actual function bodies are compiled, it will work. The inline methods in WateringSystem.h etc. are instantiated at the point of call, by which time MetricsPusher is fully defined.

**If forward declaration causes issues**: alternatively, make MetricsPusher::log*() available via a simple function pointer or global function. But try the include-order approach first.

- [ ] **Step 7: Verify build**

```bash
pio run -e esp32-s3-devkitc-1 2>&1 | tail -10
```

Expected: `SUCCESS`

- [ ] **Step 8: Run native tests (ensure no regressions)**

```bash
pio test -e native 2>&1 | tail -20
```

Expected: All 36 tests pass (MetricsPusher is not included in native tests, gated by `#ifndef NATIVE_TEST` if needed)

- [ ] **Step 9: Commit**

```bash
git add include/WateringSystem.h include/WateringSystemStateMachine.h include/TelegramNotifier.h include/NetworkManager.h src/main.cpp
git commit -m "v1.20.1: add log capture points for Prometheus/Loki monitoring"
```

---

## Task 10: Create Grafana dashboard

**Files:**
- Create: `tools/grafana-dashboard-esp32.json`

- [ ] **Step 1: Create dashboard JSON**

Create `tools/grafana-dashboard-esp32.json` with a complete Grafana dashboard definition. The dashboard has 4 rows:

Row 1 - Watering Intervals & Learning:
- Valve state timeline (state timeline panel, query: `esp32_valve_state`)
- Interval multiplier over time (time series, query: `esp32_valve_interval_multiplier`)
- Water level % (time series, query: `esp32_valve_water_level_pct`)
- Time until empty (gauge per valve, query: `esp32_valve_time_until_empty_ms / 3600000` to show hours)
- Total cycles (stat, query: `esp32_valve_total_cycles`)

Row 2 - Watering Events & Sensors:
- Watering duration (time series, query: `esp32_valve_watering_seconds`)
- Rain sensor (state timeline, query: `esp32_valve_rain_detected`)
- Pump activity (state timeline, query: `esp32_pump_active`)

Row 3 - System Health:
- Overflow (stat with alert coloring, query: `esp32_overflow_detected`)
- Water tank (state timeline, query: `esp32_water_tank_ok`)
- WiFi RSSI (time series, query: `esp32_wifi_rssi_dbm`)
- Free heap (time series, query: `esp32_free_heap_bytes`)
- Uptime (stat, query: `esp32_uptime_seconds / 3600` hours)
- Telegram failures (stat, query: `esp32_telegram_failures_total`)
- Plant light (state timeline, query: `esp32_plant_light_active`)

Row 4 - Debug Logs:
- Log viewer (logs panel, Loki query: `{job="esp32"}`)
- Log volume (bar chart, Loki query: `sum(count_over_time({job="esp32"}[1m])) by (level)`)

The JSON should use Grafana dashboard model v38+ with proper datasource UIDs (use `${DS_PROMETHEUS}` and `${DS_LOKI}` template variables so the user picks their datasources on import).

- [ ] **Step 2: Import dashboard to Grafana via API**

```bash
# First, get datasource UIDs
ssh user1@45.151.30.146 "curl -s -u admin:admin http://localhost:3000/api/datasources | python3 -m json.tool | grep -E '\"uid\"|\"name\"|\"type\"'"

# Import dashboard
ssh user1@45.151.30.146 "curl -s -u admin:admin -X POST http://localhost:3000/api/dashboards/db \
  -H 'Content-Type: application/json' \
  -d '{\"dashboard\": $(cat tools/grafana-dashboard-esp32.json), \"overwrite\": true}'"
```

If datasource UIDs need to be hardcoded (simpler for single-server setup), replace template variables with actual UIDs from the first command.

- [ ] **Step 3: Verify dashboard loads in Grafana**

Open `http://45.151.30.146:3000` in browser, navigate to the "ESP32 Watering System" dashboard. All panels should render (with data if ESP32 is pushing metrics, or empty if not yet flashed).

- [ ] **Step 4: Commit**

```bash
git add tools/grafana-dashboard-esp32.json
git commit -m "v1.20.1: add Grafana dashboard for ESP32 monitoring"
```

---

## Task 11: Build, flash, and verify end-to-end

- [ ] **Step 1: Build firmware**

```bash
pio run -e esp32-s3-devkitc-1 2>&1 | tail -10
```

Expected: `SUCCESS`

- [ ] **Step 2: Flash firmware to ESP32**

```bash
platformio run -t upload -e esp32-s3-devkitc-1
```

- [ ] **Step 3: Monitor serial output**

```bash
platformio device monitor -b 115200 --raw
```

Watch for:
- `[MetricsPusher] Push failed` errors (should not appear if proxy is running)
- Normal boot sequence with version 1.20.x
- No crashes or watchdog resets

- [ ] **Step 4: Verify metrics flowing to Prometheus**

```bash
ssh user1@45.151.30.146 "curl -s http://localhost:9090/api/v1/query?query=esp32_uptime_seconds | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[\"data\"][\"result\"])'"
```

Expected: Non-empty result with current uptime value

- [ ] **Step 5: Verify logs flowing to Loki**

```bash
ssh user1@45.151.30.146 "curl -s 'http://localhost:3100/loki/api/v1/query_range?query={job=\"esp32\"}&limit=5&start=$(date -d '5 minutes ago' +%s)000000000' | python3 -m json.tool | head -30"
```

Expected: Boot log entries visible

- [ ] **Step 6: Check Grafana dashboard with real data**

Open Grafana dashboard in browser. Panels should show:
- Uptime counting up
- WiFi RSSI value
- Free heap value
- Valve states (all idle)
- Log entries in the logs panel

---

## Task 12: Update CLAUDE.md and commit

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add monitoring section to CLAUDE.md**

Add after the Telegram section:

```markdown
## Monitoring (Prometheus + Loki + Grafana)

**Architecture**: ESP32 pushes metrics JSON (10s active / 60s idle) and debug logs (only when buffer non-empty) to `esp32_metrics_proxy.py` on Cloud.ru via nginx TLS :16443. Proxy stores metrics for Prometheus scraping and forwards logs to Loki.

**Proxy**: `tools/esp32_metrics_proxy.py`, systemd: `esp32-metrics-proxy.service`, env: `/etc/default/esp32-metrics-proxy`, port 18086.

**ESP32 firmware**: `MetricsPusher.h` runs on Core 0 in networkTask. Log buffer: 64 entries circular. Push interval: 10s when any valve active, 60s idle.

**Grafana dashboard**: "ESP32 Watering System" -- 4 rows: Watering Intervals & Learning, Events & Sensors, System Health, Debug Logs.

**Metrics endpoint**: Prometheus scrapes `host.docker.internal:18086/metrics` every 15s.

**Monitoring not working? Checklist**:
1. Check proxy: `sudo systemctl status esp32-metrics-proxy` → restart: `sudo systemctl restart esp32-metrics-proxy`
2. Test proxy health: `curl -s http://127.0.0.1:18086/health`
3. Test metrics endpoint: `curl -s http://127.0.0.1:18086/metrics`
4. Check Prometheus targets: `curl -s http://localhost:9090/api/v1/targets | grep esp32`
5. Check Loki: `curl -s 'http://localhost:3100/loki/api/v1/query?query={job="esp32"}&limit=1'`
6. Check nginx routing: `curl -sk https://localhost:16443/v1/metrics/push` (should return 401, not 404)
7. Check ESP32 serial for `[MetricsPusher]` messages
```

- [ ] **Step 2: Commit all documentation**

```bash
git add CLAUDE.md
git commit -m "v1.20.1: add monitoring documentation to CLAUDE.md"
```

#!/usr/bin/env python3
"""
ESP32 metrics proxy — receives JSON pushes from ESP32, exposes Prometheus metrics,
and forwards Loki log streams to a Loki HTTP API.

Endpoints:
  POST /v1/metrics/push  — receive ESP32 JSON, store latest values in memory
  POST /v1/logs/push     — receive Loki-format JSON, forward to Loki API
  GET  /metrics          — Prometheus text exposition (no auth)
  GET  /health           — health check (no auth)

Auth (POST only):
  METRICS_PROXY_AUTH_TOKEN=<token>
  Header: Authorization: Bearer <token>

Env vars:
  METRICS_PROXY_HOST            (default: 0.0.0.0)
  METRICS_PROXY_PORT            (default: 18086)
  METRICS_PROXY_AUTH_TOKEN      (required for POST auth)
  LOKI_PUSH_URL                 (default: http://localhost:3100/loki/api/v1/push)
"""

from __future__ import annotations

import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse
from urllib.request import Request, urlopen
from urllib.error import URLError


HOST = os.getenv("METRICS_PROXY_HOST", "0.0.0.0")
PORT = int(os.getenv("METRICS_PROXY_PORT", "18086"))
AUTH_TOKEN = os.getenv("METRICS_PROXY_AUTH_TOKEN", "").strip()
LOKI_PUSH_URL = os.getenv("LOKI_PUSH_URL", "http://localhost:3100/loki/api/v1/push")
LOKI_TIMEOUT_SEC = float(os.getenv("LOKI_TIMEOUT_SEC", "5"))

# Thread-safe storage for the latest metrics snapshot from ESP32
_metrics_lock = threading.Lock()
_latest_metrics: dict = {}
_last_push_timestamp: float = 0.0


def _json_response(handler: BaseHTTPRequestHandler, code: int, payload: dict) -> None:
    raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


def _text_response(handler: BaseHTTPRequestHandler, code: int, body: str) -> None:
    raw = body.encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
    handler.send_header("Content-Length", str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


def _require_auth(handler: BaseHTTPRequestHandler) -> bool:
    if not AUTH_TOKEN:
        return True
    auth = (handler.headers.get("Authorization") or "").strip()
    return auth == f"Bearer {AUTH_TOKEN}"


def _forward_to_loki(body: bytes) -> tuple[int, str]:
    """Forward Loki-format JSON to the Loki push API. Returns (status_code, error_or_empty)."""
    req = Request(
        LOKI_PUSH_URL,
        method="POST",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urlopen(req, timeout=LOKI_TIMEOUT_SEC) as resp:
            return resp.status, ""
    except URLError as exc:
        return 502, str(exc)
    except Exception as exc:  # pragma: no cover - runtime I/O path
        return 500, str(exc)


def _build_prometheus_metrics() -> str:
    """Build Prometheus text exposition from the latest stored metrics snapshot."""
    global _last_push_timestamp

    with _metrics_lock:
        data = dict(_latest_metrics)
        last_push = _last_push_timestamp

    lines: list[str] = []

    def gauge(name: str, help_text: str, value, labels: str = "") -> None:
        label_str = f"{{{labels}}}" if labels else ""
        lines.append(f"# HELP {name} {help_text}")
        lines.append(f"# TYPE {name} gauge")
        lines.append(f"{name}{label_str} {value}")

    def counter(name: str, help_text: str, value, labels: str = "") -> None:
        label_str = f"{{{labels}}}" if labels else ""
        lines.append(f"# HELP {name} {help_text}")
        lines.append(f"# TYPE {name} counter")
        lines.append(f"{name}{label_str} {value}")

    # --- System metrics ---
    gauge("esp32_uptime_seconds", "ESP32 uptime in seconds",
          data.get("uptime_s", 0))
    gauge("esp32_free_heap_bytes", "ESP32 free heap memory in bytes",
          data.get("free_heap", 0))
    gauge("esp32_wifi_rssi_dbm", "ESP32 WiFi RSSI in dBm",
          data.get("wifi_rssi", 0))
    gauge("esp32_last_push_timestamp", "Unix timestamp of the last metrics push from ESP32",
          last_push)

    # --- Pump ---
    gauge("esp32_pump_active", "1 if pump is currently active, 0 otherwise",
          data.get("pump", 0))

    # --- Safety / status ---
    gauge("esp32_overflow_detected", "1 if overflow condition is detected",
          data.get("overflow", 0))
    gauge("esp32_overflow_trigger_streak", "Number of consecutive overflow trigger readings",
          data.get("overflow_streak", 0))
    gauge("esp32_water_tank_ok", "1 if water tank level is sufficient",
          data.get("water_tank_ok", 1))
    gauge("esp32_plant_light_active", "1 if plant light relay is on",
          data.get("plant_light", 0))
    counter("esp32_telegram_failures_total", "Total number of Telegram send failures",
            data.get("telegram_failures", 0))

    # --- Per-valve metrics ---
    valves = data.get("valves", [])
    per_valve_defs = [
        ("esp32_valve_state",              "gauge",   "Valve state (0=IDLE, 1=active)",                    "state"),
        ("esp32_valve_phase",              "gauge",   "Valve phase index in the watering cycle",           "phase"),
        ("esp32_valve_rain_detected",      "gauge",   "1 if rain/moisture detected by tray sensor",        "rain"),
        ("esp32_valve_watering_seconds",   "gauge",   "Duration of the last watering cycle in seconds",    "watering_s"),
        ("esp32_valve_water_level_pct",    "gauge",   "Estimated water level percentage for this tray",    "water_level_pct"),
        ("esp32_valve_calibrated",         "gauge",   "1 if valve learning baseline is calibrated",        "calibrated"),
        ("esp32_valve_auto_watering",      "gauge",   "1 if auto-watering is enabled for this valve",      "auto_watering"),
        ("esp32_valve_interval_multiplier","gauge",   "Current learning interval multiplier",              "interval_mult"),
        ("esp32_valve_total_cycles",       "counter", "Total number of completed watering cycles",         "total_cycles"),
        ("esp32_valve_time_since_watering_ms", "gauge", "Milliseconds since last watering",                "time_since_ms"),
        ("esp32_valve_time_until_empty_ms","gauge",   "Estimated milliseconds until tray runs empty",      "time_until_empty_ms"),
        ("esp32_valve_baseline_fill_ms",   "gauge",   "Baseline watering fill duration in milliseconds",   "baseline_fill_ms"),
        ("esp32_valve_last_fill_ms",       "gauge",   "Last measured watering fill duration in ms",        "last_fill_ms"),
        ("esp32_valve_empty_duration_ms",  "gauge",   "Current computed empty-to-full interval in ms",     "empty_duration_ms"),
    ]

    for metric_name, metric_type, help_text, field in per_valve_defs:
        lines.append(f"# HELP {metric_name} {help_text}")
        lines.append(f"# TYPE {metric_name} {metric_type}")
        for valve in valves:
            valve_id = str(valve.get("id", "?"))
            value = valve.get(field, 0)
            lines.append(f'{metric_name}{{valve="{valve_id}"}} {value}')

    return "\n".join(lines) + "\n"


class Handler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802
        global _latest_metrics, _last_push_timestamp

        parsed = urlparse(self.path)

        if parsed.path not in ("/v1/metrics/push", "/v1/logs/push"):
            _json_response(self, 404, {"ok": False, "error": "Not found"})
            return

        if not _require_auth(self):
            _json_response(self, 401, {"ok": False, "error": "Unauthorized"})
            return

        content_length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(content_length)

        if parsed.path == "/v1/metrics/push":
            try:
                payload = json.loads(body.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                _json_response(self, 400, {"ok": False, "error": f"Invalid JSON: {exc}"})
                return

            with _metrics_lock:
                _latest_metrics = payload
                _last_push_timestamp = time.time()

            _json_response(self, 200, {"ok": True})

        else:  # /v1/logs/push
            status, err = _forward_to_loki(body)
            if err:
                _json_response(self, status, {"ok": False, "error": f"Loki error: {err}"})
            else:
                _json_response(self, 200, {"ok": True})

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)

        if parsed.path == "/health":
            with _metrics_lock:
                last_push = _last_push_timestamp
            age = time.time() - last_push if last_push > 0 else None
            _json_response(self, 200, {
                "status": "ok",
                "last_push_age_seconds": round(age, 1) if age is not None else None,
            })
            return

        if parsed.path == "/metrics":
            body = _build_prometheus_metrics()
            _text_response(self, 200, body)
            return

        _json_response(self, 404, {"ok": False, "error": "Not found"})

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        print(f"[esp32-metrics-proxy] {self.address_string()} - {fmt % args}")


def main() -> None:
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"ESP32 metrics proxy listening on {HOST}:{PORT}")
    print(f"Loki push URL: {LOKI_PUSH_URL}")
    server.serve_forever()


if __name__ == "__main__":
    main()

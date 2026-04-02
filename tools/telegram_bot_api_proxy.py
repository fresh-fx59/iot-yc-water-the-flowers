#!/usr/bin/env python3
"""
Minimal Telegram Bot API proxy for monitoring server.

Exposes:
  POST /v1/telegram/sendMessage
  POST /v1/telegram/setMyCommands
  GET  /v1/telegram/getUpdates

Optional auth:
  TELEGRAM_PROXY_AUTH_TOKEN=<token>
  Header: Authorization: Bearer <token>
"""

from __future__ import annotations

import json
import os
import ssl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlencode, urlparse
from urllib.request import Request, urlopen


HOST = os.getenv("TELEGRAM_PROXY_HOST", "0.0.0.0")
PORT = int(os.getenv("TELEGRAM_PROXY_PORT", "16443"))
AUTH_TOKEN = os.getenv("TELEGRAM_PROXY_AUTH_TOKEN", "").strip()
UPSTREAM_TIMEOUT_SEC = float(os.getenv("TELEGRAM_PROXY_TIMEOUT_SEC", "20"))
TLS_CERT_FILE = os.getenv("TELEGRAM_PROXY_TLS_CERT_FILE", "").strip()
TLS_KEY_FILE = os.getenv("TELEGRAM_PROXY_TLS_KEY_FILE", "").strip()


def _parse_form(body: bytes) -> dict[str, str]:
    parsed = parse_qs(body.decode("utf-8"), keep_blank_values=True)
    return {k: v[0] if v else "" for k, v in parsed.items()}


def _json_response(handler: BaseHTTPRequestHandler, code: int, payload: dict) -> None:
    raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


def _require_auth(handler: BaseHTTPRequestHandler) -> bool:
    if not AUTH_TOKEN:
        return True
    auth = (handler.headers.get("Authorization") or "").strip()
    return auth == f"Bearer {AUTH_TOKEN}"


def _telegram_request(bot_token: str, method: str, params: dict[str, str]) -> tuple[int, bytes]:
    upstream_url = f"https://api.telegram.org/bot{bot_token}/{method}"
    payload = urlencode(params).encode("utf-8")
    req = Request(
        upstream_url,
        method="POST",
        data=payload,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    with urlopen(req, timeout=UPSTREAM_TIMEOUT_SEC) as resp:
        return int(resp.status), resp.read()


class Handler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802
        if self.path not in ("/v1/telegram/sendMessage", "/v1/telegram/setMyCommands"):
            _json_response(self, 404, {"ok": False, "error": "Not found"})
            return
        if not _require_auth(self):
            _json_response(self, 401, {"ok": False, "error": "Unauthorized"})
            return

        content_length = int(self.headers.get("Content-Length", "0"))
        form = _parse_form(self.rfile.read(content_length))
        bot_token = form.get("bot_token", "").strip()
        method = "sendMessage" if self.path.endswith("/sendMessage") else "setMyCommands"

        if method == "sendMessage":
            chat_id = form.get("chat_id", "").strip()
            text = form.get("text", "")
            parse_mode = form.get("parse_mode", "HTML")

            if not bot_token or not chat_id:
                _json_response(self, 400, {"ok": False, "error": "Missing bot_token/chat_id"})
                return

            params = {"chat_id": chat_id, "text": text, "parse_mode": parse_mode}
        else:
            commands = form.get("commands", "").strip()

            if not bot_token or not commands:
                _json_response(self, 400, {"ok": False, "error": "Missing bot_token/commands"})
                return

            params = {"commands": commands}

        try:
            status, raw = _telegram_request(bot_token, method, params)
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except Exception as exc:  # pragma: no cover - runtime I/O path
            _json_response(self, 502, {"ok": False, "error": f"Upstream error: {exc}"})

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            _json_response(self, 200, {"status": "ok"})
            return
        if parsed.path != "/v1/telegram/getUpdates":
            _json_response(self, 404, {"ok": False, "error": "Not found"})
            return
        if not _require_auth(self):
            _json_response(self, 401, {"ok": False, "error": "Unauthorized"})
            return

        query = parse_qs(parsed.query, keep_blank_values=True)
        bot_token = (query.get("bot_token", [""])[0] or "").strip()
        offset = (query.get("offset", ["0"])[0] or "0").strip()
        timeout = (query.get("timeout", ["10"])[0] or "10").strip()
        allowed_updates = query.get("allowed_updates", ["[\"message\"]"])[0]

        if not bot_token:
            _json_response(self, 400, {"ok": False, "error": "Missing bot_token"})
            return

        try:
            status, raw = _telegram_request(
                bot_token,
                "getUpdates",
                {
                    "offset": offset,
                    "timeout": timeout,
                    "allowed_updates": allowed_updates,
                },
            )
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except Exception as exc:  # pragma: no cover - runtime I/O path
            _json_response(self, 502, {"ok": False, "error": f"Upstream error: {exc}"})

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        print(f"[telegram-proxy] {self.address_string()} - {fmt % args}")


def main() -> None:
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    tls_enabled = bool(TLS_CERT_FILE and TLS_KEY_FILE)
    if tls_enabled:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=TLS_CERT_FILE, keyfile=TLS_KEY_FILE)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"Telegram proxy listening on {HOST}:{PORT} tls={str(tls_enabled).lower()}")
    server.serve_forever()


if __name__ == "__main__":
    main()

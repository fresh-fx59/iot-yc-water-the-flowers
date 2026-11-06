#!/usr/bin/env python3
"""Direct LAN access to the rig: status, firmware upload, update and rollback triggers.

Bootstrap and recovery path. Login comes from include/secret.h and is never printed.
"""
import argparse
import base64
import json
import re
import uuid
from pathlib import Path
from urllib.request import ProxyHandler, Request, build_opener

ROOT = Path(__file__).resolve().parents[1]
OPENER = build_opener(ProxyHandler({}))  # the Mac VPN proxy must not carry LAN traffic


def secret(name):
    header = (ROOT / "include/secret.h").read_text()
    return json.loads(re.search(r"^#define\s+" + name + r'\s+("[^"\n]*")', header, re.M)[1])


def call(host, path, data=None, content_type=None, timeout=45):
    request = Request(f"http://{host}{path}", data=data, method="POST" if data is not None else "GET")
    login = base64.b64encode(f"{secret('OTA_USER')}:{secret('OTA_PASSWORD')}".encode()).decode()
    request.add_header("Authorization", "Basic " + login)
    if content_type:
        request.add_header("Content-Type", content_type)
    with OPENER.open(request, timeout=timeout) as response:
        return response.status, response.read().decode(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--status", action="store_true")
    action.add_argument("--image", type=Path, help="upload this firmware.bin, then the device reboots")
    action.add_argument("--check-update", action="store_true")
    action.add_argument("--force-update", action="store_true")
    action.add_argument("--rollback", action="store_true")
    args = parser.parse_args()

    if args.status:
        print(call(args.host, "/api/status")[1])
    elif args.image:
        image = args.image.read_bytes()
        if not image or image[0] != 0xE9:
            raise SystemExit("not an ESP32 app image")
        boundary = uuid.uuid4().hex
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
                "Content-Type: application/octet-stream\r\n\r\n").encode() + image + f"\r\n--{boundary}--\r\n".encode()
        status, _ = call(args.host, "/firmware", body, f"multipart/form-data; boundary={boundary}", timeout=300)
        print("upload HTTP", status, len(image), "bytes")
    elif args.check_update or args.force_update:
        print(call(args.host, "/api/check_update" + ("?force=1" if args.force_update else ""), b"")[0])
    else:
        print(call(args.host, "/api/rollback", b"")[0])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Publish a rig firmware image for the Telegram /check_update path.

Images hold private credentials: they stay outside Git and behind the proxy's
bearer check. The mini watering manifest at /v1/firmware/ is not touched.
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
BOARD = "esp32-s3-n8r2-6valve"
APP_PARTITION_BYTES = 0x140000  # default.csv app0/app1
REMOTE_DIR = "/var/www/firmware/rig"
URL_PREFIX = "/v1/firmware/rig/"
SSH_KEY = str(Path.home() / ".ssh/id_ed25519_monitoring")


def build_manifest(version, image, notes="", wrong_sha_for_test=False):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("version must be N.N.N")
    if not image or image[0] != 0xE9:
        raise ValueError("not an ESP32 app image")
    if len(image) > APP_PARTITION_BYTES:
        raise ValueError(f"image is {len(image)} bytes, partition holds {APP_PARTITION_BYTES}")
    if f"watering_system_{version}\0".encode() not in image:
        raise ValueError(f"image does not contain version {version}: wrong build")
    digest = hashlib.sha256(image).hexdigest()
    if wrong_sha_for_test:
        digest = hashlib.sha256(digest.encode()).hexdigest()
    manifest = {
        "version": version,
        "url": f"{URL_PREFIX}firmware-{version}.bin",
        "board": BOARD,
        "size": len(image),
        "sha256": digest,
    }
    if notes:
        manifest["notes"] = notes
    return manifest


def read_secret(name):
    header = (ROOT / "include/secret.h").read_text()
    return json.loads(re.search(r"^#define\s+" + name + r'\s+("[^"\n]*")', header, re.M)[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--notes", default="")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--wrong-sha-for-test", action="store_true",
                        help="acceptance run only: manifest checksum will not match the image")
    args = parser.parse_args()

    image = args.image.read_bytes()
    manifest = build_manifest(args.version, image, args.notes, args.wrong_sha_for_test)
    if args.dry_run:
        print(json.dumps(manifest, indent=2))
        return

    base = read_secret("TELEGRAM_PROXY_BASE_URL").rstrip("/")
    token = read_secret("TELEGRAM_PROXY_AUTH_TOKEN")
    try:
        urlopen(base + URL_PREFIX + "manifest.json", timeout=15)
        raise RuntimeError("refusing to publish: route answers without a token")
    except HTTPError as error:
        if error.code != 401:
            raise RuntimeError(f"auth guard not verified: HTTP {error.code}") from None

    ssh_host = os.environ["OTA_SSH_TARGET"]  # user@host of the proxy server
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-i", SSH_KEY, ssh_host]
    stage = f"/tmp/rig-publish-{args.version}"
    subprocess.run(ssh + [f"rm -rf {shlex.quote(stage)} && mkdir -m 700 {shlex.quote(stage)}"],
                   check=True, capture_output=True)
    with tempfile.TemporaryDirectory(prefix="rig-publish-") as folder:
        local = Path(folder)
        (local / "firmware.bin").write_bytes(image)
        (local / "manifest.json").write_text(json.dumps(manifest) + "\n")
        subprocess.run(["scp", "-q", "-i", SSH_KEY, str(local / "firmware.bin"), str(local / "manifest.json"),
                        f"{ssh_host}:{stage}/"], check=True, capture_output=True)

    remote = f"""from pathlib import Path
import grp, json, os, shutil
stage = Path({stage!r})
info = json.loads((stage / 'manifest.json').read_text())
image = (stage / 'firmware.bin').read_bytes()
assert len(image) == info['size']
out = Path({REMOTE_DIR!r})
out.mkdir(mode=0o750, exist_ok=True)
gid = grp.getgrnam('www-data').gr_gid
os.chown(out, 0, gid)
os.chmod(out, 0o750)
target = out / ('firmware-' + info['version'] + '.bin')
if target.exists():
    assert target.read_bytes() == image, 'refuse to replace a published version with other bytes'
else:
    target.write_bytes(image)
os.chmod(target, 0o640)
os.chown(target, 0, gid)
pending = out / 'manifest.json.pending'
pending.write_text(json.dumps(info) + '\\n')
os.chmod(pending, 0o640)
os.chown(pending, 0, gid)
pending.replace(out / 'manifest.json')
shutil.rmtree(stage)
"""
    subprocess.run(ssh + ["sudo -n python3 -"], input=remote, text=True, check=True, capture_output=True)

    def fetch(path):
        request = Request(base + path, headers={"Authorization": "Bearer " + token})
        with urlopen(request, timeout=60) as response:
            return response.read()

    assert json.loads(fetch(URL_PREFIX + "manifest.json")) == manifest, "manifest mismatch"
    assert fetch(manifest["url"]) == image, "image mismatch"
    print(f"PASS: published v{args.version}, {len(image)} bytes, sha256 {manifest['sha256'][:12]}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print("Publication failed: command exit", error.returncode)
        raise SystemExit(1)
    except Exception as error:  # never echo a URL or token
        print("Publication failed:", type(error).__name__, getattr(error, "code", ""), str(error)[:200] if isinstance(error, (ValueError, RuntimeError, AssertionError)) else "")
        raise SystemExit(1)

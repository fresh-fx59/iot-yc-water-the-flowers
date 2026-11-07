# Telegram firmware update (6-tray rig)

Send `/check_update` to the rig's bot. The device downloads the published image itself, so the phone and Mac need no LAN access.

| Command | Result |
|---|---|
| `/check_update` | Installs the published version if it is newer. |
| `/check_update force` | Re-flashes the published version, even at the same or a lower version. |
| `/rollback` | Boots the other partition. It may hold an older version, possibly one without `/check_update`. |

## What to expect

`Checking` → `Updating vA → vB` → `Flashed vB. Rebooting` → `Device Online` → `vB confirmed healthy`, in a few minutes. Watering is on hold during the update.

- A running or queued cycle gives `Busy`. Retry after it, or send `/halt` first. Halt does not survive the reboot, as with LAN upload.
- `Update failed: ...` means nothing changed and watering resumed.
- `Rollback: vB failed its health check` means the old version runs again.
- A `checking` message alone is not success. `confirmed healthy` is.

The new image must, within 5 minutes of boot, push metrics once, poll Telegram once, and fetch the manifest once with a verified certificate. Otherwise, or if it crashes before that, the device returns to the previous partition. LittleFS data (learning, web pages) is not touched; an update ships the app image only.

## Publish a new version

1. Set `FIRMWARE_VERSION` in `include/config.h` and the version in `CLAUDE.md`.
2. Run `pio test -e native` and `pio run -e esp32-s3-devkitc-1`.
3. Publish:

```sh
OTA_SSH_TARGET=<user@proxy-host> tools/publish_firmware.py --version X.Y.Z \
  --image .pio/build/esp32-s3-devkitc-1/firmware.bin --notes "short text"
```

The tool refuses an image that lacks the version string, is too big for the partition, or replaces a published version with other bytes. It then reads the manifest and image back with the token. Images hold credentials: never commit them.

The server folder is `/var/www/firmware/rig/`, served at `/v1/firmware/rig/` behind the proxy bearer check. The mini watering firmware uses `/v1/firmware/`. The two are separate on purpose, and the `board` field blocks a mini image here.

## LAN recovery

`http://<device-ip>/firmware` stays available as the path that needs no Telegram, no proxy and no working updater. `tools/lan_update.py --host <device-ip>` wraps it, with `--status`, `--image <file>`, `--check-update`, `--force-update`, `--rollback`.

## Changing the proxy token

`/check_update` cannot deliver a new proxy token. The device uses the same `TELEGRAM_PROXY_AUTH_TOKEN` for Telegram, metrics and the firmware download. Once the server has the new token, the old image gets 401 and cannot fetch anything. An image with the new token, installed before the server changes, fails its health check and rolls back. Change the server first, then load the new image with LAN upload or USB. This comes from reading the code; it has not been run on the device.

## Acceptance run

Run after changing the updater, with the device on a version that has the updater:

1. Publish a newer version with `--wrong-sha-for-test`, then trigger an update. Expect `checksum mismatch`, no reboot, and watering resumed.
2. Build `esp32-s3-ota-fail-health`, publish as `1.32.90`, trigger an update. Expect a rollback about 5 minutes after boot. Then delete that image from the server.
3. Publish the real version and send `/check_update` in Telegram. Expect `confirmed healthy`. Send it again: expect `Up to date` and no reboot loop.

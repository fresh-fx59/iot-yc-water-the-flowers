# Baseline duration anomaly evidence (2026-10-26)

Source: Loki via the vault `tools/waterlog` read-only query, device label
`watering-system`. Times below are the rendered Loki timestamps.

## Live firmware identity

```
10-05 11:28:00 Boot start, version: watering_system_1.31.0
10-05 11:28:20 Setup completed - starting main loop
10-16 15:40:04 Boot start, version: watering_system_1.31.0
10-16 15:40:21 Setup completed - starting main loop
```

## Tray 5 sequence

The Oct 4 cycle opened the valve and remained dry through the available log
window. The next cycle on Oct 5 immediately saw the tray wet and closed the
valve, but learning used a 24-hour elapsed value:

```
10-04 05:58:47 Tray 5: watering started
10-04 05:58:47 Tray 5: opened
10-04 05:58:48 Tray 5: 1s/23s, Sensor: DRY
10-04 05:59:02 Tray 5: 15s/9s, Sensor: DRY
10-05 05:58:47 Tray 5 sensor GPIO 12: 7/7 LOW (WET), GPIO18=PULSED
10-05 05:58:47 Tray 5: opened
10-05 05:58:48 Tray 5 sensor already WET - tray is FULL
10-05 05:58:48 CLOSING VALVE for Tray 5 (GPIO 16)
10-05 05:58:48 Session tracking: Tray 5 ended - Status: OK, Duration: 1.1s (valve open to close)
10-05 05:58:48 Fill duration: 86400.5s
10-05 05:58:48 NEW BASELINE: 86400.5s
10-05 05:58:48 Tray 5: learning: fill=86400.5s interval 3.75x->4.75x
```

Later actual watering remained normal while the persisted baseline stayed
corrupt:

```
10-09 23:59:08 Tray 5: learning: fill=24.0s interval 4.75x->5.75x
10-15 17:59:31 Session tracking: Tray 5 ended - Status: OK, Duration: 22.2s (valve open to close)
10-15 17:59:31 Tray 5: learning: fill=21.6s interval 5.75x->6.75x
```

## Tray 1 persisted baseline

```
10-01 16:55:02 Tray 1: learning: fill=29.7s interval 5.00x->6.00x
10-01 16:55:02 Baseline: 98746.7s
10-07 16:55:30 Tray 1: learning: fill=30.6s interval 6.00x->7.00x
10-07 16:55:30 Baseline: 98746.7s
10-14 16:56:01 Session tracking: Tray 1 ended - Status: OK, Duration: 31.5s (valve open to close)
10-14 16:56:01 Tray 1: learning: fill=30.9s interval 7.00x->7.00x
10-14 16:56:01 Baseline: 98746.7s
```

The source calculates `fillDuration` as `currentTime - wateringStartTime` and
updates the baseline whenever the fill exceeds the previous baseline. The
full-at-start path logs the short physical session but leaves the prior start
reference available to learning, producing the 86400.5s value. The Tray 1
98746.7s value is consistent with the same stale-baseline class; its precise
origin is unproven because the originating cycle is outside the queried
30-day Loki window. The specific abort path that left Tray 5’s start marker
stale is also unproven; emergency and manual abort paths in the source both
previously retained that marker.

No actuator, Telegram command, deployment, or device write was performed while
collecting this evidence.

## Repair and validation

Prepared version: `watering_system_1.31.1`.

- Clear pump-start marker on every new cycle and confirmed full-at-start path.
- Reject impossible fill durations before updating learning data.
- Repair an invalid saved baseline using valid last-fill or previous-fill data; if neither exists, reacquire a fill baseline at the next successful cycle. Keep calibration, interval multipliers and cycle counters.
- Native suite:64/64 passed. Regressions cover observed corrupt values, fallback ordering, aborted-cycle/full-start lifecycle state, and baseline reacquisition preserving a seven-day schedule. They exercise the production-used state helpers, not the complete GPIO/network class. Isolated source mutations demonstrate failures for missing marker resets and accidental multiplier reset to1x.
- Production ESP32-S3 firmware build passed.

Native test command on this Mac:

```sh
SDKROOT=$(xcrun --sdk macosx --show-sdk-path) \
CPLUS_INCLUDE_PATH="$(xcrun --sdk macosx --show-sdk-path)/usr/include/c++/v1" \
pio test -e native
```

Deployment is pending. Device hostname did not resolve; HTTP to the last logged
IP `192.168.100.28` timed out. No USB serial device is available. The current
host route uses gateway172.29.224.1, so local reachability is not established.
No firmware upload, filesystem replacement or valve command was performed.

Final production binary SHA-256: `7f94d832337f8a74ab54c0ba455468212bae6350f9a3448794ebfd7c4b180dbc`.

Final focused gpt-5.6-sol review: PASS after correcting schedule preservation, lifecycle regression coverage and evidence precision.

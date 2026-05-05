# Mini Fork — Single-Zone ESP32 Watering Device

**Status**: Spec, awaiting user approval
**Date**: 2026-05-05
**Source project**: `iot-yc-water-the-flowers` (this repo) at HEAD
**Target repo (new)**: `iot-yc-water-the-flowers-mini` (separate git repo)

## Goal

Build a simpler ESP32-based watering device that waters a single zone on a time-based schedule, reuses the mother project's network/Telegram/monitoring/OTA infrastructure, and is reliable enough to run unattended for ~2 weeks.

## Hardware

- **MCU board**: VCC-GND Studios **YD-ESP32-23 v1.3** (silkscreened "YD-ESP32-23 2022-v1.3, esp32-s3-n16r8"). ESP32-S3 with **16 MB flash + 8 MB PSRAM**. Same chip family as the mother project (which uses S3-N8R2), so the Arduino core, FreeRTOS dual-core pattern, LittleFS, and most pin numbers carry over. Differences: larger flash (room for safer OTA partitions), different USB layout (YD has both USB-OTG and UART USB-C), some GPIOs broken out differently.
- **PlatformIO env**: `esp32-s3-devkitc-1` board target with a custom 16 MB partition table (`partitions_16mb.csv`) — bigger app/OTA partitions plus a generous LittleFS region.
- **RTC**: DS3231 over I²C. Same module as mother project. Sole time source — no NTP.
- **Motor relay**: 1 channel. Polarity (active-high vs active-low) configurable in `config.h` via `MOTOR_RELAY_ACTIVE_HIGH` flag.
- **Soil moisture sensor**: 1 capacitive sensor (GND, VCC, AOUT). Must be wired to an **ADC1** GPIO (GPIO 1–10 on S3); ADC2 is unusable while WiFi is active.
- **Floor overflow sensor**: 1 rain-drop sensor module (VCC, GND, DO, AO). DO line wired to a digital input with internal pullup; LOW = water on floor. AO unused.

Specific GPIO pin assignments are filled in once the board is in hand and breakout pin labels confirmed. All pins live in `config.h`. Constraints to honor when assigning:
- AOUT → ADC1 only (GPIO 1–10).
- Avoid strapping pins (GPIO 0, 3, 45, 46) for outputs unless boot levels are acceptable.
- I²C for DS3231 may stay on GPIO 14 (SDA) / GPIO 3 (SCL) like mother project, or move — verify the YD-ESP32-23 breakout exposes these.

## Scope — MVP vs Deferred

**MVP (must work before user leaves on 2026-05-15-ish)**:
- Time-based schedule (mode A) with overrides via Telegram for interval / time-of-day / max runtime.
- Manual trigger (Telegram, web UI).
- Floor overflow latch with manual reset (persisted across reboot).
- Telegram notifications, web UI status, Prometheus/Loki push, OTA firmware + filesystem updates.

**Deferred (no day-1 hooks, no flags)**:
- Mode B (sensor-driven only).
- Mode C (composition of A + B).
- Mode D (adaptive learning of interval).
  Rationale: Each adds non-trivial state and tuning surface, none is needed for the 2-week deployment, and YAGNI hooks cost real complexity. Add later as discrete features when there is a real need.

## Architecture

### Dual-core split (kept from mother project)

The pattern is solid and lets us reuse `NetworkManager`, `TelegramNotifier`, `MetricsPusher`, `ota.h` largely verbatim.

- **Core 1, 10ms loop**: schedule check, soil reading, motor state machine, overflow watchdog. Never touches network.
- **Core 0, 100ms loop (`networkTask`)**: WiFi, Telegram, web server, OTA, metrics push.
- **Cross-core primitives** (same as mother project):
  - `notificationQueue` (16-slot FreeRTOS queue) for Core 1 → Core 0 alerts.
  - `g_metricsLog` callback for Loki log push.

### Header-only modules

All logic lives in `include/*.h`. `src/main.cpp` is a thin entry point.

**New / rewritten** for the mini:
| Module | Responsibility |
|---|---|
| `config.h` | pins, defaults (interval=4 days, time=07:00, runtime=120s, overflow debounce 5/7), motor polarity flag, version |
| `Settings.h` | load/save `/settings.json`, runtime mutation via Telegram/web |
| `Scheduler.h` | given `last_run`, `interval_days`, `schedule_hhmm` → compute `next_run_time`; grace-window logic for late-firing after reboot |
| `MoistureSensor.h` | ADC read (with light averaging), threshold compare, calibration helpers |
| `OverflowSensor.h` | debounced DO read (5/7 LOW = trip), latch state, persisted via `Settings.h` |
| `WateringController.h` | 3-state SM (IDLE → WATERING → IDLE), motor on/off, timeout, soil-wet stop, overflow interrupt |
| `api_handlers.h` | inline web API endpoints (rewritten — no valves[], no plant lamp) |

**Reused from mother project, mostly verbatim** (prune mother-specific formatters where they reference valves/plant lamp):
- `DS3231RTC.h`, `NetworkManager.h`, `TelegramNotifier.h`, `MetricsPusher.h`, `DebugHelper.h`, `ota.h`, `secret.h`, `TestConfig.h`

**Dropped entirely**:
- `StateMachineLogic.h`, `LearningAlgorithm.h`, `WateringSystem.h`, `WateringSystemStateMachine.h`, `ValveController.h`, `PlantLightController.h`
- `src/test-main.cpp` and the `esp32-s3-devkitc-1-test` PlatformIO env (one production firmware only)
- All `learning_data_*.json` references and persistence logic
- The mother's `data/web/prod/` UI (rebuilt smaller — single zone, single sensor)
- Mother's `test/` directory (fresh tests written for the new SM)

## Watering Logic

### State machine

Three states; transitions are intentionally trivial.

```
IDLE ──(time-to-water OR manual cmd)──► WATERING ──(soil-wet OR timeout OR overflow)──► IDLE
```

### Invariants

- Motor relay is asserted ON **only** when state == WATERING. Any transition into IDLE (success, timeout, overflow, halt, manual `/stop`) forces motor OFF before completing the transition.
- `last_run_unix` semantics: "last time the device attempted a cycle" (success OR skip-due-to-wet). The timeout path is the only exception — it does NOT advance `last_run_unix`, so the next schedule check fires the same day for retry after diagnosis.
- **Global watchdog**: independent of the SM, every Core 1 loop checks: if `motorOnGpioState == HIGH` AND `now - motor_start_time > max_runtime + 5s` → force motor OFF, log a critical alert, and `ESP.restart()`. Insurance against SM wedges from brownouts or stack corruption. Mirrors mother project's `globalSafetyWatchdog`.
- **Settings concurrency**: settings are loaded once at boot into an in-memory struct. Core 1 reads via const accessor only. Mutations (Telegram or web) go through a single Core 0 function that updates the in-memory copy then atomically writes `/settings.json` (write to `/settings.json.tmp`, fsync, rename — LittleFS supports rename). No mutex needed because Core 1 sees an atomic-pointer-sized update; in practice booleans/ints/short strings are torn-write-safe on ESP32.
- **Calibration → threshold**: when both `calibration_wet` and `calibration_dry` exist, `soil_threshold = (calibration_wet + calibration_dry) / 2`. The user can also override via `/set_threshold`.

### Per-loop check on Core 1 (every 10ms)

1. **Overflow watchdog (always first)**: read floor sensor with 5/7 debounce (5 LOW reads out of the last 7 polls = trip; same algorithm as mother project). On trip → set `overflow_latched = true`, persist to `/state.json`, force motor off, queue Telegram alert. Stays latched until `/reset_overflow`.
2. If `overflow_latched` → refuse all watering, return.
3. If state == IDLE:
   - **Schedule trigger**: if `now ≥ next_run_time` → consider watering. Grace cap: if `now - next_run_time > 12h`, skip and recompute `next_run_time` for the next scheduled hour.
   - **Pre-check soil**: read AOUT. If wet → log "skipped, already wet", advance `last_run_time` and `next_run_time`, increment `consecutive_skips_wet`, queue Telegram notice. If `consecutive_skips_wet ≥ 2` → escalate alert ("possible stuck-wet sensor — verify before plants die").
   - Else → transition to WATERING, open relay, record `motor_start_time`.
4. If state == WATERING:
   - Read soil every 100ms. If wet → close relay, transition to IDLE, set `consecutive_skips_wet = 0`, advance `last_run_time` and `next_run_time`, persist, queue Telegram success.
   - If `now - motor_start_time > max_runtime` → close relay, transition to IDLE, queue Telegram alert ("timeout — soil never reached threshold; sensor stuck dry, leak, or pots not absorbing"). Do NOT advance `last_run_time` (so user can retry after diagnosis).
   - If overflow trips during watering → close relay immediately, latch, alert.

### Manual trigger (`/water` or web button)

- If WATERING already → reject "already running".
- If `overflow_latched` → reject "overflow latched, run /reset_overflow".
- If halted → reject "halted, run /resume".
- Else → enter WATERING via the same path, no schedule check.

### Halt mode

- `/halt` → block transitions to WATERING. NOT persisted across reboot (matches mother project gotcha #8).
- `/resume` → re-enable schedule.

### Reset rules for `consecutive_skips_wet`

Reset to 0 on:
1. Any WATERING cycle that completes successfully (motor ran, sensor reached wet).
2. Any pre-check soil read that returns DRY (proves sensor isn't stuck wet).

The 2-cycle threshold triggers an alert only — no hard halt. The user decides whether to investigate.

## Telegram Commands

Naming follows mother-project style (`snake_case`, lowercase). Commands present in mother are kept verbatim where applicable.

### Control

| Command | Behavior |
|---|---|
| `/menu` | inline button panel (mother style) |
| `/help` | command reference |
| `/water` | manual trigger now (rejects if WATERING / halted / overflow) |
| `/halt` | pause schedule (non-persistent) |
| `/resume` | re-enable schedule |
| `/status` | state, last_run, next_run, soil reading, overflow, motor state, halt state |
| `/stop` | abort an in-progress WATERING (close relay) |
| `/reset_overflow` | clear the overflow latch (only command that can clear it) |
| `/overflow_status` | show debounce streak and raw value |
| `/reinit_gpio` | recovery escape hatch (kept from mother) |

### Settings (persisted to LittleFS, take effect immediately)

| Command | Default | Notes |
|---|---|---|
| `/set_interval <days>` | 4 | integer days |
| `/set_time <HH:MM>` | 07:00 | 24h clock |
| `/set_runtime <seconds>` | 120 | hard ceiling per cycle |
| `/set_threshold <0-4095>` | TBD on bench | ADC value below which soil is considered wet (capacitive sensors typically read lower-when-wet, but verify on bench) |
| `/calibrate_wet` | — | record current AOUT as wet reference |
| `/calibrate_dry` | — | record current AOUT as dry reference |

### Time

| Command | Notes |
|---|---|
| `/time` | show RTC time (kept from mother) |
| `/settime [YYYY-MM-DD HH:MM:SS]` | set RTC (kept from mother) |

### Diagnostics

| Command | Notes |
|---|---|
| `/test_motor <seconds>` | pulse motor briefly without going through SM |
| `/test_sensor` | read AOUT now (no watering) |

## Web UI

`data/web/prod/index.html` + `app.js` + `style.css`, served by ESP32. Single page, mother-project visual style for familiarity.

### Sections

1. **Status** — state, motor on/off, soil reading (live-polled at 1 Hz via `GET /api/status`, with threshold reference line), last/next run, overflow status, halt state.
2. **Controls** — `/water`, `/stop`, `/halt`, `/resume`, `/reset_overflow` buttons.
3. **Settings form** — interval, time, runtime, threshold; submit POSTs `/api/settings`.
4. **Calibration** — buttons to capture wet/dry references with current AOUT shown.
5. **OTA** — link to `/firmware` (basic auth, admin / `OTA_PASSWORD`) for both firmware and filesystem upload.

### API endpoints

- `GET /api/status` — JSON, see schema below
- `POST /api/water` — manual trigger
- `POST /api/stop` — abort current cycle
- `POST /api/halt`, `POST /api/resume`
- `POST /api/reset_overflow`
- `GET /api/settings`, `POST /api/settings` — full settings object
- `POST /api/calibrate?ref=wet|dry`
- `GET /api/test_sensor`
- `POST /api/test_motor?seconds=N`
- `GET /firmware`, `POST /firmware`, `POST /filesystem` — auth-gated OTA (kept verbatim from mother `ota.h`)

### `/api/status` schema

Field names mirror mother project where they overlap, so any throwaway scripts and the user's mental model port over. Drop everything valve-specific.

```jsonc
{
  "version": "0.1.0",
  "uptime_ms": 1234567,
  "state": "IDLE",            // IDLE | WATERING
  "halted": false,
  "pump": false,              // motor relay state — same field name as mother
  "overflow": {
    "detected": false,        // latched flag
    "raw_value": 1,           // current DO read
    "trigger_streak": 0       // current LOW-streak count toward 5/7 trip
  },
  "soil": {
    "raw": 2100,              // last AOUT reading
    "pct": 42,                // 0-100, derived from calibration_dry/wet
    "threshold": 1800,        // current wet threshold
    "last_read_unix": 1714900000
  },
  "schedule": {
    "interval_days": 4,
    "time_hhmm": "07:00",
    "last_run_unix": 1714723200,
    "next_run_unix": 1715068800,
    "consecutive_skips_wet": 0
  }
}
```

## Persistence (LittleFS)

| File | Contents |
|---|---|
| `/settings.json` | `{ interval_days, schedule_hhmm, max_runtime_sec, soil_threshold, calibration_dry, calibration_wet }` |
| `/state.json` | `{ last_run_unix, next_run_unix, overflow_latched, consecutive_skips_wet }` — written after every state change, read on boot |

No `learning_data_*.json` (no per-valve learning).

**Filesystem OTA is destructive.** Uploading a new LittleFS image via `POST /filesystem` wipes both files. Boot logic must rewrite defaults if either is missing — never assume they exist. Document this prominently in the bot user guide so the user doesn't lose calibration accidentally.

### Boot behavior

1. Init RTC, LittleFS.
2. Load `/settings.json` (fallback to compile-time defaults if missing — write defaults on first boot).
3. Load `/state.json` (fallback: `last_run = 0`, `next_run = compute from defaults`, `overflow_latched = false`, `consecutive_skips_wet = 0`).
4. If `overflow_latched` → stay refusing all watering, alert on Telegram once WiFi up.
5. If `now ≥ next_run_time` AND `now - next_run_time < 12h` (grace) → fire watering at boot.
6. If grace exceeded → recompute `next_run_time` for the next scheduled hour.

## Cloud.ru VPS

No infrastructure changes. Same Cloud.ru proxy at `https://water-the-flowers-proxy.aiengineerhelper.com:16443/` accepts any bot token.

- **Telegram**: register a new BotFather bot for the mini with its own token (bot is `@iot_alex_watering_1_bot`). Each device gets its own bot and its own DM thread in Telegram with the user. The user's `chat_id` (e.g., `314102923`) is the same for both bots since both DM the same person. The bot token lives in the new repo's `secret.h` (gitignored, never committed).
- **Telegram queue resilience**: inherits mother's 16-slot `notificationQueue`. During a WiFi outage, alerts beyond 16 silently drop. Acceptable for the mini given low alert frequency (typically <10 alerts per week of normal operation).
- **Prometheus**: add a new scrape job `esp32_watering_mini` (separate from `esp32_watering`) for clean dashboard separation. Or add a `device` label on the existing job — either works; new job is recommended.
- **Grafana**: fork `tools/grafana-dashboard-esp32.json` into `tools/grafana-dashboard-esp32-mini.json` with simplified panels (single zone, single soil sensor, single motor; drop per-valve learning panels).

### Considered and deferred: single-bot supergroup topic routing

We explored consolidating both devices under one bot in a Telegram supergroup with forum topics (one topic per device). The blocker is that `getUpdates` is exclusive per bot token — two devices long-polling the same token race on the offset and silently lose messages. Solutions exist (multiplex inside the proxy, or switch to webhooks) but require a substantial proxy rewrite and a coordinated re-flash of the mother device — risky to do under deadline pressure.

Deferring. If a third device is added later, or if managing two DM threads becomes friction, revisit. The mini's `TelegramNotifier.h` would need only minor changes at that time (include `message_thread_id` outbound, filter inbound by it).

## Bootstrap Plan

### Step 1 — Create the new repo

- New empty git repo `iot-yc-water-the-flowers-mini` (locally first; GitHub later if desired).
- Single initial commit: copy entire mother project at HEAD, add `MOTHER_PROJECT.md` noting the source SHA so cherry-picks are traceable.
- Preserves git blame for inherited files (e.g., the `WiFi.disconnect(true)` line traces back to the mother commit that added it).

### Step 2 — Strip aggressively (`v0.1.0: strip mother-project complexity`)

Delete the dropped modules, the test firmware env, the test/ directory, the existing UI, and all learning persistence. Update `platformio.ini` to a single ESP32-classic env. The repo should compile (no-op `loop()`) at end of this commit.

### Step 3 — Build new modules (`v0.2.0` … `v0.5.0`, one commit per module)

In order, with native tests written first per the mother project's TDD discipline:
1. `Settings.h` (load/save round-trip, defaults, mutation)
2. `Scheduler.h` (next_run computation, grace window, time-of-day match across DST)
3. `MoistureSensor.h` (read averaging, threshold compare, calibration math)
4. `OverflowSensor.h` (5/7 debounce, latch persistence)
5. `WateringController.h` (state transitions, timeout, overflow interrupt, manual trigger)

### Step 4 — Wire it together (`v0.6.0`)

- `src/main.cpp` orchestration, dual-core task creation
- `api_handlers.h` endpoints
- `data/web/prod/` HTML/JS/CSS
- Telegram command handlers in main.cpp's `checkTelegramCommands()`

### Step 5 — Telegram bot setup

- New BotFather bot, new token in `secret.h`
- Same Cloud.ru proxy URL, same auth token
- Same WiFi credentials

### Step 6 — Cloud.ru one-shot

- Add Prometheus scrape job for the mini's device label
- Import the mini Grafana dashboard

### Step 7 — Bot user guide (`docs/bot-guide.md` in the new repo)

Standalone Markdown document the user can keep open on their phone while away. Sections:
- **Quick start**: bot username, what each topic alert means at a glance.
- **Command reference**: every Telegram command, what it does, what it replies with, when to use it. Grouped by category (control, settings, time, diagnostics).
- **Alert glossary**: every alert message the device can emit (success, skip-wet, stuck-wet warning, timeout, overflow trip, WiFi recovery, OTA reboot), with cause and recommended response.
- **Recovery procedures**: "device is silent" → checklist; "overflow latched" → procedure; "OTA failed, device unreachable" → fallback to USB reflash.
- **Calibration walkthrough**: dry-soil vs wet-soil reference capture, what threshold to set if calibration is unavailable.
- **Don't-do list**: don't run `POST /filesystem` while away (wipes calibration); don't change `/set_runtime` to a tiny number; don't power-cycle while overflow is latched expecting it to clear (it now persists across reboot).

### Step 8 — On-bench validation

- `pio test -e native` — all module tests pass
- Flash → serial log: "boot OK, RTC OK, LittleFS OK, WiFi OK, Telegram OK, metrics OK"
- Manual `/water` → motor runs → soil reading visible in Grafana
- Floor sensor pour test → overflow latches → reboot → confirm latch survived → `/reset_overflow` clears it
- Schedule test: `/set_time` to a minute from now, observe trigger
- OTA test: rebuild, upload via `/firmware`, confirm device reboots with new version
- Filesystem OTA test: `pio run -t buildfs && pio run -t uploadfs` via web — confirm device writes default `/settings.json` and `/state.json` on first boot after fs-flash (the upload wipes existing files)

## Open Questions / TBD Until Decided

- **Specific GPIO pin assignments** — pending the YD-ESP32-23 breakout pinout being confirmed in hand. Constraints captured in the Hardware section.
- **Custom 16 MB partition table layout** — exact `partitions_16mb.csv` (factory app size, two OTA slots, NVS, LittleFS region) defined in the implementation plan; safe defaults: 2× 4 MB OTA app slots, 4 MB LittleFS, rest for NVS/coredump.
- **Soil sensor threshold default** — unknowable without bench measurement. Placeholder until `/calibrate_dry` and `/calibrate_wet` are run.

## Update Path (already covered by reused `ota.h`)

WiFi OTA for both firmware AND filesystem via the auth-gated `/firmware` web page (mother project, v1.25.0):
- `POST /firmware` → `U_FLASH` (firmware `.bin`)
- `POST /filesystem` → `U_SPIFFS` (LittleFS image)

The mini reuses `ota.h` verbatim, so both endpoints come along for free.

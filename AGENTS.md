# Repository Guidelines

## Project Structure & Module Organization
Firmware lives in `src/`: `main.cpp` is the production ESP32 build and `test-main.cpp` is the hardware test firmware. Core logic and device modules are header-centric under `include/` (`StateMachineLogic.h`, `LearningAlgorithm.h`, `WateringSystem.h`, `NetworkManager.h`). Native unit tests live in `test/`, with `test_native_all.cpp` as the main desktop suite. Web assets for LittleFS are in `data/web/prod/`; experimental dashboard pages are in `data/web/test/`. Deployment helpers for the Telegram proxy sit in `deploy/systemd/` and `tools/`.

## Build, Test, and Development Commands
Use PlatformIO from the repo root:

```bash
platformio run -e esp32-s3-devkitc-1            # Build production firmware
platformio run -t upload -e esp32-s3-devkitc-1  # Flash production firmware
platformio run -e esp32-s3-devkitc-1-test       # Build hardware test firmware
platformio run -t buildfs -e esp32-s3-devkitc-1 # Build LittleFS image from data/
platformio run -t uploadfs -e esp32-s3-devkitc-1
pio test -e native                              # Run desktop Unity tests
platformio device monitor -b 115200 --raw       # Serial monitor
```

Rebuild and upload the filesystem whenever you change files under `data/web/prod/`.

## Coding Style & Naming Conventions
Follow the existing Arduino/C++ style: 4-space indentation, opening braces on the same line, and descriptive header comments for non-trivial modules. Keep pure logic in reusable helpers (`StateMachineLogic`, `LearningAlgorithm`) and keep hardware/network side effects out of native-testable code where possible. Use `PascalCase` for types, `camelCase` for functions/variables, and `UPPER_SNAKE_CASE` for macros and configuration constants in `config.h`.

## Testing Guidelines
The repository uses Unity through PlatformIO native tests. Add or update tests in `test/` whenever state transitions, timeout rules, learning calculations, or safety logic change. Prefer extending `test_native_all.cpp` for active coverage; older focused test files are present but marked deprecated. Run `pio test -e native` before opening a PR.

## Version Bumping
Every code change MUST bump the version. Update ALL of these locations:
1. `include/config.h:10` — `VERSION` string (e.g. `"watering_system_1.20.5"`)
2. `CLAUDE.md:8` — `**Version**: 1.20.5 (config.h:10)`

Use the new version in the commit message prefix (e.g. `v1.20.5: ...`). Patch version increments for fixes and small changes; minor version increments for new features.

## Commit & Pull Request Guidelines
Recent history follows short, imperative, version-prefixed subjects such as `v1.19.3: increase tray 2 watering timeout to 35s`. Match that format for release-oriented changes; otherwise keep subjects concise and specific. PRs should describe behavioral impact, affected hardware or web flows, required filesystem upload steps, and the tests run. Include screenshots only for dashboard or OTA UI changes.

## Security & Configuration Tips
Do not commit secrets. Local credentials and tokens belong in `include/secret.h`. Treat LittleFS contents and Telegram/MQTT settings as deployable configuration, and double-check proxy/service files in `deploy/systemd/` before shipping monitoring changes.

## Debugging via Monitoring Logs (Loki)

The live device ships logs (and metrics) to the monitoring box's Loki via the metrics proxy. This is the primary way to debug field behavior without serial access. Reach for it before guessing.

**Tool:** `~/Documents/projects/personal-os/tools/waterlog` (wraps the fleet `mon` ssh; built 2026-06-30). Modes:
- `waterlog summary [days]` — per-tray multiplier / last-fill / cycle-count / timeout table + tank events. **Start here.**
- `waterlog tray N [days]` — every line for Tray N (does the index math for you).
- `waterlog timeouts [days]` / `waterlog recent [min]` / `waterlog grep 'REGEX' [days]`.

Loki labels: `{job="esp32", device="watering-system"}`. (Prometheus metric selector was unverified as of 2026-06-30 — trust the logs.)

**Two traps that will bite you:**
1. **Indexing.** A log line `Valve N` = internal `valveIndex N` = the user's **`Tray N+1`**. Session-tracking lines (`Tray M`) and Telegram alerts use `M = valveIndex+1`. So the user's *Tray 1* is log *Valve 0*. `waterlog tray N` already maps this; the raw logs do not.
2. **Loki 429s on filtered queries.** A server-side `|=`/`|~` filter over a multi-day range splits into hundreds of subqueries → `429 "too many outstanding requests"` (surfaces as a python JSON-decode error in `lokiq logs`). **Pull label-only and filter client-side** — that's exactly what `waterlog` does (label-only query + python regex + retry-on-429). Don't add line filters to long-range server queries.

**The underwatering signature (learning runaway), how to read it:**
- `waterlog summary` → any tray with `mult` >> 1.0× is under-watering (1.0× = 24 h; the cap is `MAX_INTERVAL_MULTIPLIER`). The whole fleet drifting >1.0× = the asymmetric grow-vs-shrink loop; one tray pinned near the cap = that sensor.
- In `waterlog tray N`, the tell is a cycle that **completes `✓ OK` while every `Sensor N-1 GPIO …` line in the trace reads `(DRY)`**, with `learning: fill=…s interval A→B` where B > A and `fill` shrinking over cycles. That means a brief WET flicker (caught by the 100 ms rain check, missed by the 5 s debug log) ended the cycle and was recorded as a real fill → interval grows → tray waters less, shorter, each time. Root cause was an **un-debounced** `readRainSensor`; fixed in v1.28.0 (5-of-7 majority). Manual `/api/water` cycles also feed learning, so a fast false-complete on a manual top-up *raises* the interval.
- TIMEOUT events (`waterlog timeouts`) are the *opposite* state: pump ran the full per-valve window, sensor never wetted → interval decrements. Genuine slow-flow/clog looks like repeated timeouts on one valve.
- Also check tank state: `water level before: X% (empty)` / `WATER LEVEL LOW` blocks watering fleet-wide — a low tank under-waters everything regardless of learning.

## Plan Execution
When a written implementation plan is ready (e.g. under `docs/superpowers/plans/`), execute it via **subagent-driven development** (`superpowers:subagent-driven-development`) by default — fresh subagent per task with review between tasks. Do not ask which execution mode to use unless the user explicitly requests inline execution.

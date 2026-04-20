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

## Plan Execution
When a written implementation plan is ready (e.g. under `docs/superpowers/plans/`), execute it via **subagent-driven development** (`superpowers:subagent-driven-development`) by default — fresh subagent per task with review between tasks. Do not ask which execution mode to use unless the user explicitly requests inline execution.

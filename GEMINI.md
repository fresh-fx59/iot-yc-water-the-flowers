# GEMINI.md

## Project Overview

This is an IoT project for an automatic flower watering system controlled by an ESP32 microcontroller. The system manages 6 valves, 6 rain sensors, and a water pump to automate the watering process.

The core of the project is a C++ application built using the PlatformIO framework. It features a sophisticated watering algorithm that includes a 5-phase watering cycle, a time-based learning algorithm to adapt to each plant's water consumption, and extensive safety features to prevent overwatering.

The system is designed to be highly autonomous and resilient, with features like:
- **Time-Based Learning:** Automatically adjusts watering frequency based on learned water consumption rates for each plant.
- **Safety Systems:** Multiple layers of protection, including a master overflow sensor, watering timeouts, and an emergency halt mode.
- **Remote Control and Monitoring:** Integrates with Telegram for notifications and debugging, and uses MQTT for remote control and status reporting.
- **Web Interface:** A web-based UI for manual control and over-the-air (OTA) firmware updates.

## Building and Running

The project uses PlatformIO for building, deploying, and managing the firmware. There are two separate build environments: one for production and one for testing.

### Production

- **Source File:** `src/main.cpp`
- **Build Command:** `platformio run -e esp32-s3-devkitc-1`
- **Upload Command:** `platformio run -t upload -e esp32-s3-devkitc-1`
- **Monitor Command:** `platformio device monitor -b 115200 --raw`

### Testing

- **Source File:** `src/test-main.cpp`
- **Build Command:** `platformio run -e esp32-s3-devkitc-1-test`
- **Upload Command:** `platformio run -t upload -e esp32-s3-devkitc-1-test`
- **Monitor Command:** `platformio device monitor -b 115200 --raw`

### Filesystem

The project uses LittleFS to store web interface files and learning data.

- **Build Filesystem:** `platformio run -t buildfs -e esp32-s3-devkitc-1`
- **Upload Filesystem:** `platformio run -t uploadfs -e esp32-s3-devkitc-1`

A full deployment involves cleaning the build, building and uploading the filesystem, and then building and uploading the firmware.

## Development Conventions

- **Configuration:** Secret keys and sensitive information (WiFi credentials, API tokens) are stored in `include/secret.h`, which should not be committed to version control.
- **Environments:** The project maintains a strict separation between production and test code. `src/main.cpp` is for the production firmware, while `src/test-main.cpp` is for a hardware testing and debugging firmware.
- **MQTT API:** The system is controlled via MQTT commands. The available commands are documented in the `README.md` file.
- **Web Interface:** The web interface for production is located in `data/web/prod`, and the test dashboard is in `data/web/test`.

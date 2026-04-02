/**
 * Smart Watering System - Main Entry Point
 * ESP32-S3-N8R2
 * Version: 1.15.0 - Multi-threaded Safety Architecture
 *
 * Controls 6 valves, 6 rain sensors, 1 water pump, and master overflow sensor
 * Features time-based learning algorithm with automatic watering
 * Persists learning data to flash storage
 * Uses DS3231 RTC as source of truth for time
 * Master overflow sensor (GPIO 42) provides emergency water overflow detection
 * 10-second boot countdown for emergency firmware updates
 *
 * ARCHITECTURE:
 * - Core 1 (main loop): Watering control @ 100Hz (time-critical, never blocks)
 * - Core 0 (network task): WiFi/MQTT/Telegram/OTA @ 2Hz (can timeout without affecting watering)
 * - Network issues cannot cause sensor monitoring failures or overflow
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>

// Project headers
#include <config.h>
#include <DS3231RTC.h>
#include <ValveController.h>
#include <WateringSystem.h>
#include <NetworkManager.h>
#include <TelegramNotifier.h>
#include <DebugHelper.h>
#include <api_handlers.h>
#include <ota.h>

// ============================================
// Global Objects
// ============================================
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);
WateringSystem wateringSystem;
int lastUpdateId = 0; // Tracks the last processed Telegram update ID to avoid reprocessing old messages.

// ============================================
// Multi-threading for Safety-Critical Operations
// Core 0: Network operations (can block/timeout without affecting watering)
// Core 1: Watering control (time-critical, never blocks)
// ============================================
TaskHandle_t networkTaskHandle = NULL;

// Forward declarations
void checkTelegramCommands(int timeout = 10);
void loopOta();

// Network operations task - runs independently on Core 0
void networkTask(void* parameter) {
    DebugHelper::debug("🧵 Network task started on Core " + String(xPortGetCoreID()));

    while (true) {
        // Always serve local web/API requests first. This must stay responsive even
        // when internet services (Telegram/MQTT) are unavailable.
        loopOta();

        // Keep WiFi state machine running regardless of halt mode.
        NetworkManager::loopWiFi();

        if (NetworkManager::isWiFiConnected()) {
            TelegramNotifier::ensureBotCommandsRegistered();

            // Keep Telegram command handling available in both normal and halt mode.
            checkTelegramCommands(0);
            wateringSystem.processPendingNotifications();
            DebugHelper::loop();

            // MQTT traffic is disabled during halt mode, but local HTTP stays active.
            if (!wateringSystem.isHaltMode()) {
                NetworkManager::loopMQTT();
                wateringSystem.publishPendingMQTTState();
            }
        }

        // Poll quickly so local API/UI and OTA remain responsive.
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// ============================================
// NTP Time Sync Helper
// Syncs RTC with NTP server (Moscow timezone: UTC+3)
// ============================================
bool syncTimeFromNTP() {
    DebugHelper::debug("🌐 Syncing time from NTP...");

    // Moscow timezone: UTC+3, no DST
    const long gmtOffset_sec = 3 * 3600;
    const int daylightOffset_sec = 0;

    // Configure NTP (pool.ntp.org with fallbacks)
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

    // Wait for time to be set (max 10 seconds)
    int retries = 0;
    while (retries < 20) {
        time_t now = time(nullptr);
        if (now > 1640000000) { // Jan 2022 - sanity check
            // Time successfully obtained from NTP
            struct tm *timeinfo = localtime(&now);
            char buffer[30];
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);

            DebugHelper::debug("✓ NTP time obtained: " + String(buffer));

            // Set RTC from NTP time
            DS3231RTC::setTime(now);

            // Re-sync ESP32 system time from RTC (ensures consistency)
            DS3231RTC::setSystemTimeFromRTC();

            DebugHelper::debug("✓ RTC synchronized with NTP");
            return true;
        }
        delay(500);
        retries++;
    }

    DebugHelper::debugImportant("❌ NTP sync timeout - check internet connection");
    return false;
}

// ============================================
// Telegram Command Handler
// Processes incoming Telegram commands like /halt and /resume.
// timeout: Long polling timeout in seconds (0 for immediate check)
// ============================================
void checkTelegramCommands(int timeout) {
    if (!NetworkManager::isWiFiConnected()) {
        return;
    }
    // networkTask runs every 100ms; polling Telegram each tick causes excessive
    // TLS reconnect churn and noisy ssl_client ERR:9/(-76) logs on ESP32.
    // Keep command responsiveness while limiting connection churn.
    static unsigned long lastTelegramPollMs = 0;
    if (timeout <= 0) {
        unsigned long now = millis();
        if (now - lastTelegramPollMs < TELEGRAM_COMMAND_POLL_INTERVAL_MS) {
            return;
        }
        lastTelegramPollMs = now;
    }

    String command = TelegramNotifier::checkForCommands(lastUpdateId, timeout);

    if (command == "/help" || command == "help" ||
        command == "/start" || command == "start") {
        DebugHelper::debugImportant("📘 HELP command received!");
        DebugHelper::flushBuffer();
        sendTelegramDebug(TelegramNotifier::getHelpMessage());
    } else if (command == "/halt" || command == "halt") {
        if (!wateringSystem.isHaltMode()) {
            DebugHelper::debugImportant("🛑 HALT command received!");
            wateringSystem.setHaltMode(true);

            // Send confirmation
            String haltMessage = "🛑 <b>HALT MODE ACTIVATED</b>\n\n";
            haltMessage += "• All watering operations BLOCKED\n";
            haltMessage += "• System ready for firmware update\n";
            haltMessage += "• OTA: http://" + WiFi.localIP().toString() + "/firmware\n";
            haltMessage += "• Send /resume to exit halt mode";

            DebugHelper::flushBuffer();
            sendTelegramDebug(haltMessage);
        }
    } else if (command == "/resume" || command == "resume") {
        if (wateringSystem.isHaltMode()) {
            DebugHelper::debugImportant("▶️ RESUME command received!");
            wateringSystem.setHaltMode(false);

            // Send confirmation
            String resumeMessage = "▶️ <b>SYSTEM RESUMED</b>\n\n";
            resumeMessage += "• Normal operations restored.\n";
            resumeMessage += "• Send /halt to re-enter halt mode.";

            DebugHelper::flushBuffer();
            sendTelegramDebug(resumeMessage);
        }
    } else if (command == "/time" || command == "time") {
        // Display current time from RTC
        float temp = DS3231RTC::getTemperature();
        float battery = DS3231RTC::getBatteryVoltage();

        String timeMessage = "⏰ <b>Current Time</b>\n\n";
        timeMessage += "📅 " + TelegramNotifier::getCurrentDateTime() + "\n";
        timeMessage += "🌡️ RTC Temp: " + String(temp, 2) + " °C\n";
        timeMessage += "🔋 Battery: " + String(battery, 3) + " V";

        if (battery < 2.5) {
            timeMessage += " ⚠️ LOW!";
        }

        timeMessage += "\n\n" + wateringSystem.getPlantLightStatusMessage();
        timeMessage += "\n\n💡 Use /settime to update";

        sendTelegramDebug(timeMessage);
    } else if (command == "/settime" || command == "settime" ||
               command.startsWith("/settime ") || command.startsWith("settime ")) {

        // Extract time string (if provided)
        String timeStr = command;
        timeStr.replace("/settime", "");
        timeStr.replace("settime", "");
        timeStr.trim();

        // AUTO MODE: No arguments provided - sync from NTP
        if (timeStr.length() == 0) {
            DebugHelper::flushBuffer();

            String syncingMessage = "🌐 <b>Auto Time Sync</b>\n\n";
            syncingMessage += "⏳ Connecting to NTP servers...\n";
            syncingMessage += "🌍 Timezone: Moscow (UTC+3)";
            sendTelegramDebug(syncingMessage);

            // Attempt NTP sync
            if (syncTimeFromNTP()) {
                String successMessage = "✅ <b>TIME AUTO-SYNCED</b>\n\n";
                successMessage += "⏰ Current time: " + TelegramNotifier::getCurrentDateTime() + "\n";
                successMessage += "🌐 Source: NTP (pool.ntp.org)\n";
                successMessage += "🔧 RTC and system time synchronized\n\n";
                successMessage += "💡 To set manually: /settime YYYY-MM-DD HH:MM:SS";

                DebugHelper::flushBuffer();
                sendTelegramDebug(successMessage);
            } else {
                String errorMessage = "❌ <b>NTP Sync Failed</b>\n\n";
                errorMessage += "⚠️ Could not reach NTP servers\n";
                errorMessage += "🔍 Check:\n";
                errorMessage += "  • Internet connection\n";
                errorMessage += "  • WiFi signal strength\n";
                errorMessage += "  • Router firewall (port 123)\n\n";
                errorMessage += "💡 Try manual: /settime YYYY-MM-DD HH:MM:SS\n";
                errorMessage += "Example: /settime 2026-01-12 14:30:00";

                DebugHelper::flushBuffer();
                sendTelegramDebug(errorMessage);
            }
        }
        // MANUAL MODE: User provided date/time
        else {
            // Parse datetime: YYYY-MM-DD HH:MM:SS
            int year, month, day, hour, minute, second;
            if (sscanf(timeStr.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
                // Validate ranges
                if (year >= 2000 && year <= 2099 && month >= 1 && month <= 12 &&
                    day >= 1 && day <= 31 && hour >= 0 && hour <= 23 &&
                    minute >= 0 && minute <= 59 && second >= 0 && second <= 59) {

                    // Calculate day of week (1=Sunday, 7=Saturday)
                    // Using Zeller's congruence simplified
                    int y = year;
                    int m = month;
                    if (m < 3) {
                        m += 12;
                        y--;
                    }
                    int dayOfWeek = (day + (13 * (m + 1)) / 5 + y + y / 4 - y / 100 + y / 400) % 7;
                    dayOfWeek = (dayOfWeek + 6) % 7 + 1; // Convert to 1-7 (Sunday=1)

                    // Set RTC time
                    DS3231RTC::setTime(second, minute, hour, dayOfWeek, day, month, year - 2000);

                    // Update ESP32 system time from RTC
                    DS3231RTC::setSystemTimeFromRTC();

                    String successMessage = "✅ <b>TIME MANUALLY SET</b>\n\n";
                    successMessage += "⏰ New time: " + TelegramNotifier::getCurrentDateTime() + "\n";
                    successMessage += "📅 Day of week: " + String(dayOfWeek) + "\n";
                    successMessage += "🔧 RTC and system time synchronized";

                    DebugHelper::flushBuffer();
                    sendTelegramDebug(successMessage);
                    DebugHelper::debugImportant("✓ RTC time manually set to: " + timeStr);
                } else {
                    String errorMessage = "❌ <b>Invalid date/time values</b>\n\n";
                    errorMessage += "Valid ranges:\n";
                    errorMessage += "• Year: 2000-2099\n";
                    errorMessage += "• Month: 1-12\n";
                    errorMessage += "• Day: 1-31\n";
                    errorMessage += "• Hour: 0-23\n";
                    errorMessage += "• Minute: 0-59\n";
                    errorMessage += "• Second: 0-59";
                    sendTelegramDebug(errorMessage);
                }
            } else {
                String errorMessage = "❌ <b>Invalid time format</b>\n\n";
                errorMessage += "Usage:\n";
                errorMessage += "• Auto-sync: /settime\n";
                errorMessage += "• Manual: /settime YYYY-MM-DD HH:MM:SS\n\n";
                errorMessage += "Example: /settime 2026-01-12 14:30:00";
                sendTelegramDebug(errorMessage);
            }
        }
    } else if (command == "/reset_overflow" || command == "reset_overflow") {
        DebugHelper::debugImportant("🔄 RESET OVERFLOW command received!");
        wateringSystem.resetOverflowFlag();

        // Send confirmation
        String message = "✅ <b>OVERFLOW FLAG RESET</b>\n\n";
        message += "• Emergency stop cleared\n";
        message += "• GPIO hardware reinitialized\n";
        message += "• System ready to resume watering\n\n";
        message += "💡 Auto-watering will resume when trays are empty";

        DebugHelper::flushBuffer();
        sendTelegramDebug(message);
    } else if (command == "/reinit_gpio" || command == "reinit_gpio") {
        DebugHelper::debugImportant("🔧 REINIT GPIO command received!");
        wateringSystem.reinitializeGPIOHardware();

        // Send confirmation
        String message = "✅ <b>GPIO HARDWARE REINITIALIZED</b>\n\n";
        message += "• All valve pins reinitialized\n";
        message += "• Pump pin reinitialized\n";
        message += "• Sensor power pin reinitialized\n\n";
        message += "💡 Use this if relay modules are stuck after emergency events";

        DebugHelper::flushBuffer();
        sendTelegramDebug(message);
    } else if (command == "/overflow_status" || command == "overflow_status" ||
               command == "/overflow_sensor" || command == "overflow_sensor") {
        DebugHelper::debugImportant("🔍 OVERFLOW STATUS command received!");
        DebugHelper::flushBuffer();
        sendTelegramDebug(wateringSystem.getOverflowStatusMessage());
    } else if (command == "/lamp" || command == "lamp" ||
               command == "/lamp_status" || command == "lamp_status") {
        sendTelegramDebug(wateringSystem.getPlantLightStatusMessage());
    } else if (command == "/lamp_on" || command == "lamp_on") {
        wateringSystem.setPlantLightManualOn();

        String message = "💡 <b>PLANT LIGHT MANUAL ON</b>\n\n";
        message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
        message += "🔌 Relay GPIO: " + String(PLANT_LIGHT_RELAY_PIN) + "\n";
        message += "🤖 Mode: manual_on\n";
        message += "💡 Auto schedule paused until /lamp_auto";

        DebugHelper::flushBuffer();
        sendTelegramDebug(message);
    } else if (command == "/lamp_off" || command == "lamp_off") {
        wateringSystem.setPlantLightManualOff();

        String message = "🌙 <b>PLANT LIGHT MANUAL OFF</b>\n\n";
        message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
        message += "🔌 Relay GPIO: " + String(PLANT_LIGHT_RELAY_PIN) + "\n";
        message += "🤖 Mode: manual_off\n";
        message += "💡 Auto schedule paused until /lamp_auto";

        DebugHelper::flushBuffer();
        sendTelegramDebug(message);
    } else if (command == "/lamp_auto" || command == "lamp_auto") {
        wateringSystem.setPlantLightAuto();

        String message = "🤖 <b>PLANT LIGHT AUTO MODE</b>\n\n";
        message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
        message += "📅 Schedule: 22:00 -> 07:00\n";
        message += "🔄 Manual override cleared\n\n";
        message += wateringSystem.getPlantLightStatusMessage();

        DebugHelper::flushBuffer();
        sendTelegramDebug(message);
    }
}

// ============================================ 
// DS3231 RTC Initialization
// Professional approach: Set system time once at boot
// ============================================ 
void initializeRTC() {
    DebugHelper::debug("Initializing DS3231 RTC...");

    // Initialize DS3231
    if (!DS3231RTC::init()) {
        DebugHelper::debugImportant("❌ DS3231 initialization failed!");
        DebugHelper::debugImportant("   System will continue but time may be incorrect");
        return;
    }

    // Set ESP32 system time from RTC (ONCE!)
    if (!DS3231RTC::setSystemTimeFromRTC()) {
        DebugHelper::debugImportant("⚠️ Failed to set system time from RTC");
        return;
    }

    // Read additional RTC info
    float temp = DS3231RTC::getTemperature();
    char tempBuffer[20];
    snprintf(tempBuffer, sizeof(tempBuffer), "%.2f °C", temp);
    DebugHelper::debug("✓ DS3231 Temperature: " + String(tempBuffer));

    float battery = DS3231RTC::getBatteryVoltage();
    char batteryBuffer[30];
    snprintf(batteryBuffer, sizeof(batteryBuffer), "%.3f V", battery);
    DebugHelper::debug("✓ DS3231 Battery: " + String(batteryBuffer));

    // Warn if battery is low
    if (battery < 2.5) {
        DebugHelper::debugImportant("⚠️ DS3231 battery low (" + String(batteryBuffer) + ") - replace soon!");
    }
}

// ============================================ 
// API Handler Registration
// ============================================ 
void registerApiHandlers() {
    Serial.println("Registering API handlers...");
    httpServer.on("/api/water", HTTP_GET, handleWaterApi);
    Serial.println("  ✓ Registered /api/water");
    httpServer.on("/api/stop", HTTP_GET, handleStopApi);
    Serial.println("  ✓ Registered /api/stop");
    httpServer.on("/api/status", HTTP_GET, handleStatusApi);
    Serial.println("  ✓ Registered /api/status");
    httpServer.on("/api/lamp", HTTP_GET, handlePlantLightApi);
    Serial.println("  ✓ Registered /api/lamp");
    httpServer.on("/api/reset_calibration", HTTP_GET, handleResetCalibrationApi);
    Serial.println("  ✓ Registered /api/reset_calibration");
}

// ============================================ 
// Boot Countdown for Emergency Halt
// Allows entering halt mode during the initial boot sequence.
// ============================================ 
void bootCountdown() {
    if (!NetworkManager::isWiFiConnected()) {
        DebugHelper::debug("⚠️ WiFi not connected - skipping countdown");
        return;
    }

    // Flush buffered debug messages before sending notification
    DebugHelper::flushBuffer();

    // Send countdown notification
    String message = "🟢 <b>Device Online</b>\n";
    message += "⏰ " + TelegramNotifier::getCurrentDateTime() + "\n";
    message += "📍 IP: " + WiFi.localIP().toString() + "\n";
    message += "📶 WiFi: " + String(WiFi.RSSI()) + " dBm\n";
    message += "🔧 Version: " + String(VERSION) + "\n\n";
    message += "⏱️ <b>Starting in 10 seconds...</b>\n";
    message += "Send /halt to prevent operations and enter firmware update mode";

    DebugHelper::debug("📱 Sending countdown notification...");
    sendTelegramDebug(message);

    // 10-second countdown loop
    unsigned long countdownStart = millis();
    const unsigned long COUNTDOWN_DURATION = 10000; // 10 seconds

    DebugHelper::debug("⏱️ Starting 10-second countdown...");
    DebugHelper::debug("   Send /halt via Telegram to enter firmware update mode");

    while (millis() - countdownStart < COUNTDOWN_DURATION) {
        checkTelegramCommands(0); // Use 0s timeout during boot to avoid blocking the countdown
        if (wateringSystem.isHaltMode()) {
            return; // Exit countdown if halt mode is activated
        }
        delay(500); // Check every 500ms
        yield(); // Feed watchdog
    }

    DebugHelper::debug("✓ Countdown complete - normal operation mode");
}

// ============================================ 
// Setup Function
// ============================================ 
void setup() {
    // Initialize serial
    Serial.begin(DEBUG_SERIAL_BAUDRATE);
    delay(3000);  // Wait for serial monitor
    Serial.println("\n\n\n");
    delay(100);

    // Print banner (queued for Telegram)
    DebugHelper::debug("=================================");
    DebugHelper::debug("🚀 BOOT START");
    DebugHelper::debug("Smart Watering System");
    DebugHelper::debug("Platform: ESP32-S3-N8R2");
    DebugHelper::debug("Version: " + String(VERSION));
    DebugHelper::debug("Device ID: " + DebugHelper::maskCredential(String(YC_DEVICE_ID)));
    DebugHelper::debug("Valves: " + String(NUM_VALVES));
    DebugHelper::debug("=================================");

    // Initialize battery measurement pins
    pinMode(BATTERY_CONTROL_PIN, OUTPUT);
    digitalWrite(BATTERY_CONTROL_PIN, LOW);  // Transistor OFF by default
    pinMode(BATTERY_ADC_PIN, INPUT);

    // Configure ADC for battery measurement
    analogReadResolution(12);        // 12-bit resolution (0-4095)
    analogSetAttenuation(ADC_11db);  // 0-3.3V range

    // Initialize DS3231 RTC (source of truth for time)
    initializeRTC();

    // Initialize LittleFS for data persistence
    DebugHelper::debug("Initializing LittleFS...");
    if (!LittleFS.begin(false)) {
        DebugHelper::debugImportant("⚠️  LittleFS mount failed, formatting...");
        if (!LittleFS.begin(true)) {
            DebugHelper::debugImportant("❌ LittleFS format failed!");
        } else {
            DebugHelper::debug("✓ LittleFS formatted and mounted");
        }
    } else {
        DebugHelper::debug("✓ LittleFS mounted successfully");
    }

    // Initialize watering system (will load learning data from LittleFS)
    wateringSystem.init();

    // Initialize network manager
    NetworkManager::setWateringSystem(&wateringSystem);
    NetworkManager::init();

    // IDEMPOTENT MIGRATION: Delete old learning data file (if exists)
    if (LittleFS.exists(LEARNING_DATA_FILE_OLD)) {
        DebugHelper::debugImportant("🔄 MIGRATION: Deleting old learning data: " + String(LEARNING_DATA_FILE_OLD));
        LittleFS.remove(LEARNING_DATA_FILE_OLD);
    }

    // Load learning data (DS3231 provides time, no WiFi dependency)
    // Note: First boot will show VFS error log when file doesn't exist (harmless)
    if (!wateringSystem.loadLearningData()) {
        DebugHelper::debugImportant("⚠️  No saved learning data found - will calibrate on first watering");
    }

    // Connect to WiFi
    NetworkManager::connectWiFi();

    // Connect to MQTT (if WiFi available)
    if (NetworkManager::isWiFiConnected()) {
        NetworkManager::connectMQTT();
    }

    // CRITICAL: Set watering system reference for web API
    setWateringSystemRef(&wateringSystem);

    // Initialize OTA updates
    setupOta();

    // ============================================
    // BOOT COUNTDOWN: 10-second emergency halt window
    // ============================================
    bootCountdown();

    // ============================================
    // Create Network Task on Core 0
    // ============================================
    // This separates time-critical watering operations (Core 1) from
    // network I/O (Core 0) to prevent WiFi/Telegram/MQTT issues from
    // blocking sensor monitoring and causing overflows.
    DebugHelper::debug("Creating network task on Core 0...");

    xTaskCreatePinnedToCore(
        networkTask,           // Task function
        "NetworkTask",         // Task name for debugging
        8192,                  // Stack size (8KB) - sufficient for network operations
        NULL,                  // Task parameters
        1,                     // Priority (1 = low, watering on Core 1 is default priority)
        &networkTaskHandle,    // Task handle for management
        0                      // Core 0 (Core 1 reserved for main loop/watering)
    );

    if (networkTaskHandle == NULL) {
        DebugHelper::debugImportant("❌ Failed to create network task!");
        DebugHelper::debugImportant("   System will run in single-threaded mode (less safe)");
    } else {
        DebugHelper::debug("✓ Network task created on Core 0");
        DebugHelper::debug("✓ Watering control runs on Core " + String(xPortGetCoreID()) + " (main loop)");
    }

    DebugHelper::debug("Setup completed - starting main loop");
}

// ============================================ 
// Boot Flag for First Loop
// ============================================ 
bool firstLoop = true;

// ============================================ 
// Main Loop
// Continuously monitors the system, processes watering logic, and handles
// Telegram commands, especially during halt mode.
// ============================================ 
void loop() {
    // Halt mode blocks watering logic, but network task continues handling
    // OTA/local web and Telegram command checks.
    if (wateringSystem.isHaltMode()) {
        // Fallback path if network task failed to start.
        if (networkTaskHandle == NULL) {
            checkTelegramCommands(0);
        }
        delay(100);
        return;
    }

    // First loop: Send schedule and smart boot watering (if not in halt mode)
    if (firstLoop) {
        firstLoop = false;

        // Send watering schedule (best-effort, doesn't block watering)
        if (NetworkManager::isWiFiConnected()) {
            wateringSystem.sendWateringSchedule("Startup Schedule");
        }

        // Smart boot watering: only water if needed (NO network dependency)
        // 1. Fresh device (no calibration data) - water to establish baseline
        // 2. OR any valve is overdue (next watering time in past) - catch up after long outage
        // This prevents over-watering during frequent power cycles
        if (wateringSystem.isFirstBoot()) {
            DebugHelper::debugImportant("🚿 First boot detected - starting initial calibration watering");
            wateringSystem.startSequentialWatering("Boot Calibration");
        } else if (wateringSystem.hasOverdueValves()) {
            int overdueValves[NUM_VALVES];
            int overdueCount = wateringSystem.getOverdueValveIndices(overdueValves, NUM_VALVES);

            DebugHelper::debugImportant("🚿 Overdue valves detected - starting catch-up watering for " + String(overdueCount) + " tray(s)");
            wateringSystem.startSequentialWateringCustom(overdueValves, overdueCount, "Boot Catch-up");
        } else {
            DebugHelper::debug("✓ All valves on schedule - auto-watering will handle it");
        }
    }

    // ============================================
    // CRITICAL: Watering Control Loop (Core 1)
    // ============================================
    // This loop runs every 10ms (100Hz) for responsive sensor monitoring.
    // Network operations (WiFi, MQTT, Telegram, OTA) run independently on
    // Core 0 and cannot block this loop, preventing overflow issues.

    wateringSystem.processWateringLoop();

    // Small delay to prevent watchdog issues (10ms = 100Hz loop rate)
    // This ensures sensors are checked every 100ms as designed
    delay(10);
}

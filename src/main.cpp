/**
 * Smart Watering System - Main Entry Point
 * ESP32-S3-N8R2
 * Version: 1.12.1 - Master Overflow Sensor + Emergency Halt Mode
 *
 * Controls 6 valves, 6 rain sensors, 1 water pump, and master overflow sensor
 * Features time-based learning algorithm with automatic watering
 * Persists learning data to flash storage
 * Uses DS3231 RTC as source of truth for time
 * Master overflow sensor (GPIO 42) provides emergency water overflow detection
 * 10-second boot countdown for emergency firmware updates
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
int lastUpdateId = 0; // Telegram update ID

// ============================================
// Telegram Command Handler
// ============================================
void checkTelegramCommands() {
    if (!NetworkManager::isWiFiConnected()) {
        return;
    }

    String command = TelegramNotifier::checkForCommands(lastUpdateId);

    if (command == "/halt" || command == "halt") {
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
    }
}

// ============================================
// DS3231 RTC Initialization
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
}

// ============================================
// Boot Countdown for Emergency Halt
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
        checkTelegramCommands();
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

    DebugHelper::debug("Setup completed - starting main loop");
}

// ============================================
// Boot Flag for First Loop
// ============================================
bool firstLoop = true;

// ============================================
// Main Loop
// ============================================
void loop() {
    // If in halt mode, only check for telegram commands
    if (wateringSystem.isHaltMode()) {
        checkTelegramCommands();
        delay(1000); // Check for commands every second
        return;
    }

    // First loop: Send schedule and smart boot watering (if not in halt mode)
    if (firstLoop && NetworkManager::isWiFiConnected()) {
        firstLoop = false;

        // Send watering schedule
        wateringSystem.sendWateringSchedule("Startup Schedule");

        // Smart boot watering: only water if needed
        // 1. Fresh device (no calibration data) - water to establish baseline
        // 2. OR any valve is overdue (next watering time in past) - catch up after long outage
        // This prevents over-watering during frequent power cycles
        if (wateringSystem.isFirstBoot()) {
            DebugHelper::debugImportant("🚿 First boot detected - starting initial calibration watering");
            wateringSystem.startSequentialWatering();
        } else if (wateringSystem.hasOverdueValves()) {
            DebugHelper::debugImportant("🚿 Overdue valves detected - starting catch-up watering");
            wateringSystem.startSequentialWatering();
        } else {
            DebugHelper::debug("✓ All valves on schedule - auto-watering will handle it");
        }
    }

    // Handle other tasks
    checkTelegramCommands();

    // Check WiFi connection
    if (!NetworkManager::isWiFiConnected()) {
        DebugHelper::debugImportant("⚠️ WiFi disconnected, attempting reconnect...");
        NetworkManager::connectWiFi();
        delay(5000);
        return;
    }

    // Handle MQTT
    NetworkManager::loopMQTT();

    // Process watering logic
    wateringSystem.processWateringLoop();

    // Handle OTA updates
    loopOta();

    // Flush buffered debug messages to Telegram
    DebugHelper::loop();

    // Small delay to prevent watchdog issues
    delay(10);
}

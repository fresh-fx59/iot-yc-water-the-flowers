/**
 * Smart Watering System - Main Entry Point
 * ESP32-S3-N8R2
 * Version: 1.10.4 - DS3231 RTC Integration
 *
 * Controls 6 valves, 6 rain sensors, and 1 water pump
 * Features time-based learning algorithm with automatic watering
 * Persists learning data to flash storage
 * Uses DS3231 RTC as source of truth for time
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

    // Read and display current time
    struct tm timeinfo;
    if (DS3231RTC::getLocalTime(&timeinfo)) {
        char buffer[30];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
        DebugHelper::debug("✓ DS3231 Time: " + String(buffer));

        // Read and display temperature
        float temp = DS3231RTC::getTemperature();
        char tempBuffer[20];
        snprintf(tempBuffer, sizeof(tempBuffer), "%.2f °C", temp);
        DebugHelper::debug("✓ DS3231 Temperature: " + String(tempBuffer));

        // Read and display battery voltage
        float battery = DS3231RTC::getBatteryVoltage();
        char batteryBuffer[30];
        snprintf(batteryBuffer, sizeof(batteryBuffer), "%.3f V", battery);
        DebugHelper::debug("✓ DS3231 Battery: " + String(batteryBuffer));

        // Warn if battery is low
        if (battery < 2.5) {
            DebugHelper::debugImportant("⚠️ DS3231 battery low (" + String(batteryBuffer) + ") - replace soon!");
        }
    } else {
        DebugHelper::debugImportant("⚠️ Failed to read time from DS3231");
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
    DebugHelper::debug("Device ID: " + String(YC_DEVICE_ID));
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
    // First loop: Send notifications and smart boot watering
    if (firstLoop && NetworkManager::isWiFiConnected()) {
        firstLoop = false;
        TelegramNotifier::sendDeviceOnline(VERSION, DEVICE_TYPE);
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

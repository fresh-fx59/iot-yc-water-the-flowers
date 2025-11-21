/**
 * Smart Watering System - Main Entry Point
 * ESP32-S3-DevKitC-1
 * Version: 1.5.0 - Time-Based Learning
 *
 * Controls 6 valves, 6 rain sensors, and 1 water pump
 * Features time-based learning algorithm with automatic watering
 * Persists learning data to flash storage
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <time.h>

// Project headers
#include <config.h>
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
// NTP Time Synchronization
// ============================================
void syncTime() {
    DebugHelper::debug("Synchronizing time with NTP server...");

    // Configure time with NTP server
    // GMT+3 (Moscow time) with daylight saving time offset
    configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

    // Wait for time to be set
    struct tm timeinfo;
    int retries = 0;
    while (!getLocalTime(&timeinfo) && retries < 10) {
        delay(1000);
        retries++;
    }

    if (getLocalTime(&timeinfo)) {
        char buffer[30];
        strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
        DebugHelper::debug("✓ Time synchronized: " + String(buffer));
    } else {
        DebugHelper::debugImportant("⚠️ Time sync failed - will retry on first watering");
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
    DebugHelper::debug("Smart Watering System");
    DebugHelper::debug("Platform: ESP32-S3-DevKitC-1");
    DebugHelper::debug("Version: " + String(VERSION));
    DebugHelper::debug("Device ID: " + String(YC_DEVICE_ID));
    DebugHelper::debug("Valves: " + String(NUM_VALVES));
    DebugHelper::debug("=================================");

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

    // Connect to WiFi
    NetworkManager::connectWiFi();

    if (NetworkManager::isWiFiConnected()) {
        // Synchronize time with NTP
        syncTime();

        // IDEMPOTENT MIGRATION: Check if old learning data file exists
        // If old file exists, delete it to trigger fresh calibration with new file format
        // This only runs once - after migration, old file won't exist
        if (LittleFS.exists("/learning_data.json")) {
            DebugHelper::debugImportant("🔄 MIGRATION: Found old learning data file, deleting for fresh start...");
            LittleFS.remove("/learning_data.json");
        }

        // Load learning data AFTER NTP sync (needs real time for proper timestamp conversion)
        if (!wateringSystem.loadLearningData()) {
            DebugHelper::debugImportant("⚠️  No saved learning data found - will calibrate on first watering");
        }

        // Connect to MQTT
        NetworkManager::connectMQTT();
    } else {
        // No WiFi - load learning data without real time (will use millis fallback)
        DebugHelper::debugImportant("⚠️  Loading learning data without NTP sync");
        if (!wateringSystem.loadLearningData()) {
            DebugHelper::debugImportant("⚠️  No saved learning data found or load failed");
        }
    }

    // CRITICAL: Set watering system reference for web API
    setWateringSystemRef(&wateringSystem);

    // Initialize OTA updates
    setupOta();

    DebugHelper::debug("Setup completed");

    // Send Telegram notifications
    if (NetworkManager::isWiFiConnected()) {
        TelegramNotifier::sendDeviceOnline(VERSION, DEVICE_TYPE);

        // Send watering schedule after startup
        wateringSystem.sendWateringSchedule("Startup Schedule");

        // Auto-water all trays on startup to build learning data
        // This ensures trays get calibrated without manual intervention
        DebugHelper::debugImportant("🚿 Starting automatic watering on boot...");
        wateringSystem.startSequentialWatering();
    }
}

// ============================================
// Main Loop
// ============================================
void loop() {
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

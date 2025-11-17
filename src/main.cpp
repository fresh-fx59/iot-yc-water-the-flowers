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
    DEBUG_SERIAL.println("Synchronizing time with NTP server...");

    // Configure time with NTP server
    // GMT+3 (Moscow time) with daylight saving time offset
    configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

    // Wait for time to be set
    struct tm timeinfo;
    int retries = 0;
    while (!getLocalTime(&timeinfo) && retries < 10) {
        DEBUG_SERIAL.print(".");
        delay(1000);
        retries++;
    }

    if (getLocalTime(&timeinfo)) {
        char buffer[30];
        strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
        DEBUG_SERIAL.println("\n✓ Time synchronized: " + String(buffer));
    } else {
        DEBUG_SERIAL.println("\n⚠️ Time sync failed - will retry on first watering");
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

    // Print banner
    DEBUG_SERIAL.println("=================================");
    DEBUG_SERIAL.println("Smart Watering System");
    DEBUG_SERIAL.println("Platform: ESP32-S3-DevKitC-1");
    DEBUG_SERIAL.println("Version: " + String(VERSION));
    DEBUG_SERIAL.println("Device ID: " + String(YC_DEVICE_ID));
    DEBUG_SERIAL.println("Valves: " + String(NUM_VALVES));
    DEBUG_SERIAL.println("=================================");
    DEBUG_SERIAL.println();

    // Initialize LittleFS for data persistence
    DEBUG_SERIAL.println("Initializing LittleFS...");
    if (!LittleFS.begin(false)) {
        DEBUG_SERIAL.println("⚠️  LittleFS mount failed, formatting...");
        if (!LittleFS.begin(true)) {
            DEBUG_SERIAL.println("❌ LittleFS format failed!");
        } else {
            DEBUG_SERIAL.println("✓ LittleFS formatted and mounted");
        }
    } else {
        DEBUG_SERIAL.println("✓ LittleFS mounted successfully");
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

        // Connect to MQTT
        NetworkManager::connectMQTT();
    }

    // CRITICAL: Set watering system reference for web API
    setWateringSystemRef(&wateringSystem);

    // Initialize OTA updates
    setupOta();

    DEBUG_SERIAL.println("Setup completed\n");

    // Send Telegram online notification
    if (NetworkManager::isWiFiConnected()) {
        TelegramNotifier::sendDeviceOnline(VERSION, DEVICE_TYPE);
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

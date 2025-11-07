/**
 * Smart Watering System - Main Entry Point
 * ESP32-S3-DevKitC-1
 * Version: 1.4.0
 *
 * Controls 6 valves, 6 rain sensors, and 1 water pump
 * Features dynamic learning algorithm for optimal watering
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

// Project headers
#include <config.h>
#include <ValveController.h>
#include <WateringSystem.h>
#include <NetworkManager.h>
#include <api_handlers.h>
#include <ota.h>

// ============================================
// Global Objects
// ============================================
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);
WateringSystem wateringSystem;

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

    // Initialize watering system
    wateringSystem.init();

    // Initialize network manager
    NetworkManager::setWateringSystem(&wateringSystem);
    NetworkManager::init();

    // Connect to WiFi
    NetworkManager::connectWiFi();

    if (NetworkManager::isWiFiConnected()) {
        // Connect to MQTT
        NetworkManager::connectMQTT();
    }

    // CRITICAL: Set watering system reference for web API
    setWateringSystemRef(&wateringSystem);

    // Initialize OTA updates
    setupOta();

    DEBUG_SERIAL.println("Setup completed\n");
}

// ============================================
// Main Loop
// ============================================
void loop() {
    // Check WiFi connection
    if (!NetworkManager::isWiFiConnected()) {
        DEBUG_SERIAL.println("WiFi disconnected, attempting reconnect...");
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

    // Small delay to prevent watchdog issues
    delay(10);
}

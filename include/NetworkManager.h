#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "config.h"
#include "DebugHelper.h"
#include "WateringSystem.h"

// External clients (defined in main.cpp)
extern WiFiClientSecure wifiClient;
extern PubSubClient mqttClient;

// ============================================
// Network Manager Class
// Combines WiFi and MQTT management
// ============================================
class NetworkManager {
private:
    static WateringSystem* wateringSystem;

public:
    // ========== Initialization ==========
    static void setWateringSystem(WateringSystem* ws) {
        wateringSystem = ws;
    }

    static void init() {
        // Configure MQTT client
        wifiClient.setInsecure(); // For development - use proper certificates in production
        mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
        mqttClient.setCallback(messageCallback);
        mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
        mqttClient.setKeepAlive(MQTT_KEEP_ALIVE);
        DebugHelper::debug("Network Manager initialized");
    }

    // ========== WiFi Management ==========
    static void connectWiFi() {
        DebugHelper::debug("Connecting to WiFi: " + String(SSID));
        WiFi.mode(WIFI_STA);
        WiFi.begin(SSID, SSID_PASSWORD);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < WIFI_MAX_RETRY_ATTEMPTS) {
            delay(WIFI_RETRY_DELAY_MS);
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            DebugHelper::debug("✓ WiFi Connected! IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
        } else {
            DebugHelper::debugImportant("❌ WiFi Connection Failed!");
        }
    }

    static bool isWiFiConnected() {
        return WiFi.status() == WL_CONNECTED;
    }

    // ========== MQTT Management ==========
    static void connectMQTT() {
        if (mqttClient.connected()) return;

        DebugHelper::debug("Connecting to Yandex IoT Core as " + String(YC_DEVICE_ID));

        int attempts = 0;
        while (!mqttClient.connected() && attempts < 5) {
            String clientId = "WateringSystem_" + String(YC_DEVICE_ID);
            if (mqttClient.connect(clientId.c_str(), YC_DEVICE_ID, MQTT_PASSWORD)) {
                DebugHelper::debug("✓ MQTT Connected!");

                if (mqttClient.subscribe(COMMAND_TOPIC.c_str())) {
                    DebugHelper::debug("Subscribed to: " + COMMAND_TOPIC);
                } else {
                    DebugHelper::debugImportant("❌ Failed to subscribe to commands");
                }

                publishConnectionEvent();
            } else {
                DebugHelper::debugImportant("❌ MQTT connection failed, rc=" + String(mqttClient.state()) + " retrying in 5 seconds");
                delay(5000);
                attempts++;
            }
        }
    }

    static void loopMQTT() {
        if (mqttClient.connected()) {
            mqttClient.loop();
        } else {
            DebugHelper::debugImportant("⚠️ MQTT disconnected, attempting reconnect...");
            connectMQTT();
        }
    }

    static bool isMQTTConnected() {
        return mqttClient.connected();
    }

private:
    // ========== MQTT Callback ==========
    static void messageCallback(char* topic, byte* payload, unsigned int length) {
        String payloadStr = "";
        for (unsigned int i = 0; i < length; i++) {
            payloadStr += (char)payload[i];
        }

        DebugHelper::debug("MQTT Message received - Topic: " + String(topic) + ", Payload: " + payloadStr);

        processCommand(payloadStr);
    }

    // ========== Command Processing ==========
    static void processCommand(const String& command) {
        if (!wateringSystem) {
            DebugHelper::debugImportant("❌ Error: WateringSystem not initialized");
            return;
        }

        // Only supported command: start_all
        if (command == "start_all") {
            DebugHelper::debugImportant("📡 MQTT Command: Start sequential watering (all valves)");
            wateringSystem->startSequentialWatering();
        }
        else {
            DebugHelper::debug("Unknown MQTT command: " + command + " (only 'start_all' is supported)");
        }
    }

    // ========== Event Publishing ==========
    static void publishConnectionEvent() {
        String connectMessage = String("{\"device_id\":\"") + YC_DEVICE_ID +
                               "\",\"version\":\"" + VERSION +
                               "\",\"type\":\"" + DEVICE_TYPE +
                               "\",\"status\":\"connected\",\"chip\":\"ESP32-S3\"}";
        String connectTopic = STATE_TOPIC + String("/connection");

        if (mqttClient.publish(connectTopic.c_str(), connectMessage.c_str())) {
            DebugHelper::debug("Published connection event");
        } else {
            DebugHelper::debugImportant("❌ Failed to publish connection event");
        }
    }
};

// Static member initialization
WateringSystem* NetworkManager::wateringSystem = nullptr;

#endif // NETWORK_MANAGER_H

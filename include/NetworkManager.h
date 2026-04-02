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
    static unsigned long mqttDisconnectedSince;   // 0 = connected, >0 = millis() when disconnect detected
    static bool mqttLongOutageNotified;            // true if 10min outage notification was sent
    static bool mqttSilentReconnect;               // suppress Telegram during short outage reconnects

    // WiFi outage tracking (v1.17.3)
    static unsigned long wifiDisconnectedSince;    // 0 = connected, >0 = millis() when disconnect detected
    static bool wifiLongOutageNotified;            // true if 1min outage notification was sent
    static unsigned long lastWifiReconnectAttempt;  // millis() of last reconnect attempt
    static unsigned long wifiReconnectBackoffMs;   // current backoff interval (grows exponentially)

    // MQTT reconnection backoff (v1.17.3)
    static unsigned long lastMqttReconnectAttempt;  // millis() of last reconnect attempt
    static unsigned long mqttReconnectBackoffMs;   // current backoff interval (grows exponentially)

public:
    // ========== Initialization ==========
    static void setWateringSystem(WateringSystem* ws) {
        wateringSystem = ws;
    }

    static void init() {
        // Configure MQTT client
        wifiClient.setInsecure(); // For development - use proper certificates in production
        wifiClient.setTimeout(5);  // 5 second TLS handshake timeout (default 30s blocks Core 0 too long)
        mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
        mqttClient.setCallback(messageCallback);
        mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
        mqttClient.setKeepAlive(MQTT_KEEP_ALIVE);
        mqttClient.setSocketTimeout(5);  // 5 second socket timeout for MQTT operations
        DebugHelper::debug("Network Manager initialized");
    }

    // ========== WiFi Management ==========
    static void connectWiFi() {
        DebugHelper::debug("Connecting to WiFi: " + DebugHelper::maskCredential(String(SSID)));
        WiFi.mode(WIFI_STA);
        WiFi.begin(SSID, SSID_PASSWORD);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < WIFI_MAX_RETRY_ATTEMPTS) {
            delay(WIFI_RETRY_DELAY_MS);
            yield();  // Feed watchdog
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setAutoReconnect(true);
            DebugHelper::debug("✓ WiFi Connected! IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
        } else {
            DebugHelper::debugImportant("❌ WiFi Connection Failed!");
        }
    }

    static bool isWiFiConnected() {
        return WiFi.status() == WL_CONNECTED;
    }

    // ========== WiFi Reconnection with Backoff (v1.17.3) ==========
    // Call from Core 0 networkTask. Handles reconnection with exponential backoff
    // and WiFi.disconnect(true) cleanup to prevent driver corruption.
    static void loopWiFi() {
        if (WiFi.status() == WL_CONNECTED) {
            // WiFi is connected — check if we just recovered from an outage
            if (wifiDisconnectedSince > 0) {
                unsigned long outageDuration = millis() - wifiDisconnectedSince;
                unsigned long minutes = outageDuration / 60000;
                unsigned long seconds = (outageDuration / 1000) % 60;
                DebugHelper::debugImportant("✓ WiFi reconnected after " + String(minutes) + "m " + String(seconds) + "s outage, IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
                // Reset all tracking
                wifiDisconnectedSince = 0;
                wifiLongOutageNotified = false;
                wifiReconnectBackoffMs = WIFI_RECONNECT_BACKOFF_INITIAL_MS;
            }
            return;
        }

        // WiFi is disconnected
        unsigned long now = millis();

        if (wifiDisconnectedSince == 0) {
            // First detection of disconnect
            wifiDisconnectedSince = now;
            if (wifiDisconnectedSince == 0) wifiDisconnectedSince = 1;  // avoid 0 (means "connected")
            Serial.println("⚠️ WiFi disconnected, will attempt reconnect with backoff");
        }

        // Check if we should notify about long outage
        if (!wifiLongOutageNotified) {
            unsigned long outageDuration = now - wifiDisconnectedSince;
            if (outageDuration >= WIFI_OUTAGE_NOTIFY_THRESHOLD_MS) {
                unsigned long minutes = outageDuration / 60000;
                // Can only log to Serial since WiFi is down
                Serial.println("⚠️ WiFi disconnected for " + String(minutes) + " minutes, still trying to reconnect...");
                wifiLongOutageNotified = true;
            }
        }

        // Check if backoff period has elapsed
        if (now - lastWifiReconnectAttempt < wifiReconnectBackoffMs) {
            return;  // Not time yet
        }

        lastWifiReconnectAttempt = now;

        // CRITICAL: Disconnect fully before reconnecting to reset WiFi driver state
        Serial.println("🔄 WiFi reconnect attempt (backoff: " + String(wifiReconnectBackoffMs / 1000) + "s)");
        WiFi.disconnect(true);  // true = turn off WiFi radio completely
        delay(100);             // Brief pause for driver cleanup

        WiFi.mode(WIFI_STA);
        WiFi.begin(SSID, SSID_PASSWORD);

        // Short blocking wait: WIFI_RECONNECT_MAX_ATTEMPTS retries x 500ms = 2.5s
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < WIFI_RECONNECT_MAX_ATTEMPTS) {
            delay(WIFI_RETRY_DELAY_MS);
            yield();
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setAutoReconnect(true);
            // Recovery will be logged on next loopWiFi() call (wifiDisconnectedSince > 0)
        } else {
            // Increase backoff for next attempt (exponential, capped)
            wifiReconnectBackoffMs = min(wifiReconnectBackoffMs * 2, WIFI_RECONNECT_BACKOFF_MAX_MS);
            Serial.println("❌ WiFi reconnect failed, next attempt in " + String(wifiReconnectBackoffMs / 1000) + "s");
        }
    }

    // ========== MQTT Management ==========

    // Single-attempt MQTT connect (v1.17.3: no retry loop, backoff handled by loopMQTT)
    // Blocks max ~5s (TLS timeout) instead of old 175s (5 attempts × 35s)
    static void connectMQTT() {
        if (mqttClient.connected()) return;

        if (!mqttSilentReconnect) {
            DebugHelper::debug("Connecting to Yandex IoT Core as " + DebugHelper::maskCredential(String(YC_DEVICE_ID)));
        }

        yield();  // Feed watchdog
        String clientId = "WateringSystem_" + String(YC_DEVICE_ID);
        if (mqttClient.connect(clientId.c_str(), YC_DEVICE_ID, MQTT_PASSWORD)) {
            if (!mqttSilentReconnect) {
                DebugHelper::debug("✓ MQTT Connected!");
            }

            if (mqttClient.subscribe(COMMAND_TOPIC.c_str())) {
                if (!mqttSilentReconnect) {
                    DebugHelper::debug("Subscribed to: " + COMMAND_TOPIC);
                }
            } else {
                DebugHelper::debugImportant("❌ Failed to subscribe to commands");
            }

            publishConnectionEvent();
            // Reset backoff on success
            mqttReconnectBackoffMs = MQTT_RECONNECT_BACKOFF_INITIAL_MS;
        } else {
            if (!mqttSilentReconnect) {
                DebugHelper::debugImportant("❌ MQTT connection failed, rc=" + String(mqttClient.state()));
            }
        }
    }

    static void loopMQTT() {
        if (mqttClient.connected()) {
            mqttClient.loop();
            // Check if we just reconnected after an outage
            if (mqttDisconnectedSince > 0) {
                unsigned long outageDuration = millis() - mqttDisconnectedSince;
                if (outageDuration >= MQTT_OUTAGE_NOTIFY_THRESHOLD_MS) {
                    unsigned long minutes = outageDuration / 60000;
                    unsigned long seconds = (outageDuration / 1000) % 60;
                    DebugHelper::debugImportant("✓ MQTT reconnected after " + String(minutes) + "m " + String(seconds) + "s outage");
                }
                mqttDisconnectedSince = 0;
                mqttLongOutageNotified = false;
                mqttSilentReconnect = false;
            }
        } else {
            unsigned long now = millis();
            if (mqttDisconnectedSince == 0) {
                // First detection of disconnect
                mqttDisconnectedSince = now;
                if (mqttDisconnectedSince == 0) mqttDisconnectedSince = 1;  // avoid 0 (means "connected")
            } else if (!mqttLongOutageNotified) {
                unsigned long outageDuration = now - mqttDisconnectedSince;
                if (outageDuration >= MQTT_OUTAGE_NOTIFY_THRESHOLD_MS) {
                    unsigned long minutes = outageDuration / 60000;
                    DebugHelper::debugImportant("⚠️ MQTT disconnected for " + String(minutes) + " minutes, still trying to reconnect...");
                    mqttLongOutageNotified = true;
                }
            }
            // Suppress Telegram for short outage reconnect attempts
            mqttSilentReconnect = (now - mqttDisconnectedSince) < MQTT_OUTAGE_NOTIFY_THRESHOLD_MS;

            // Exponential backoff between MQTT reconnection attempts (v1.17.3)
            if (now - lastMqttReconnectAttempt >= mqttReconnectBackoffMs) {
                lastMqttReconnectAttempt = now;
                connectMQTT();
                // Increase backoff for next attempt if still disconnected
                if (!mqttClient.connected()) {
                    mqttReconnectBackoffMs = min(mqttReconnectBackoffMs * 2, MQTT_RECONNECT_BACKOFF_MAX_MS);
                }
            }
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

        // Start sequential watering
        if (command == "start_all") {
            DebugHelper::debugImportant("📡 MQTT Command: Start sequential watering (all valves)");
            wateringSystem->startSequentialWatering();
        }
        // Halt mode - block all watering
        else if (command == "halt" || command == "/halt") {
            DebugHelper::debugImportant("📡 MQTT Command: HALT MODE activated");
            wateringSystem->setHaltMode(true);
        }
        // Resume - exit halt mode
        else if (command == "resume" || command == "/resume") {
            DebugHelper::debugImportant("📡 MQTT Command: Resuming normal operations");
            wateringSystem->setHaltMode(false);
        }
        // Test all sensors
        else if (command == "test_sensors") {
            DebugHelper::debugImportant("📡 MQTT Command: Testing all sensors");
            wateringSystem->testAllSensors();
        }
        // Test specific sensor (format: test_sensor_N where N=0-5)
        else if (command.startsWith("test_sensor_")) {
            int valveIndex = command.substring(12).toInt(); // Extract number after "test_sensor_"
            DebugHelper::debugImportant("📡 MQTT Command: Testing sensor " + String(valveIndex));
            wateringSystem->testSensor(valveIndex);
        }
        // Reset overflow flag
        else if (command == "reset_overflow" || command == "/reset_overflow") {
            DebugHelper::debugImportant("📡 MQTT Command: Resetting overflow flag");
            wateringSystem->resetOverflowFlag();
        }
        // Reinitialize GPIO hardware (for stuck relays)
        else if (command == "reinit_gpio" || command == "/reinit_gpio") {
            DebugHelper::debugImportant("📡 MQTT Command: Reinitializing GPIO hardware");
            wateringSystem->reinitializeGPIOHardware();
        }
        else if (command == "overflow_status" || command == "/overflow_status" ||
                 command == "overflow_sensor" || command == "/overflow_sensor") {
            DebugHelper::debugImportant("📡 MQTT Command: Reading overflow sensor status");
            sendTelegramDebug(wateringSystem->getOverflowStatusMessage());
        }
        else {
            DebugHelper::debug("Unknown MQTT command: " + command);
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
            if (!mqttSilentReconnect) {
                DebugHelper::debug("Published connection event");
            }
        } else {
            if (!mqttSilentReconnect) {
                DebugHelper::debugImportant("❌ Failed to publish connection event");
            }
        }
    }
};

// Static member initialization
WateringSystem* NetworkManager::wateringSystem = nullptr;
unsigned long NetworkManager::mqttDisconnectedSince = 0;
bool NetworkManager::mqttLongOutageNotified = false;
bool NetworkManager::mqttSilentReconnect = false;
unsigned long NetworkManager::lastMqttReconnectAttempt = 0;
unsigned long NetworkManager::mqttReconnectBackoffMs = MQTT_RECONNECT_BACKOFF_INITIAL_MS;
unsigned long NetworkManager::wifiDisconnectedSince = 0;
bool NetworkManager::wifiLongOutageNotified = false;
unsigned long NetworkManager::lastWifiReconnectAttempt = 0;
unsigned long NetworkManager::wifiReconnectBackoffMs = WIFI_RECONNECT_BACKOFF_INITIAL_MS;

#endif // NETWORK_MANAGER_H

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "config.h"
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
        DEBUG_SERIAL.println("Network Manager initialized");
    }

    // ========== WiFi Management ==========
    static void connectWiFi() {
        DEBUG_SERIAL.print("Connecting to WiFi: ");
        DEBUG_SERIAL.println(SSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(SSID, SSID_PASSWORD);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < WIFI_MAX_RETRY_ATTEMPTS) {
            delay(WIFI_RETRY_DELAY_MS);
            DEBUG_SERIAL.print(".");
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            DEBUG_SERIAL.println("\nWiFi Connected!");
            DEBUG_SERIAL.print("IP Address: ");
            DEBUG_SERIAL.println(WiFi.localIP());
            DEBUG_SERIAL.print("Signal Strength (RSSI): ");
            DEBUG_SERIAL.print(WiFi.RSSI());
            DEBUG_SERIAL.println(" dBm");
        } else {
            DEBUG_SERIAL.println("\nWiFi Connection Failed!");
        }
    }

    static bool isWiFiConnected() {
        return WiFi.status() == WL_CONNECTED;
    }

    // ========== MQTT Management ==========
    static void connectMQTT() {
        if (mqttClient.connected()) return;

        DEBUG_SERIAL.print("Connecting to Yandex IoT Core as ");
        DEBUG_SERIAL.println(YC_DEVICE_ID);

        int attempts = 0;
        while (!mqttClient.connected() && attempts < 5) {
            String clientId = "WateringSystem_" + String(YC_DEVICE_ID);
            if (mqttClient.connect(clientId.c_str(), YC_DEVICE_ID, MQTT_PASSWORD)) {
                DEBUG_SERIAL.println("MQTT Connected!");

                if (mqttClient.subscribe(COMMAND_TOPIC.c_str())) {
                    DEBUG_SERIAL.println("Subscribed to: " + COMMAND_TOPIC);
                } else {
                    DEBUG_SERIAL.println("Failed to subscribe to commands");
                }

                publishConnectionEvent();
            } else {
                DEBUG_SERIAL.print("MQTT connection failed, rc=");
                DEBUG_SERIAL.print(mqttClient.state());
                DEBUG_SERIAL.println(" retrying in 5 seconds");
                delay(5000);
                attempts++;
            }
        }
    }

    static void loopMQTT() {
        if (mqttClient.connected()) {
            mqttClient.loop();
        } else {
            DEBUG_SERIAL.println("MQTT disconnected, attempting reconnect...");
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

        DEBUG_SERIAL.println("MQTT Message received:");
        DEBUG_SERIAL.println("Topic: " + String(topic));
        DEBUG_SERIAL.println("Payload: " + payloadStr);

        processCommand(payloadStr);
    }

    // ========== Command Processing ==========
    static void processCommand(const String& command) {
        if (!wateringSystem) {
            DEBUG_SERIAL.println("Error: WateringSystem not initialized");
            return;
        }

        // Basic watering commands
        if (command.startsWith("start_valve_")) {
            int valveIndex = command.substring(12).toInt();
            DEBUG_SERIAL.println("Command: Start watering valve " + String(valveIndex));
            wateringSystem->startWatering(valveIndex);
        }
        else if (command.startsWith("stop_valve_")) {
            int valveIndex = command.substring(11).toInt();
            DEBUG_SERIAL.println("Command: Stop watering valve " + String(valveIndex));
            wateringSystem->stopWatering(valveIndex);
        }
        else if (command == "start_all") {
            DEBUG_SERIAL.println("Command: Start sequential watering (all valves)");
            wateringSystem->startSequentialWatering();
        }
        else if (command.startsWith("start_sequence_")) {
            handleSequenceCommand(command);
        }
        else if (command == "stop_all") {
            DEBUG_SERIAL.println("Command: Stop all watering");
            wateringSystem->stopSequentialWatering();
            for (int i = 0; i < NUM_VALVES; i++) {
                wateringSystem->stopWatering(i);
            }
        }
        else if (command == "state") {
            DEBUG_SERIAL.println("Command: Publish state");
            wateringSystem->publishCurrentState();
        }
        else if (command.startsWith("clear_timeout_")) {
            int valveIndex = command.substring(14).toInt();
            DEBUG_SERIAL.println("Command: Clear timeout flag for valve " + String(valveIndex));
            wateringSystem->clearTimeoutFlag(valveIndex);
        }
        // Learning algorithm commands
        else if (command.startsWith("reset_calibration_")) {
            int valveIndex = command.substring(18).toInt();
            DEBUG_SERIAL.println("Command: Reset calibration for valve " + String(valveIndex));
            wateringSystem->resetCalibration(valveIndex);
        }
        else if (command == "reset_all_calibrations") {
            DEBUG_SERIAL.println("Command: Reset all calibrations");
            wateringSystem->resetAllCalibrations();
        }
        else if (command == "learning_status") {
            DEBUG_SERIAL.println("Command: Print learning status");
            wateringSystem->printLearningStatus();
        }
        else if (command.startsWith("set_skip_cycles_")) {
            handleSkipCyclesCommand(command);
        }
        else {
            DEBUG_SERIAL.println("Unknown command: " + command);
        }
    }

    // ========== Command Helpers ==========
    static void handleSequenceCommand(const String& command) {
        // Parse comma-separated valve indices: start_sequence_0,2,4
        String valveList = command.substring(15);
        int valveIndices[NUM_VALVES];
        int count = 0;

        int startPos = 0;
        int commaPos = valveList.indexOf(',');

        while (commaPos != -1 && count < NUM_VALVES) {
            String valveStr = valveList.substring(startPos, commaPos);
            valveIndices[count++] = valveStr.toInt();
            startPos = commaPos + 1;
            commaPos = valveList.indexOf(',', startPos);
        }

        // Get last valve
        if (startPos < valveList.length() && count < NUM_VALVES) {
            String valveStr = valveList.substring(startPos);
            valveIndices[count++] = valveStr.toInt();
        }

        DEBUG_SERIAL.println("Command: Start sequential watering (custom sequence)");
        wateringSystem->startSequentialWateringCustom(valveIndices, count);
    }

    static void handleSkipCyclesCommand(const String& command) {
        // Format: set_skip_cycles_0_5 (valve 0, skip 5 cycles)
        int firstUnderscore = command.indexOf('_', 16);
        if (firstUnderscore != -1) {
            String valveStr = command.substring(16, firstUnderscore);
            String cyclesStr = command.substring(firstUnderscore + 1);
            int valveIndex = valveStr.toInt();
            int cycles = cyclesStr.toInt();
            DEBUG_SERIAL.println("Command: Set skip cycles for valve " + String(valveIndex) + " to " + String(cycles));
            wateringSystem->setSkipCycles(valveIndex, cycles);
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
            DEBUG_SERIAL.println("Published connection event");
        } else {
            DEBUG_SERIAL.println("Failed to publish connection event");
        }
    }
};

// Static member initialization
WateringSystem* NetworkManager::wateringSystem = nullptr;

#endif // NETWORK_MANAGER_H

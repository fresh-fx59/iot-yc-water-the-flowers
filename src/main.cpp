#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <secret.h>
#include <ota.h>

//developed with https://claude.ai/chat/391e9870-78b7-48cb-8733-b0c53d5dfb42

// Device configuration
const char* VERSION = "watering_system_0.0.1";
const char* DEVICE_TYPE = "smart_watering_system";

// MQTT Configuration
const char* MQTT_SERVER = "mqtt.cloud.yandex.net";
const int MQTT_PORT = 8883;
const int MQTT_BUFFER_SIZE = 1024;
const int MQTT_KEEP_ALIVE = 15;

// Pin definitions for ESP32-S3-DevKitC-1
#define DEBUG_SERIAL Serial
#define DEBUG_SERIAL_BAUDRATE 115200
#define LED_PIN 2  // Built-in LED on GPIO2

// Pump control
#define PUMP_PIN 4

// Valve pins
#define VALVE1_PIN 5
#define VALVE2_PIN 6
#define VALVE3_PIN 7
#define VALVE4_PIN 15
#define VALVE5_PIN 16
#define VALVE6_PIN 17

// Rain sensor pins (one per valve)
#define RAIN_SENSOR1_PIN 8
#define RAIN_SENSOR2_PIN 9
#define RAIN_SENSOR3_PIN 10
#define RAIN_SENSOR4_PIN 11
#define RAIN_SENSOR5_PIN 12
#define RAIN_SENSOR6_PIN 13

const int NUM_VALVES = 6;
const int VALVE_PINS[NUM_VALVES] = {VALVE1_PIN, VALVE2_PIN, VALVE3_PIN, VALVE4_PIN, VALVE5_PIN, VALVE6_PIN};
const int RAIN_SENSOR_PINS[NUM_VALVES] = {RAIN_SENSOR1_PIN, RAIN_SENSOR2_PIN, RAIN_SENSOR3_PIN, RAIN_SENSOR4_PIN, RAIN_SENSOR5_PIN, RAIN_SENSOR6_PIN};

// Timing constants
const unsigned long RAIN_CHECK_INTERVAL = 500; // Check rain sensor every 500ms
const unsigned long STATE_PUBLISH_INTERVAL = 2000; // Publish state every 2 seconds

// MQTT data
const String DEVICE_TOPIC_PREFIX = String("$devices/") + YC_DEVICE_ID + String("/");
const String COMMAND_TOPIC = DEVICE_TOPIC_PREFIX + String("commands");
const String EVENT_TOPIC = DEVICE_TOPIC_PREFIX + String("events");
const String STATE_TOPIC = DEVICE_TOPIC_PREFIX + String("state");

// Global objects
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// Forward declaration
class WateringSystem;

enum ValveState {
    VALVE_CLOSED = 0,
    VALVE_OPEN = 1
};

enum PumpState {
    PUMP_OFF = 0,
    PUMP_ON = 1
};

enum WateringPhase {
    PHASE_IDLE,
    PHASE_CHECKING_RAIN,
    PHASE_OPENING_VALVE,
    PHASE_WATERING,
    PHASE_CLOSING_VALVE,
    PHASE_ERROR
};

struct ValveController {
    int valveIndex;
    ValveState state;
    WateringPhase phase;
    bool rainDetected;
    unsigned long lastRainCheck;
    bool wateringRequested;
    
    ValveController(int idx) : 
        valveIndex(idx), 
        state(VALVE_CLOSED), 
        phase(PHASE_IDLE),
        rainDetected(false),
        lastRainCheck(0),
        wateringRequested(false) {}
};

class WateringSystem {
private:
    PumpState pumpState;
    ValveController* valves[NUM_VALVES];
    int activeValveCount;
    unsigned long lastStatePublish;
    
public:
    WateringSystem() : pumpState(PUMP_OFF), activeValveCount(0), lastStatePublish(0) {
        for (int i = 0; i < NUM_VALVES; i++) {
            valves[i] = new ValveController(i);
        }
    }
    
    void init() {
        // Initialize pump pin
        pinMode(PUMP_PIN, OUTPUT);
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(PUMP_PIN, LOW);
        digitalWrite(LED_PIN, LOW);
        
        // Initialize valve pins
        for (int i = 0; i < NUM_VALVES; i++) {
            pinMode(VALVE_PINS[i], OUTPUT);
            digitalWrite(VALVE_PINS[i], LOW);
        }
        
        // Initialize rain sensor pins with internal pull-up
        for (int i = 0; i < NUM_VALVES; i++) {
            pinMode(RAIN_SENSOR_PINS[i], INPUT_PULLUP);
        }
        
        DEBUG_SERIAL.println("WateringSystem initialized with " + String(NUM_VALVES) + " valves");
        DEBUG_SERIAL.println("Valve pins: 5,6,7,15,16,17");
        DEBUG_SERIAL.println("Rain sensor pins: 8,9,10,11,12,13");
        publishStateChange("system", "initialized");
    }
    
    void startWatering(int valveIndex) {
        if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
            DEBUG_SERIAL.println("Invalid valve index: " + String(valveIndex));
            publishStateChange("error", "invalid_valve_index");
            return;
        }
        
        ValveController* valve = valves[valveIndex];
        
        if (valve->phase != PHASE_IDLE) {
            DEBUG_SERIAL.println("Valve " + String(valveIndex) + " is already active");
            return;
        }
        
        DEBUG_SERIAL.println("Starting watering cycle for valve " + String(valveIndex));
        valve->wateringRequested = true;
        valve->phase = PHASE_CHECKING_RAIN;
        publishStateChange("valve" + String(valveIndex), "cycle_started");
    }
    
    void stopWatering(int valveIndex) {
        if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
            return;
        }
        
        ValveController* valve = valves[valveIndex];
        valve->wateringRequested = false;
        closeValve(valveIndex);
        valve->phase = PHASE_IDLE;
        publishStateChange("valve" + String(valveIndex), "cycle_stopped");
        updatePumpState();
    }
    
    void processWateringLoop() {
        unsigned long currentTime = millis();
        
        // Process each valve independently
        for (int i = 0; i < NUM_VALVES; i++) {
            processValve(i, currentTime);
        }
        
        // Publish state periodically
        if (currentTime - lastStatePublish >= STATE_PUBLISH_INTERVAL) {
            publishCurrentState();
            lastStatePublish = currentTime;
        }
    }
    
    void publishCurrentState() {
        if (!mqttClient.connected()) return;
        
        String stateJson = "{";
        stateJson += "\"pump\":\"" + String(pumpState == PUMP_ON ? "on" : "off") + "\",";
        stateJson += "\"valves\":[";
        
        for (int i = 0; i < NUM_VALVES; i++) {
            stateJson += "{";
            stateJson += "\"id\":" + String(i) + ",";
            stateJson += "\"state\":\"" + String(valves[i]->state == VALVE_OPEN ? "open" : "closed") + "\",";
            stateJson += "\"phase\":\"" + getPhaseString(valves[i]->phase) + "\",";
            stateJson += "\"rain\":" + String(valves[i]->rainDetected ? "true" : "false");
            stateJson += "}";
            if (i < NUM_VALVES - 1) stateJson += ",";
        }
        
        stateJson += "]}";
        
        mqttClient.publish(STATE_TOPIC.c_str(), stateJson.c_str());
    }
    
private:
    void processValve(int valveIndex, unsigned long currentTime) {
        ValveController* valve = valves[valveIndex];
        
        switch (valve->phase) {
            case PHASE_IDLE:
                // Nothing to do
                break;
                
            case PHASE_CHECKING_RAIN:
                // Check rain sensor before starting
                if (currentTime - valve->lastRainCheck >= RAIN_CHECK_INTERVAL) {
                    valve->lastRainCheck = currentTime;
                    bool isRaining = readRainSensor(valveIndex);
                    valve->rainDetected = isRaining;
                    
                    if (isRaining) {
                        DEBUG_SERIAL.println("Valve " + String(valveIndex) + ": Rain detected, aborting cycle");
                        publishStateChange("valve" + String(valveIndex), "rain_detected_abort");
                        valve->phase = PHASE_IDLE;
                        valve->wateringRequested = false;
                    } else {
                        DEBUG_SERIAL.println("Valve " + String(valveIndex) + ": No rain, opening valve");
                        valve->phase = PHASE_OPENING_VALVE;
                    }
                }
                break;
                
            case PHASE_OPENING_VALVE:
                openValve(valveIndex);
                valve->phase = PHASE_WATERING;
                publishStateChange("valve" + String(valveIndex), "valve_opened");
                break;
                
            case PHASE_WATERING:
                // Monitor rain sensor during watering
                if (currentTime - valve->lastRainCheck >= RAIN_CHECK_INTERVAL) {
                    valve->lastRainCheck = currentTime;
                    bool isRaining = readRainSensor(valveIndex);
                    valve->rainDetected = isRaining;
                    
                    if (isRaining) {
                        DEBUG_SERIAL.println("Valve " + String(valveIndex) + ": Rain detected during watering, closing");
                        publishStateChange("valve" + String(valveIndex), "rain_detected_stopping");
                        valve->phase = PHASE_CLOSING_VALVE;
                    } else if (!valve->wateringRequested) {
                        // Manual stop requested
                        valve->phase = PHASE_CLOSING_VALVE;
                    }
                }
                break;
                
            case PHASE_CLOSING_VALVE:
                closeValve(valveIndex);
                valve->phase = PHASE_IDLE;
                valve->wateringRequested = false;
                publishStateChange("valve" + String(valveIndex), "valve_closed");
                updatePumpState();
                break;
                
            case PHASE_ERROR:
                DEBUG_SERIAL.println("Valve " + String(valveIndex) + " in error state");
                closeValve(valveIndex);
                valve->phase = PHASE_IDLE;
                updatePumpState();
                break;
        }
    }
    
    bool readRainSensor(int valveIndex) {
        // Rain sensors typically output LOW when wet, HIGH when dry
        // Using internal pull-up, so:
        // HIGH (1) = dry (pulled up)
        // LOW (0) = wet (sensor pulls to ground)
        int sensorValue = digitalRead(RAIN_SENSOR_PINS[valveIndex]);
        return (sensorValue == LOW); // LOW = wet/rain detected
    }
    
    void openValve(int valveIndex) {
        digitalWrite(VALVE_PINS[valveIndex], HIGH);
        valves[valveIndex]->state = VALVE_OPEN;
        activeValveCount++;
        updatePumpState();
        DEBUG_SERIAL.println("Valve " + String(valveIndex) + " opened (GPIO " + String(VALVE_PINS[valveIndex]) + ")");
    }
    
    void closeValve(int valveIndex) {
        digitalWrite(VALVE_PINS[valveIndex], LOW);
        valves[valveIndex]->state = VALVE_CLOSED;
        if (activeValveCount > 0) activeValveCount--;
        DEBUG_SERIAL.println("Valve " + String(valveIndex) + " closed (GPIO " + String(VALVE_PINS[valveIndex]) + ")");
    }
    
    void updatePumpState() {
        // Turn pump on if any valve is open, off if all closed
        if (activeValveCount > 0 && pumpState == PUMP_OFF) {
            digitalWrite(PUMP_PIN, HIGH);
            digitalWrite(LED_PIN, HIGH);
            pumpState = PUMP_ON;
            DEBUG_SERIAL.println("Pump turned ON (GPIO " + String(PUMP_PIN) + ")");
            publishStateChange("pump", "on");
        } else if (activeValveCount == 0 && pumpState == PUMP_ON) {
            digitalWrite(PUMP_PIN, LOW);
            digitalWrite(LED_PIN, LOW);
            pumpState = PUMP_OFF;
            DEBUG_SERIAL.println("Pump turned OFF (GPIO " + String(PUMP_PIN) + ")");
            publishStateChange("pump", "off");
        }
    }
    
    String getPhaseString(WateringPhase phase) {
        switch (phase) {
            case PHASE_IDLE: return "idle";
            case PHASE_CHECKING_RAIN: return "checking_rain";
            case PHASE_OPENING_VALVE: return "opening_valve";
            case PHASE_WATERING: return "watering";
            case PHASE_CLOSING_VALVE: return "closing_valve";
            case PHASE_ERROR: return "error";
            default: return "unknown";
        }
    }
    
    void publishStateChange(const String& component, const String& state) {
        if (!mqttClient.connected()) {
            // Silently fail - don't disrupt watering algorithm
            return;
        }
        
        String eventJson = "{\"component\":\"" + component + "\",\"state\":\"" + state + "\",\"timestamp\":" + String(millis()) + "}";
        
        if (!mqttClient.publish(EVENT_TOPIC.c_str(), eventJson.c_str())) {
            // Failed to publish, but continue operation
            DEBUG_SERIAL.println("Failed to publish state change: " + component + " -> " + state);
        }
    }
};

class WiFiManager {
public:
    static void connect() {
        DEBUG_SERIAL.print("Connecting to WiFi: ");
        DEBUG_SERIAL.println(SSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(SSID, SSID_PASSWORD);
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 30) {
            delay(1000);
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
    
    static bool isConnected() {
        return WiFi.status() == WL_CONNECTED;
    }
};

class MQTTManager {
private:
    static WateringSystem* wateringSystem;
    
public:
    static void setWateringSystem(WateringSystem* ws) {
        wateringSystem = ws;
    }
    
    static void init() {
        wifiClient.setInsecure(); // For development - use proper certificates in production
        mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
        mqttClient.setCallback(messageCallback);
        mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
        mqttClient.setKeepAlive(MQTT_KEEP_ALIVE);
        
        DEBUG_SERIAL.println("MQTT Manager initialized");
    }
    
    static void connect() {
        if (mqttClient.connected()) {
            return;
        }
        
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
    
    static void loop() {
        if (mqttClient.connected()) {
            mqttClient.loop();
        } else {
            DEBUG_SERIAL.println("MQTT disconnected, attempting reconnect...");
            connect();
        }
    }
    
    static bool isConnected() {
        return mqttClient.connected();
    }
    
private:
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
    
    static void processCommand(const String& command) {
        if (!wateringSystem) {
            DEBUG_SERIAL.println("Error: WateringSystem not initialized");
            return;
        }
        
        // Commands:
        // start_valve_0, start_valve_1, ... start_valve_5 - start watering specific valve
        // stop_valve_0, stop_valve_1, ... stop_valve_5 - stop watering specific valve
        // state - publish current state
        // stop_all - stop all watering
        
        if (command.startsWith("start_valve_")) {
            int valveIndex = command.substring(12).toInt();
            DEBUG_SERIAL.println("Command: Start watering valve " + String(valveIndex));
            wateringSystem->startWatering(valveIndex);
        } else if (command.startsWith("stop_valve_")) {
            int valveIndex = command.substring(11).toInt();
            DEBUG_SERIAL.println("Command: Stop watering valve " + String(valveIndex));
            wateringSystem->stopWatering(valveIndex);
        } else if (command == "state") {
            DEBUG_SERIAL.println("Command: Publish state");
            wateringSystem->publishCurrentState();
        } else if (command == "stop_all") {
            DEBUG_SERIAL.println("Command: Stop all watering");
            for (int i = 0; i < NUM_VALVES; i++) {
                wateringSystem->stopWatering(i);
            }
        } else {
            DEBUG_SERIAL.println("Unknown command: " + command);
        }
    }
    
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

WateringSystem* MQTTManager::wateringSystem = nullptr;
WateringSystem wateringSystem;

void setup() {
    DEBUG_SERIAL.begin(DEBUG_SERIAL_BAUDRATE);
    delay(1000);
    
    DEBUG_SERIAL.println("\n\n=================================");
    DEBUG_SERIAL.println("Smart Watering System");
    DEBUG_SERIAL.println("Platform: ESP32-S3-DevKitC-1");
    DEBUG_SERIAL.println("Version: " + String(VERSION));
    DEBUG_SERIAL.println("Device ID: " + String(YC_DEVICE_ID));
    DEBUG_SERIAL.println("Valves: " + String(NUM_VALVES));
    DEBUG_SERIAL.println("=================================\n");
    
    // Initialize watering system
    wateringSystem.init();
    
    // Initialize MQTT
    MQTTManager::setWateringSystem(&wateringSystem);
    MQTTManager::init();
    
    // Connect to WiFi
    WiFiManager::connect();
    
    if (WiFiManager::isConnected()) {
        // Connect to MQTT
        MQTTManager::connect();
    }
    
    // Initialize OTA updates
    setupOta();
    
    DEBUG_SERIAL.println("Setup completed\n");
}

void loop() {
    // Check WiFi connection
    if (!WiFiManager::isConnected()) {
        DEBUG_SERIAL.println("WiFi disconnected, attempting reconnect...");
        WiFiManager::connect();
        delay(5000);
        return;
    }
    
    // Handle MQTT
    MQTTManager::loop();
    
    // Process watering logic for all valves
    wateringSystem.processWateringLoop();
    
    // Handle OTA updates
    loopOta();
    
    // Small delay to prevent watchdog issues
    delay(10);
}
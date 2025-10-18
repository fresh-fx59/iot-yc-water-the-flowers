#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <secret.h>
#include <ota.h>

// Device configuration
const char* VERSION = "watering_system_1.3.0";
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

// rain sensor on pin. sensor enables only when needed
#define SENSOR_POWER_PIN 18

const int NUM_VALVES = 6;
const int VALVE_PINS[NUM_VALVES] = {VALVE1_PIN, VALVE2_PIN, VALVE3_PIN, VALVE4_PIN, VALVE5_PIN, VALVE6_PIN};
const int RAIN_SENSOR_PINS[NUM_VALVES] = {RAIN_SENSOR1_PIN, RAIN_SENSOR2_PIN, RAIN_SENSOR3_PIN, RAIN_SENSOR4_PIN, RAIN_SENSOR5_PIN, RAIN_SENSOR6_PIN};

// Timing constants
const unsigned long RAIN_CHECK_INTERVAL = 100; // Check rain sensor every 500ms
const unsigned long VALVE_STABILIZATION_DELAY = 500; // Wait 500ms after opening valve before checking sensor
const unsigned long STATE_PUBLISH_INTERVAL = 2000; // Publish state every 2 seconds
const unsigned long MAX_WATERING_TIME = 5000; // ms

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
    PHASE_OPENING_VALVE,           // Step 1: Open valve first
    PHASE_WAITING_STABILIZATION,   // Step 2: Wait for valve to fully open
    PHASE_CHECKING_INITIAL_RAIN,   // Step 3: Check if already wet
    PHASE_WATERING,                // Step 4: Pump on, wait for wet sensor
    PHASE_CLOSING_VALVE,
    PHASE_ERROR
};

struct ValveController {
    int valveIndex;
    ValveState state;
    WateringPhase phase;
    bool rainDetected;
    unsigned long lastRainCheck;
    unsigned long valveOpenTime;      // When valve was opened
    unsigned long wateringStartTime;  // When pump started (for timeout)
    bool wateringRequested;
    bool timeoutOccurred;             // Flag if timeout happened
    
    ValveController(int idx) : 
        valveIndex(idx), 
        state(VALVE_CLOSED), 
        phase(PHASE_IDLE),
        rainDetected(false),
        lastRainCheck(0),
        valveOpenTime(0),
        wateringStartTime(0),
        wateringRequested(false),
        timeoutOccurred(false) {}
};


class WateringSystem {
private:
    PumpState pumpState;
    ValveController* valves[NUM_VALVES];
    int activeValveCount;
    unsigned long lastStatePublish;
    String lastStateJson;
    
    // Sequential watering state
    bool sequentialMode;
    int currentSequenceIndex;
    int sequenceValves[NUM_VALVES];
    int sequenceLength;
    
public:
    WateringSystem() : pumpState(PUMP_OFF), activeValveCount(0), lastStatePublish(0), lastStateJson(""),
                       sequentialMode(false), currentSequenceIndex(0), sequenceLength(0) {
        for (int i = 0; i < NUM_VALVES; i++) {
            valves[i] = new ValveController(i);
            sequenceValves[i] = 0;
        }
    }

    // Get last cached state for web interface
    String getLastState() {
        return lastStateJson;
    }
    
    void init() {
        // Initialize pump pin
        pinMode(PUMP_PIN, OUTPUT);
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(PUMP_PIN, LOW);
        digitalWrite(LED_PIN, LOW);

        // initialize pin that grants power(groud) to sensor pin 
        pinMode(SENSOR_POWER_PIN, OUTPUT);
        digitalWrite(SENSOR_POWER_PIN, LOW);
        
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
        
        DEBUG_SERIAL.println("═══════════════════════════════════════");
        DEBUG_SERIAL.println("Starting watering cycle for valve " + String(valveIndex));
        DEBUG_SERIAL.println("Step 1: Opening valve...");
        valve->wateringRequested = true;
        valve->rainDetected = false;
        valve->lastRainCheck = 0;
        valve->phase = PHASE_OPENING_VALVE;  // Start by opening valve first!
        publishStateChange("valve" + String(valveIndex), "cycle_started");
    }
    
    void stopWatering(int valveIndex) {
        if (valveIndex < 0 || valveIndex >= NUM_VALVES) {
            return;
        }
        
        ValveController* valve = valves[valveIndex];
        valve->wateringRequested = false;
        
        // Turn off pump first if this valve is watering
        if (valve->phase == PHASE_WATERING) {
            updatePumpState();
        }
        
        closeValve(valveIndex);
        valve->phase = PHASE_IDLE;
        publishStateChange("valve" + String(valveIndex), "cycle_stopped");
        
        // Final pump check
        updatePumpState();
    }
    
    void processWateringLoop() {
        unsigned long currentTime = millis();
        
        // Process each valve independently
        for (int i = 0; i < NUM_VALVES; i++) {
            processValve(i, currentTime);
        }
        
        // Handle sequential watering - check if current valve is done
        if (sequentialMode && currentSequenceIndex > 0) {
            int lastValveIndex = sequenceValves[currentSequenceIndex - 1];
            if (isValveComplete(lastValveIndex)) {
                // Previous valve is done, start next one
                delay(1000); // Small delay between valves
                startNextInSequence();
            }
        }
        
        // Publish state periodically
        if (currentTime - lastStatePublish >= STATE_PUBLISH_INTERVAL) {
            publishCurrentState();
            lastStatePublish = currentTime;
        }
    }

    void publishCurrentState() {
        // Build state JSON
        String stateJson = "{";
        stateJson += "\"pump\":\"" + String(pumpState == PUMP_ON ? "on" : "off") + "\",";
        stateJson += "\"sequential_mode\":" + String(sequentialMode ? "true" : "false") + ",";
        
        if (sequentialMode) {
            stateJson += "\"sequence_progress\":" + String(currentSequenceIndex) + ",";
            stateJson += "\"sequence_total\":" + String(sequenceLength) + ",";
        }
        
        stateJson += "\"valves\":[";
        
        for (int i = 0; i < NUM_VALVES; i++) {
            stateJson += "{";
            stateJson += "\"id\":" + String(i) + ",";
            stateJson += "\"state\":\"" + String(valves[i]->state == VALVE_OPEN ? "open" : "closed") + "\",";
            stateJson += "\"phase\":\"" + getPhaseString(valves[i]->phase) + "\",";
            stateJson += "\"rain\":" + String(valves[i]->rainDetected ? "true" : "false") + ",";
            stateJson += "\"timeout\":" + String(valves[i]->timeoutOccurred ? "true" : "false");
            
            // Add watering progress if active
            if (valves[i]->phase == PHASE_WATERING && valves[i]->wateringStartTime > 0) {
                unsigned long elapsed = millis() - valves[i]->wateringStartTime;
                int remainingSeconds = (MAX_WATERING_TIME - elapsed) / 1000;
                if (remainingSeconds < 0) remainingSeconds = 0;
                stateJson += ",\"watering_seconds\":" + String(elapsed / 1000);
                stateJson += ",\"remaining_seconds\":" + String(remainingSeconds);
            }
            
            stateJson += "}";
            if (i < NUM_VALVES - 1) stateJson += ",";
        }
        
        stateJson += "]}";
        
        // Cache state for web interface
        lastStateJson = stateJson;
        
        // Publish to MQTT if connected
        if (mqttClient.connected()) {
            mqttClient.publish(STATE_TOPIC.c_str(), stateJson.c_str());
        }
    }

    void clearTimeoutFlag(int valveIndex) {
        if (valveIndex >= 0 && valveIndex < NUM_VALVES) {
            valves[valveIndex]->timeoutOccurred = false;
            DEBUG_SERIAL.println("Timeout flag cleared for valve " + String(valveIndex));
            publishStateChange("valve" + String(valveIndex), "timeout_cleared");
        }
    }

        // NEW FUNCTION: Start sequential watering of all valves
    void startSequentialWatering() {
        if (sequentialMode) {
            DEBUG_SERIAL.println("Sequential watering already in progress");
            return;
        }
        
        DEBUG_SERIAL.println("\n╔═══════════════════════════════════════════╗");
        DEBUG_SERIAL.println("║  SEQUENTIAL WATERING STARTED (ALL VALVES) ║");
        DEBUG_SERIAL.println("╚═══════════════════════════════════════════╝");
        
        // Prepare sequence: all valves 0-5
        sequenceLength = NUM_VALVES;
        for (int i = 0; i < NUM_VALVES; i++) {
            sequenceValves[i] = i;
        }
        
        sequentialMode = true;
        currentSequenceIndex = 0;
        
        publishStateChange("system", "sequential_started");
        
        // Start first valve
        startNextInSequence();
    }
    
    // NEW FUNCTION: Start sequential watering of specific valves
    void startSequentialWateringCustom(int* valveIndices, int count) {
        if (sequentialMode) {
            DEBUG_SERIAL.println("Sequential watering already in progress");
            return;
        }
        
        if (count == 0 || count > NUM_VALVES) {
            DEBUG_SERIAL.println("Invalid valve count for sequential watering");
            return;
        }
        
        DEBUG_SERIAL.println("\n╔═══════════════════════════════════════════╗");
        DEBUG_SERIAL.println("║  SEQUENTIAL WATERING STARTED              ║");
        DEBUG_SERIAL.println("╚═══════════════════════════════════════════╝");
        DEBUG_SERIAL.print("Valve sequence: ");
        for (int i = 0; i < count; i++) {
            DEBUG_SERIAL.print(valveIndices[i]);
            if (i < count - 1) DEBUG_SERIAL.print(", ");
        }
        DEBUG_SERIAL.println();
        
        // Prepare custom sequence
        sequenceLength = count;
        for (int i = 0; i < count; i++) {
            sequenceValves[i] = valveIndices[i];
        }
        
        sequentialMode = true;
        currentSequenceIndex = 0;
        
        publishStateChange("system", "sequential_started");
        
        // Start first valve
        startNextInSequence();
    }
    
    // NEW FUNCTION: Stop sequential watering
    void stopSequentialWatering() {
        if (!sequentialMode) {
            return;
        }
        
        DEBUG_SERIAL.println("\n⚠️  SEQUENTIAL WATERING STOPPED");
        sequentialMode = false;
        
        // Stop all active valves
        for (int i = 0; i < NUM_VALVES; i++) {
            if (valves[i]->phase != PHASE_IDLE) {
                stopWatering(i);
            }
        }
        
        publishStateChange("system", "sequential_stopped");
    }
    
    // NEW FUNCTION: Check if a valve has completed its cycle
    bool isValveComplete(int valveIndex) {
        return valves[valveIndex]->phase == PHASE_IDLE && !valves[valveIndex]->wateringRequested;
    }
    
    // NEW FUNCTION: Start next valve in sequence
    void startNextInSequence() {
        if (!sequentialMode) {
            return;
        }
        
        if (currentSequenceIndex >= sequenceLength) {
            // Sequence complete!
            DEBUG_SERIAL.println("\n╔═══════════════════════════════════════════╗");
            DEBUG_SERIAL.println("║  SEQUENTIAL WATERING COMPLETE ✓           ║");
            DEBUG_SERIAL.println("╚═══════════════════════════════════════════╝");
            sequentialMode = false;
            publishStateChange("system", "sequential_complete");
            return;
        }
        
        int valveIndex = sequenceValves[currentSequenceIndex];
        DEBUG_SERIAL.println("\n→ [Sequence " + String(currentSequenceIndex + 1) + "/" + String(sequenceLength) + 
                           "] Starting Valve " + String(valveIndex));
        
        startWatering(valveIndex);
        currentSequenceIndex++;
    }
    
private:
    void processValve(int valveIndex, unsigned long currentTime) {
        ValveController* valve = valves[valveIndex];
        
        switch (valve->phase) {
            case PHASE_IDLE:
                // Nothing to do
                break;
                
            case PHASE_OPENING_VALVE:
                // Open valve FIRST, before checking sensor
                openValve(valveIndex);
                valve->valveOpenTime = currentTime;
                valve->phase = PHASE_WAITING_STABILIZATION;
                DEBUG_SERIAL.println("  Valve " + String(valveIndex) + " opened, waiting for stabilization...");
                publishStateChange("valve" + String(valveIndex), "valve_opened");
                break;
                
            case PHASE_WAITING_STABILIZATION:
                // Wait for valve to fully open and water to start flowing
                if (currentTime - valve->valveOpenTime >= VALVE_STABILIZATION_DELAY) {
                    valve->phase = PHASE_CHECKING_INITIAL_RAIN;
                    valve->lastRainCheck = currentTime;
                    DEBUG_SERIAL.println("Step 2: Checking rain sensor (valve is open)...");
                }
                break;
                
            case PHASE_CHECKING_INITIAL_RAIN:
                // Now check if sensor is already wet (valve is open)
                if (currentTime - valve->lastRainCheck >= RAIN_CHECK_INTERVAL) {
                    valve->lastRainCheck = currentTime;
                    bool isRaining = readRainSensor(valveIndex);
                    valve->rainDetected = isRaining;
                    
                    if (isRaining) {
                        // Sensor is already wet - close valve WITHOUT STARTING PUMP
                        DEBUG_SERIAL.println("  Sensor " + String(valveIndex) + " is already WET");
                        DEBUG_SERIAL.println("  Pump will NOT start - closing valve");
                        publishStateChange("valve" + String(valveIndex), "already_wet_abort");
                        valve->phase = PHASE_CLOSING_VALVE;
                        // Pump state will be checked after valve closes
                    } else {
                        // Sensor is dry - start watering!
                        DEBUG_SERIAL.println("  Sensor " + String(valveIndex) + " is DRY");
                        DEBUG_SERIAL.println("Step 3: Starting pump, watering until sensor is wet...");
                        DEBUG_SERIAL.println("  Safety timeout: " + String(MAX_WATERING_TIME / 1000) + " seconds");
                        valve->wateringStartTime = currentTime;
                        valve->timeoutOccurred = false;
                        valve->phase = PHASE_WATERING;
                        updatePumpState();  // ← NOW turn pump ON (only when actually watering)
                        publishStateChange("valve" + String(valveIndex), "watering_started");
                    }
                }
                break;
                
            case PHASE_WATERING:
                // Check for timeout FIRST
                if (currentTime - valve->wateringStartTime >= MAX_WATERING_TIME) {
                    // TIMEOUT! Safety stop
                    DEBUG_SERIAL.println("\n⚠️  TIMEOUT: Valve " + String(valveIndex) + " exceeded maximum watering time!");
                    DEBUG_SERIAL.println("  Max time: " + String(MAX_WATERING_TIME / 1000) + " seconds");
                    DEBUG_SERIAL.println("  Sensor may be faulty or water supply issue");
                    DEBUG_SERIAL.println("  SAFETY STOP - Closing valve");
                    valve->timeoutOccurred = true;
                    valve->phase = PHASE_CLOSING_VALVE;
                    updatePumpState();  // ← Turn pump OFF before closing valve
                    publishStateChange("valve" + String(valveIndex), "timeout_safety_stop");
                    break;
                }
                
                // Monitor rain sensor - wait until it detects water
                if (currentTime - valve->lastRainCheck >= RAIN_CHECK_INTERVAL) {
                    valve->lastRainCheck = currentTime;
                    bool isRaining = readRainSensor(valveIndex);
                    valve->rainDetected = isRaining;
                    
                    // Show progress every 10 seconds
                    if ((currentTime - valve->wateringStartTime) % 10000 < RAIN_CHECK_INTERVAL) {
                        int elapsed = (currentTime - valve->wateringStartTime) / 1000;
                        int remaining = (MAX_WATERING_TIME - (currentTime - valve->wateringStartTime)) / 1000;
                        DEBUG_SERIAL.println("  Valve " + String(valveIndex) + " watering: " + 
                                        String(elapsed) + "s elapsed, " + 
                                        String(remaining) + "s remaining");
                    }
                    
                    if (isRaining) {
                        // Sensor detected water - stop!
                        int totalTime = (currentTime - valve->wateringStartTime) / 1000;
                        DEBUG_SERIAL.println("  Sensor " + String(valveIndex) + " detected WATER ✓");
                        DEBUG_SERIAL.println("  Total watering time: " + String(totalTime) + " seconds");
                        DEBUG_SERIAL.println("Step 4: Stopping pump and closing valve");
                        DEBUG_SERIAL.println("═══════════════════════════════════════");
                        publishStateChange("valve" + String(valveIndex), "watering_complete");
                        valve->phase = PHASE_CLOSING_VALVE;
                        updatePumpState();  // ← Turn pump OFF before closing valve
                    } else if (!valve->wateringRequested) {
                        // Manual stop requested
                        DEBUG_SERIAL.println("  Manual stop requested for valve " + String(valveIndex));
                        valve->phase = PHASE_CLOSING_VALVE;
                        updatePumpState();  // ← Turn pump OFF before closing valve
                    }
                }
                break;
                
            case PHASE_CLOSING_VALVE:
                closeValve(valveIndex);
                valve->phase = PHASE_IDLE;
                valve->wateringRequested = false;
                publishStateChange("valve" + String(valveIndex), "valve_closed");
                // Final pump state check after valve is closed
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
        // Power on sensor
        digitalWrite(SENSOR_POWER_PIN, HIGH);
        delay(100);  // Let it stabilize

        // Rain sensors typically output LOW when wet, HIGH when dry
        // Using internal pull-up, so:
        // HIGH (1) = dry (pulled up)
        // LOW (0) = wet (sensor pulls to ground)
        bool sensorValue = digitalRead(RAIN_SENSOR_PINS[valveIndex]);

        // Power off immediately
        digitalWrite(SENSOR_POWER_PIN, LOW);

        return (sensorValue == LOW); // LOW = wet/rain detected
    }
    
    void openValve(int valveIndex) {
        digitalWrite(VALVE_PINS[valveIndex], HIGH);
        valves[valveIndex]->state = VALVE_OPEN;
        activeValveCount++;
        // DON'T call updatePumpState() here - we'll do it only when actually watering
        DEBUG_SERIAL.println("Valve " + String(valveIndex) + " opened (GPIO " + String(VALVE_PINS[valveIndex]) + ")");
    }
    
    void closeValve(int valveIndex) {
        digitalWrite(VALVE_PINS[valveIndex], LOW);
        valves[valveIndex]->state = VALVE_CLOSED;
        if (activeValveCount > 0) activeValveCount--;
        // DON'T call updatePumpState() here either
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
            case PHASE_OPENING_VALVE: return "opening_valve";
            case PHASE_WAITING_STABILIZATION: return "stabilizing";
            case PHASE_CHECKING_INITIAL_RAIN: return "checking_sensor";
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
    // start_all - start sequential watering of all valves
    // start_sequence_0,2,4 - start sequential watering of specific valves (comma-separated)
    // stop_all - stop all watering (including sequential mode)
    // state - publish current state
    
    if (command.startsWith("start_valve_")) {
        int valveIndex = command.substring(12).toInt();
        DEBUG_SERIAL.println("Command: Start watering valve " + String(valveIndex));
        wateringSystem->startWatering(valveIndex);
        
    } else if (command.startsWith("stop_valve_")) {
        int valveIndex = command.substring(11).toInt();
        DEBUG_SERIAL.println("Command: Stop watering valve " + String(valveIndex));
        wateringSystem->stopWatering(valveIndex);
        
    } else if (command == "start_all") {
        DEBUG_SERIAL.println("Command: Start sequential watering (all valves)");
        wateringSystem->startSequentialWatering();
        
    } else if (command.startsWith("start_sequence_")) {
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
        
    } else if (command == "stop_all") {
        DEBUG_SERIAL.println("Command: Stop all watering");
        wateringSystem->stopSequentialWatering();
        for (int i = 0; i < NUM_VALVES; i++) {
            wateringSystem->stopWatering(i);
        }
        
    } else if (command == "state") {
        DEBUG_SERIAL.println("Command: Publish state");
        wateringSystem->publishCurrentState();
        
    } else if (command.startsWith("clear_timeout_")) {
        int valveIndex = command.substring(14).toInt();
        DEBUG_SERIAL.println("Command: Clear timeout flag for valve " + String(valveIndex));
        wateringSystem->clearTimeoutFlag(valveIndex);
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
    // Initialize serial at correct baud rate
    Serial.begin(115200);
    
    // Wait extra time for serial monitor to connect
    delay(3000);
    
    // Send several newlines to clear any garbage
    Serial.println("\n\n\n");
    delay(100);
    
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
    
    // Initialize MQTT
    MQTTManager::setWateringSystem(&wateringSystem);
    MQTTManager::init();
    
    // Connect to WiFi
    WiFiManager::connect();
    
    if (WiFiManager::isConnected()) {
        // Connect to MQTT
        MQTTManager::connect();
    }
    
    // CRITICAL: Set the watering system reference for web API BEFORE setupOta()
    setWateringSystemRef(&wateringSystem);
    
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

// ============================================
// API Handler implementations
// ============================================
extern WebServer httpServer;
extern WateringSystem* g_wateringSystem_ptr;

void handleWaterApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }
    
    String valveStr = httpServer.arg("valve");
    int valve = valveStr.toInt();
    
    if (valve < 1 || valve > 6) {
        httpServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid valve number\"}");
        return;
    }
    
    Serial.printf("✓ API: Starting watering for valve %d\n", valve);
    g_wateringSystem_ptr->startWatering(valve - 1);
    httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Watering started\"}");
}

void handleStopApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }
    
    String valveStr = httpServer.arg("valve");
    
    if (valveStr == "all") {
        Serial.println("✓ API: Stopping all valves");
        for (int i = 0; i < 6; i++) {
            g_wateringSystem_ptr->stopWatering(i);
        }
        httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"All watering stopped\"}");
    } else {
        int valve = valveStr.toInt();
        if (valve < 1 || valve > 6) {
            httpServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid valve number\"}");
            return;
        }
        Serial.printf("✓ API: Stopping valve %d\n", valve);
        g_wateringSystem_ptr->stopWatering(valve - 1);
        httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Watering stopped\"}");
    }
}

void handleStatusApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }
    
    String stateJson = g_wateringSystem_ptr->getLastState();
    
    if (stateJson.length() == 0) {
        stateJson = "{\"pump\":\"off\",\"valves\":[";
        for (int i = 0; i < 6; i++) {
            stateJson += "{\"id\":" + String(i) + ",\"state\":\"closed\",\"phase\":\"idle\",\"rain\":false}";
            if (i < 5) stateJson += ",";
        }
        stateJson += "]}";
    }
    
    httpServer.send(200, "application/json", stateJson);
}

// Register API handlers (called from setup after setupOta)
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

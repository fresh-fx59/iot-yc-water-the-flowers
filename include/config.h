#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <secret.h>

// ============================================
// Device Configuration
// ============================================
const char *VERSION = "watering_system_1.13.5";
const char *DEVICE_TYPE = "smart_watering_system_time_based";

// ============================================
// Hardware Pin Definitions (ESP32-S3-N8R2)
// ============================================
#define LED_PIN 48  // Built-in RGB NeoPixel LED
#define PUMP_PIN 4
#define RAIN_SENSOR_POWER_PIN 18

// Valve pins
#define VALVE1_PIN 5
#define VALVE2_PIN 6
#define VALVE3_PIN 7
#define VALVE4_PIN 15
#define VALVE5_PIN 16
#define VALVE6_PIN 17

// Rain sensor pins
#define RAIN_SENSOR1_PIN 8
#define RAIN_SENSOR2_PIN 9
#define RAIN_SENSOR3_PIN 10
#define RAIN_SENSOR4_PIN 11
#define RAIN_SENSOR5_PIN 12
#define RAIN_SENSOR6_PIN 13

// DS3231 RTC I2C pins
#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 3
#define DS3231_I2C_ADDRESS 0x68

// DS3231 Battery Measurement pins
#define BATTERY_ADC_PIN 1        // ADC pin (reads voltage divider)
#define BATTERY_CONTROL_PIN 2    // Controls transistor (HIGH = measure, LOW = off)

// Master Overflow Sensor pin (2N2222 transistor circuit)
#define MASTER_OVERFLOW_SENSOR_PIN 42  // LOW = overflow detected, HIGH = normal

// ============================================
// System Constants
// ============================================
const int NUM_VALVES = 6;
const int VALVE_PINS[NUM_VALVES] = {VALVE1_PIN, VALVE2_PIN, VALVE3_PIN,
                                    VALVE4_PIN, VALVE5_PIN, VALVE6_PIN};
const int RAIN_SENSOR_PINS[NUM_VALVES] = {RAIN_SENSOR1_PIN, RAIN_SENSOR2_PIN,
                                          RAIN_SENSOR3_PIN, RAIN_SENSOR4_PIN,
                                          RAIN_SENSOR5_PIN, RAIN_SENSOR6_PIN};

// ============================================
// Timing Constants
// ============================================
const unsigned long RAIN_CHECK_INTERVAL = 100; // Check rain sensor every 100ms
const unsigned long VALVE_STABILIZATION_DELAY =
    500; // Wait 500ms for valve to open
const unsigned long STATE_PUBLISH_INTERVAL =
    2000;                                      // Publish state every 2 seconds
const unsigned long MAX_WATERING_TIME = 25000; // Maximum watering time (25s) - REDUCED FOR SAFETY
const unsigned long ABSOLUTE_SAFETY_TIMEOUT = 30000; // Absolute hard limit (30s) - EMERGENCY CUTOFF
const unsigned long SENSOR_POWER_STABILIZATION = 100; // Sensor power-on delay

// ============================================
// Learning Algorithm Constants
// ============================================
const float LEARNING_EMPTY_THRESHOLD =
    0.95; // If fill_ratio >= 0.95, consider tray empty
const float LEARNING_FULL_THRESHOLD =
    0.10; // If fill_ratio < 0.10, tray was almost full
const int LEARNING_MAX_SKIP_CYCLES = 15; // Maximum cycles to skip
const int LEARNING_FULL_SKIP_CYCLES =
    10; // Skip cycles when tray is almost full
const unsigned long AUTO_WATERING_MIN_INTERVAL_MS =
    86400000; // 24 hours minimum between auto-watering attempts
const unsigned long UNCALIBRATED_RETRY_INTERVAL_MS =
    86400000; // 24 hours retry for uncalibrated trays found full

// ============================================
// DS3231 Battery Voltage Calibration
// ============================================
// Adjust this value to match your multimeter reading
// Formula: CALIBRATION_FACTOR = (multimeter_voltage / raw_reading)
// Example: If multimeter shows 3.23V and program shows 3.02V:
//          CALIBRATION_FACTOR = 3.23 / 3.02 = 1.0695
const float BATTERY_VOLTAGE_CALIBRATION = 1.0695;

// ============================================
// Debug Configuration
// ============================================
#define IS_DEBUG_TO_SERIAL_ENABLED false
#define IS_DEBUG_TO_TELEGRAM_ENABLED true

// ============================================
// Telegram Queue Configuration
// ============================================
const int TELEGRAM_QUEUE_SIZE = 20;        // Max messages in queue
const int TELEGRAM_MAX_RETRY_ATTEMPTS = 5; // Retry attempts per message
const unsigned long TELEGRAM_RETRY_DELAY_MS = 2000; // Wait 2s between retries
const unsigned long MESSAGE_GROUP_INTERVAL_MS =
    2000; // Group messages within 2 seconds
const unsigned long MESSAGE_GROUP_MAX_AGE_MS =
    180000; // Flush after 3 min max (safety limit)

// ============================================
// Serial Configuration
// ============================================
#define DEBUG_SERIAL Serial
#define DEBUG_SERIAL_BAUDRATE 115200

// ============================================
// MQTT Configuration
// ============================================
const char *MQTT_SERVER = "mqtt.cloud.yandex.net";
const int MQTT_PORT = 8883;
const int MQTT_BUFFER_SIZE = 1024;
const int MQTT_KEEP_ALIVE = 15;

// MQTT Topics
const String DEVICE_TOPIC_PREFIX =
    String("$devices/") + YC_DEVICE_ID + String("/");
const String COMMAND_TOPIC = DEVICE_TOPIC_PREFIX + String("commands");
const String EVENT_TOPIC = DEVICE_TOPIC_PREFIX + String("events");
const String STATE_TOPIC = DEVICE_TOPIC_PREFIX + String("state");

// ============================================
// WiFi Configuration
// ============================================
const int WIFI_MAX_RETRY_ATTEMPTS = 30;
const int WIFI_RETRY_DELAY_MS = 500;

// ============================================
// OTA Configuration
// ============================================
const char *OTA_HOSTNAME = "esp32-watering";

#endif // CONFIG_H

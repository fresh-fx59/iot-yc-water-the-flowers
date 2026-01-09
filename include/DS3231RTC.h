#ifndef DS3231RTC_H
#define DS3231RTC_H

#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include "config.h"

// ============================================
// DS3231 RTC Helper Functions
// ============================================

namespace DS3231RTC {

// BCD conversion helpers
inline uint8_t bcdToDec(uint8_t val) {
  return (val / 16 * 10) + (val % 16);
}

inline uint8_t decToBcd(uint8_t val) {
  return ((val / 10 * 16) + (val % 10));
}

// Read single register from DS3231
inline uint8_t readRegister(uint8_t reg) {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 1);
  return Wire.read();
}

// Write single register to DS3231
inline void writeRegister(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// Initialize DS3231 RTC and I2C
inline bool init() {
  // Initialize I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Check if DS3231 is responding
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  byte error = Wire.endTransmission();

  if (error != 0) {
    Serial.println("❌ ERROR: DS3231 not found on I2C bus!");
    return false;
  }

  Serial.println("✓ DS3231 RTC initialized (SDA: GPIO 14, SCL: GPIO 3)");
  return true;
}

// Read current time from DS3231 and return as time_t (Unix timestamp)
inline time_t getTime() {
  // Read time registers (0x00 to 0x06)
  uint8_t second = bcdToDec(readRegister(0x00) & 0x7F);
  uint8_t minute = bcdToDec(readRegister(0x01));
  uint8_t hour = bcdToDec(readRegister(0x02) & 0x3F);
  uint8_t dayOfWeek = bcdToDec(readRegister(0x03));
  uint8_t day = bcdToDec(readRegister(0x04));
  uint8_t month = bcdToDec(readRegister(0x05) & 0x1F);
  uint8_t year = bcdToDec(readRegister(0x06));

  // Convert to tm structure
  struct tm timeinfo;
  timeinfo.tm_sec = second;
  timeinfo.tm_min = minute;
  timeinfo.tm_hour = hour;
  timeinfo.tm_mday = day;
  timeinfo.tm_mon = month - 1;  // tm_mon is 0-11
  timeinfo.tm_year = year + 100;  // tm_year is years since 1900, DS3231 year is since 2000
  timeinfo.tm_wday = dayOfWeek - 1;  // tm_wday is 0-6 (Sunday=0), DS3231 is 1-7
  timeinfo.tm_isdst = 0;  // No DST

  // Convert to Unix timestamp
  return mktime(&timeinfo);
}

// Get current time and fill tm structure
inline bool getLocalTime(struct tm *timeinfo) {
  time_t now = getTime();
  if (now == -1) {
    return false;
  }

  *timeinfo = *localtime(&now);
  return true;
}

// Set DS3231 time
inline void setTime(uint8_t second, uint8_t minute, uint8_t hour,
                    uint8_t dayOfWeek, uint8_t day, uint8_t month, uint8_t year) {
  writeRegister(0x00, decToBcd(second));
  writeRegister(0x01, decToBcd(minute));
  writeRegister(0x02, decToBcd(hour));
  writeRegister(0x03, decToBcd(dayOfWeek));
  writeRegister(0x04, decToBcd(day));
  writeRegister(0x05, decToBcd(month));
  writeRegister(0x06, decToBcd(year));
}

// Set DS3231 time from Unix timestamp
inline void setTime(time_t timestamp) {
  struct tm *timeinfo = localtime(&timestamp);

  setTime(
    timeinfo->tm_sec,
    timeinfo->tm_min,
    timeinfo->tm_hour,
    timeinfo->tm_wday + 1,  // Convert 0-6 to 1-7
    timeinfo->tm_mday,
    timeinfo->tm_mon + 1,   // Convert 0-11 to 1-12
    timeinfo->tm_year - 100 // Convert years since 1900 to years since 2000
  );
}

// Read temperature from DS3231
inline float getTemperature() {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0x11);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 2);
  int8_t tempMSB = Wire.read();
  uint8_t tempLSB = Wire.read();
  return tempMSB + ((tempLSB >> 6) * 0.25);
}

// Read battery voltage
inline float getBatteryVoltage() {
  // Turn on transistor to enable voltage divider
  digitalWrite(BATTERY_CONTROL_PIN, HIGH);
  delay(100);  // Wait for circuit to stabilize

  // Take multiple readings and average them
  const int numReadings = 10;
  long adcSum = 0;
  for (int i = 0; i < numReadings; i++) {
    adcSum += analogRead(BATTERY_ADC_PIN);
    delay(10);
  }

  // Turn off transistor to save battery
  digitalWrite(BATTERY_CONTROL_PIN, LOW);

  // Calculate average ADC value
  float adcAverage = adcSum / (float)numReadings;

  // Convert to voltage (ESP32-S3 ADC with 11db attenuation: 0-3.3V → 0-4095)
  float adcVoltage = (adcAverage / 4095.0) * 3.3;

  // Battery voltage is 2x the ADC voltage (voltage divider with R1=R2=100kΩ)
  float batteryVoltageRaw = adcVoltage * 2.0;

  // Apply calibration factor
  return batteryVoltageRaw * BATTERY_VOLTAGE_CALIBRATION;
}

// Print current time to serial (for debugging)
inline void printTime() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.print("DS3231 Time: ");
    Serial.println(buffer);
  } else {
    Serial.println("Failed to read time from DS3231");
  }
}

} // namespace DS3231RTC

#endif // DS3231RTC_H

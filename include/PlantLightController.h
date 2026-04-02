#ifndef PLANT_LIGHT_CONTROLLER_H
#define PLANT_LIGHT_CONTROLLER_H

#include <Arduino.h>
#include <time.h>

#include "config.h"

enum PlantLightMode {
  PLANT_LIGHT_MODE_AUTO = 0,
  PLANT_LIGHT_MODE_MANUAL_ON = 1,
  PLANT_LIGHT_MODE_MANUAL_OFF = 2
};

class PlantLightController {
private:
  bool lampOn;
  PlantLightMode mode;

  bool writeRelayState(bool enabled) {
    lampOn = enabled;
    digitalWrite(PLANT_LIGHT_RELAY_PIN,
                 enabled == PLANT_LIGHT_ACTIVE_HIGH ? HIGH : LOW);
    return true;
  }

  bool applyRelayState(bool enabled) {
    if (lampOn == enabled) {
      return false;
    }

    return writeRelayState(enabled);
  }

public:
  PlantLightController() : lampOn(false), mode(PLANT_LIGHT_MODE_AUTO) {}

  void init() {
    pinMode(PLANT_LIGHT_RELAY_PIN, OUTPUT);
    writeRelayState(false);
  }

  static bool isScheduleActive(const tm &timeInfo) {
    const int currentMinute = timeInfo.tm_hour * 60 + timeInfo.tm_min;
    const int onMinute =
        PLANT_LIGHT_SCHEDULE_ON_HOUR * 60 + PLANT_LIGHT_SCHEDULE_ON_MINUTE;
    const int offMinute =
        PLANT_LIGHT_SCHEDULE_OFF_HOUR * 60 + PLANT_LIGHT_SCHEDULE_OFF_MINUTE;

    if (onMinute == offMinute) {
      return true;
    }

    if (onMinute < offMinute) {
      return currentMinute >= onMinute && currentMinute < offMinute;
    }

    return currentMinute >= onMinute || currentMinute < offMinute;
  }

  bool shouldBeOnNow(time_t now) const {
    tm timeInfo;
    localtime_r(&now, &timeInfo);
    return isScheduleActive(timeInfo);
  }

  bool applyAutomaticSchedule(time_t now) {
    if (mode != PLANT_LIGHT_MODE_AUTO) {
      return false;
    }

    return applyRelayState(shouldBeOnNow(now));
  }

  bool setManualOn() {
    mode = PLANT_LIGHT_MODE_MANUAL_ON;
    return applyRelayState(true);
  }

  bool setManualOff() {
    mode = PLANT_LIGHT_MODE_MANUAL_OFF;
    return applyRelayState(false);
  }

  bool setAuto(time_t now) {
    mode = PLANT_LIGHT_MODE_AUTO;
    return applyRelayState(shouldBeOnNow(now));
  }

  void syncAutoStateSilently(time_t now) {
    mode = PLANT_LIGHT_MODE_AUTO;
    writeRelayState(shouldBeOnNow(now));
  }

  bool isOn() const { return lampOn; }

  PlantLightMode getMode() const { return mode; }

  const char *getModeName() const {
    switch (mode) {
    case PLANT_LIGHT_MODE_MANUAL_ON:
      return "manual_on";
    case PLANT_LIGHT_MODE_MANUAL_OFF:
      return "manual_off";
    case PLANT_LIGHT_MODE_AUTO:
    default:
      return "auto";
    }
  }
};

#endif // PLANT_LIGHT_CONTROLLER_H

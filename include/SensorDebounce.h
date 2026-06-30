#ifndef SENSOR_DEBOUNCE_H
#define SENSOR_DEBOUNCE_H

// Pure, hardware-free debounce decision shared by the firmware and the native
// test suite. Rain/soil sensors are active-LOW (LOW = wet).
//
// A single stray LOW reading -- EMI from pump/valve switching, condensation, a
// momentary contact -- must NOT be taken as "wet". Reading one spike as wet both
// cuts a watering cycle short (under-fill) and can trip the "tray already full
// -> 2x interval" learning path, which is how one tray's learned interval runs
// away to the cap and that tray ends up heavily under-watered. We therefore
// sample the pin N times and only declare "wet" when at least `threshold` of
// those samples were LOW (e.g. 5 of 7), mirroring the master overflow sensor.
namespace SensorDebounce {

// Decide "wet" from the number of LOW (wet) samples counted in a sample window.
inline bool isWet(int lowReadings, int threshold) {
  return lowReadings >= threshold;
}

}  // namespace SensorDebounce

#endif  // SENSOR_DEBOUNCE_H

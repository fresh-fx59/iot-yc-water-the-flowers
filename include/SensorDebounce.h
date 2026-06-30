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

// Sustained-wet confirmation across watering polls. Debounce (above) rejects a
// single noisy SAMPLE; this rejects a single noisy READ. A fill only completes
// after `required` consecutive wet reads (each ~RAIN_CHECK_INTERVAL apart), so a
// brief mid-cycle flicker can't end watering early and be logged as a real fill
// — the field failure that ran tray intervals away. Mirrors the overflow
// sensor's CONFIRMATION_CHECKS. A wet read grows the streak; any dry read resets
// it to 0.
inline int nextWetStreak(int streak, bool wet) { return wet ? streak + 1 : 0; }
inline bool fillConfirmed(int streak, int required) { return streak >= required; }

}  // namespace SensorDebounce

#endif  // SENSOR_DEBOUNCE_H

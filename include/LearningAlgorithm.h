#ifndef LEARNING_ALGORITHM_H
#define LEARNING_ALGORITHM_H

#include <Arduino.h>
#ifdef NATIVE_TEST
#include "TestConfig.h"
#else
#include "config.h"
#endif

// ============================================
// Time-Based Learning Algorithm Helper Functions
// ============================================
namespace LearningAlgorithm {

// Calculate water level before watering based on fill duration
inline float calculateWaterLevelBefore(unsigned long fillDuration,
                                       unsigned long baselineFillDuration) {
  if (baselineFillDuration == 0)
    return 0.0;

  float fillRatio = (float)fillDuration / (float)baselineFillDuration;

  // Water level before = 100% - (fill ratio * 100%)
  // If fillRatio = 1.0 (full fill) → was 0% (empty)
  // If fillRatio = 0.5 (half fill) → was 50% (half full)
  float waterLevelBefore = 100.0f - (fillRatio * 100.0f);
  return (waterLevelBefore < 0.0f) ? 0.0f : waterLevelBefore;
}

// Calculate estimated time to empty based on fill ratio and time since last
// watering
inline unsigned long
calculateEmptyDuration(unsigned long fillDuration,
                       unsigned long baselineFillDuration,
                       unsigned long timeSinceLastWatering) {
  if (fillDuration == 0 || baselineFillDuration == 0)
    return 0;

  float fillRatio = (float)fillDuration / (float)baselineFillDuration;

  // If tray was empty (fillRatio ≈ 1.0)
  if (fillRatio >= LEARNING_EMPTY_THRESHOLD) {
    return timeSinceLastWatering;
  }

  // If tray had water remaining, calculate consumption rate
  // emptyToFullDuration = timeSinceLastWatering / fillRatio
  unsigned long emptyDuration =
      (unsigned long)((float)timeSinceLastWatering / fillRatio);

  return emptyDuration;
}

// Format time duration for display (ms to human-readable)
inline String formatDuration(unsigned long milliseconds) {
  unsigned long seconds = milliseconds / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;

  if (days > 0) {
    return String(days) + "d " + String(hours % 24) + "h";
  } else if (hours > 0) {
    return String(hours) + "h " + String(minutes % 60) + "m";
  } else if (minutes > 0) {
    return String(minutes) + "m " + String(seconds % 60) + "s";
  } else {
    return String(seconds) + "." + String((milliseconds % 1000) / 100) + "s";
  }
}
} // namespace LearningAlgorithm

#endif // LEARNING_ALGORITHM_H

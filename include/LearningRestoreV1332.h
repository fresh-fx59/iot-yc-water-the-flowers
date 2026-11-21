#ifndef LEARNING_RESTORE_V1332_H
#define LEARNING_RESTORE_V1332_H

// v1.33.2 one-time restore of the 6-tray rig's learning data.
//
// WHY: v1.33.0 added root keys to the learning JSON while the parse buffer
// stayed StaticJsonDocument<2048>. v1.33.1 boot (2026-11-21 22:21:52) failed with
// "Failed to parse JSON: NoMemory", treated the rig as a fresh device and ran
// a full "Boot Calibration" watering of all trays, overwriting the file.
// These values are the last good state, read from Prometheus
// (job="esp32_watering") at 2026-11-21 22:15:00 +03 (epoch 1795288500), with the
// operator-approved v1.33.0/v1.33.1 intervals (tray 6 = 5 days).
//
// RIG-SPECIFIC: this table belongs to the 6-tray rig on the /v1/firmware/rig/
// channel. It runs once (flag "restoreV1332" in the learning JSON).

namespace LearningRestoreV1332 {

const unsigned long BASELINE_MS[6]  = {30458, 30457, 14734, 24582, 22075, 17698};
const unsigned long LAST_FILL_MS[6] = {30352, 18639, 6407, 13630, 7148, 13233};
const float MULTIPLIER[6]           = {9.75f, 10.5f, 7.875f, 9.0f, 10.5f, 5.0f};
const int TOTAL_CYCLES[6]           = {60, 51, 58, 45, 53, 92};
// Real time (epoch seconds) of each tray's last completed watering before the loss.
const unsigned long LAST_WATER_EPOCH[6] = {1795010411, 1794772360, 1794728615, 1795268667, 1795166234, 1795166278};

// Any RTC/NTP epoch below this is "clock not set" (before 2026-01-01).
const unsigned long MIN_VALID_EPOCH = 1767225600UL;

// Age (ms) of tray i's last watering, restored from the table. Returns 0 when the
// clock is not valid or the stored time is in the future (caller then falls back).
inline unsigned long restoredAgeMs(int i, unsigned long nowEpoch) {
  if (i < 0 || i > 5) return 0;
  if (nowEpoch < MIN_VALID_EPOCH || nowEpoch < LAST_WATER_EPOCH[i]) return 0;
  return (nowEpoch - LAST_WATER_EPOCH[i]) * 1000UL;
}

// Use the MORE RECENT of the restored watering and a watering the device logged
// after the loss (the 2026-11-21 boot calibration fills count as real waterings).
// hasCurrent=false or restored=0 handled; returns 0 = "watered now".
inline unsigned long pickAgeMs(bool hasCurrent, unsigned long currentAgeMs,
                               unsigned long restoredMs) {
  if (restoredMs == 0) return hasCurrent ? currentAgeMs : 0;
  if (!hasCurrent) return restoredMs;
  return currentAgeMs < restoredMs ? currentAgeMs : restoredMs;
}

}  // namespace LearningRestoreV1332

#endif  // LEARNING_RESTORE_V1332_H

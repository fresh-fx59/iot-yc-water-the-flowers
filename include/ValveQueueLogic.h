#ifndef VALVE_QUEUE_LOGIC_H
#define VALVE_QUEUE_LOGIC_H

#include <Arduino.h>

namespace ValveQueueLogic {

struct QueueEntry {
  int valveIndex;       // 0-5
  String triggerType;   // "Auto" | "Manual" | "Telegram" | "Sequential"
  bool force;           // skip learning-interval gate
};

inline bool contains(const QueueEntry* queue, int length, int valveIndex) {
  for (int i = 0; i < length; i++) {
    if (queue[i].valveIndex == valveIndex) return true;
  }
  return false;
}

// Returns true if appended, false if dedupe or capacity full.
// First-wins on dedupe: original entry's triggerType/force preserved.
inline bool enqueue(QueueEntry* queue, int& length, int capacity,
                    const QueueEntry& entry) {
  if (length >= capacity) return false;
  if (contains(queue, length, entry.valveIndex)) return false;
  queue[length++] = entry;
  return true;
}

// Returns true if head was popped into out; false if queue empty.
inline bool dequeue(QueueEntry* queue, int& length, QueueEntry& out) {
  if (length == 0) return false;
  out = queue[0];
  for (int i = 1; i < length; i++) {
    queue[i - 1] = queue[i];
  }
  length--;
  return true;
}

// Returns true if an entry for valveIndex was found and removed.
inline bool remove(QueueEntry* queue, int& length, int valveIndex) {
  for (int i = 0; i < length; i++) {
    if (queue[i].valveIndex == valveIndex) {
      for (int j = i + 1; j < length; j++) {
        queue[j - 1] = queue[j];
      }
      length--;
      return true;
    }
  }
  return false;
}

inline void clear(int& length) { length = 0; }

// All three conditions must hold before the next entry may be popped.
inline bool canDequeue(unsigned long currentTime, unsigned long nextReadyTime,
                       int currentlyActiveValve, int queueLength) {
  return currentlyActiveValve == -1 && queueLength > 0 &&
         currentTime >= nextReadyTime;
}

}  // namespace ValveQueueLogic

#endif  // VALVE_QUEUE_LOGIC_H

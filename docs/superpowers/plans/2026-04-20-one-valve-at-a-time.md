# One-Valve-At-A-Time Constraint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce a system-wide invariant that at most one valve is non-IDLE at a time, with a configurable 30-second inter-valve gap, by funneling every watering request through a universal FIFO queue.

**Architecture:** Extract pure queue primitives into a new `ValveQueueLogic` namespace (unit-testable, matches codebase pattern). Add queue state and orchestration methods (`enqueueValve`, `beginValveCycle`, `processQueue`) to `WateringSystem`. Refactor `startWatering` into a validate-then-enqueue entry point; move the phase-transition + Telegram session start into the dequeue-time hook `beginValveCycle`. Remove the blocking `delay(1000)` and `sequentialMode`/`sequenceValves[]`/`startNextInSequence` machinery — the new queue subsumes it.

**Tech Stack:** C++ (Arduino-flavored), PlatformIO, Unity test framework, ArduinoFake, ESP32-S3

**Design doc:** `docs/superpowers/specs/2026-04-20-one-valve-at-a-time-design.md`

---

## Task 1: Add inter-valve gap constant

**Files:**
- Modify: `include/config.h`

- [ ] **Step 1: Add constant**

Find the timing constants block in `config.h` (near `VALVE_NORMAL_TIMEOUTS`, `VALVE_EMERGENCY_TIMEOUTS`). Add:

```cpp
// Universal inter-valve gap — pause between finishing one valve and starting
// the next queued valve. Gives the pump pressure and sensors time to settle so
// every cycle sees the same flow rate (required for stable learning baselines).
const unsigned long INTER_VALVE_GAP_MS = 30000;  // 30 seconds
```

- [ ] **Step 2: Verify it compiles**

```bash
pio run -e native
```

Expected: compiles clean, no warnings about the new constant.

- [ ] **Step 3: Commit**

```bash
git add include/config.h
git commit -m "v1.24.0: add INTER_VALVE_GAP_MS config constant"
```

---

## Task 2: Create pure queue primitives with tests

**Files:**
- Create: `include/ValveQueueLogic.h`
- Modify: `test/test_native_all.cpp`

- [ ] **Step 1: Write failing tests for queue primitives**

Add to `test/test_native_all.cpp` above `setUp()`:

```cpp
#include "ValveQueueLogic.h"

void test_queue_empty_by_default(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    TEST_ASSERT_FALSE(ValveQueueLogic::contains(q, len, 3));
}

void test_queue_enqueue_appends(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    ValveQueueLogic::QueueEntry entry = {2, "Manual", false};
    bool added = ValveQueueLogic::enqueue(q, len, 6, entry);
    TEST_ASSERT_TRUE(added);
    TEST_ASSERT_EQUAL(1, len);
    TEST_ASSERT_EQUAL(2, q[0].valveIndex);
    TEST_ASSERT_EQUAL_STRING("Manual", q[0].triggerType.c_str());
}

void test_queue_dedupes_same_valve(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    ValveQueueLogic::enqueue(q, len, 6, {2, "Manual", false});
    bool added = ValveQueueLogic::enqueue(q, len, 6, {2, "Auto", true});
    TEST_ASSERT_FALSE(added);
    TEST_ASSERT_EQUAL(1, len);
    TEST_ASSERT_EQUAL_STRING("Manual", q[0].triggerType.c_str());  // first wins
}

void test_queue_fifo_order(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    ValveQueueLogic::enqueue(q, len, 6, {1, "Manual", false});
    ValveQueueLogic::enqueue(q, len, 6, {3, "Manual", false});
    ValveQueueLogic::enqueue(q, len, 6, {5, "Manual", false});
    ValveQueueLogic::QueueEntry out;
    TEST_ASSERT_TRUE(ValveQueueLogic::dequeue(q, len, out));
    TEST_ASSERT_EQUAL(1, out.valveIndex);
    TEST_ASSERT_EQUAL(2, len);
    TEST_ASSERT_EQUAL(3, q[0].valveIndex);
    TEST_ASSERT_EQUAL(5, q[1].valveIndex);
}

void test_queue_dequeue_empty_returns_false(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    ValveQueueLogic::QueueEntry out;
    TEST_ASSERT_FALSE(ValveQueueLogic::dequeue(q, len, out));
}

void test_queue_remove_by_index(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    ValveQueueLogic::enqueue(q, len, 6, {1, "Manual", false});
    ValveQueueLogic::enqueue(q, len, 6, {3, "Manual", false});
    ValveQueueLogic::enqueue(q, len, 6, {5, "Manual", false});
    bool removed = ValveQueueLogic::remove(q, len, 3);
    TEST_ASSERT_TRUE(removed);
    TEST_ASSERT_EQUAL(2, len);
    TEST_ASSERT_EQUAL(1, q[0].valveIndex);
    TEST_ASSERT_EQUAL(5, q[1].valveIndex);
}

void test_queue_remove_missing_returns_false(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    ValveQueueLogic::enqueue(q, len, 6, {1, "Manual", false});
    TEST_ASSERT_FALSE(ValveQueueLogic::remove(q, len, 5));
    TEST_ASSERT_EQUAL(1, len);
}

void test_queue_clear(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    ValveQueueLogic::enqueue(q, len, 6, {1, "Manual", false});
    ValveQueueLogic::enqueue(q, len, 6, {3, "Manual", false});
    ValveQueueLogic::clear(len);
    TEST_ASSERT_EQUAL(0, len);
}

void test_queue_enqueue_full_returns_false(void) {
    ValveQueueLogic::QueueEntry q[6];
    int len = 0;
    for (int i = 0; i < 6; i++) {
        ValveQueueLogic::enqueue(q, len, 6, {i, "Manual", false});
    }
    // Try to add a 7th (impossible since dedupe stops at 6 unique valves,
    // but cover the capacity guard explicitly)
    ValveQueueLogic::QueueEntry dup = {0, "Auto", false};
    bool added = ValveQueueLogic::enqueue(q, len, 6, dup);
    TEST_ASSERT_FALSE(added);
    TEST_ASSERT_EQUAL(6, len);
}

void test_queue_can_dequeue_all_conditions(void) {
    // canDequeue(currentTime, nextReadyTime, activeValve, queueLength)
    // Must have: activeValve == -1 AND queueLength > 0 AND currentTime >= nextReadyTime
    TEST_ASSERT_TRUE(ValveQueueLogic::canDequeue(100, 100, -1, 1));
    TEST_ASSERT_TRUE(ValveQueueLogic::canDequeue(200, 100, -1, 1));
    TEST_ASSERT_FALSE(ValveQueueLogic::canDequeue(50, 100, -1, 1));   // gap not elapsed
    TEST_ASSERT_FALSE(ValveQueueLogic::canDequeue(100, 100, 3, 1));   // valve active
    TEST_ASSERT_FALSE(ValveQueueLogic::canDequeue(100, 100, -1, 0));  // empty
}
```

Register the new tests in `main()` at the bottom of `test_native_all.cpp` (near the other `RUN_TEST(...)` calls):

```cpp
    // ValveQueueLogic tests
    RUN_TEST(test_queue_empty_by_default);
    RUN_TEST(test_queue_enqueue_appends);
    RUN_TEST(test_queue_dedupes_same_valve);
    RUN_TEST(test_queue_fifo_order);
    RUN_TEST(test_queue_dequeue_empty_returns_false);
    RUN_TEST(test_queue_remove_by_index);
    RUN_TEST(test_queue_remove_missing_returns_false);
    RUN_TEST(test_queue_clear);
    RUN_TEST(test_queue_enqueue_full_returns_false);
    RUN_TEST(test_queue_can_dequeue_all_conditions);
```

- [ ] **Step 2: Run tests — expect failure**

```bash
pio test -e native
```

Expected: compile error — `ValveQueueLogic.h: No such file or directory`.

- [ ] **Step 3: Create `include/ValveQueueLogic.h`**

```cpp
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
```

- [ ] **Step 4: Run tests — expect pass**

```bash
pio test -e native
```

Expected: all new `test_queue_*` tests PASS; existing 36 tests still pass.

- [ ] **Step 5: Commit**

```bash
git add include/ValveQueueLogic.h test/test_native_all.cpp
git commit -m "v1.24.0: add ValveQueueLogic pure primitives with unit tests"
```

---

## Task 3: Add queue state to WateringSystem (no behavior wired yet)

**Files:**
- Modify: `include/WateringSystem.h`

Goal: declare new fields, initialize them, include the new header. Existing behavior unchanged (queue stays empty). This is a safe scaffolding commit.

- [ ] **Step 1: Include the queue header**

In `include/WateringSystem.h` near the top, with the other includes:

```cpp
#include "ValveQueueLogic.h"
```

- [ ] **Step 2: Add private fields**

Find the `// Sequential watering state` block (around line 56). Leave the existing fields in place for now (we'll remove them in Task 9). Immediately after them, add:

```cpp
  // Universal single-valve queue (replaces sequentialMode-only machinery).
  // At most one valve may be non-IDLE at a time; others wait here.
  ValveQueueLogic::QueueEntry valveQueue[NUM_VALVES];
  int valveQueueLength;
  int currentlyActiveValve;         // -1 if no valve is actively cycling
  unsigned long nextValveReadyTime; // earliest millis() allowed for next dequeue
  bool batchSessionActive;          // true while a multi-valve batch is draining
```

- [ ] **Step 3: Initialize in constructor**

Find the `WateringSystem()` constructor initializer list (around line 95). Add the new fields at the end of the list (preserving existing order):

```cpp
        valveQueueLength(0), currentlyActiveValve(-1),
        nextValveReadyTime(0), batchSessionActive(false)
```

- [ ] **Step 4: Verify compile**

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: clean build, no behavior change.

- [ ] **Step 5: Verify existing tests still pass**

```bash
pio test -e native
```

Expected: all 46 tests pass (36 original + 10 new queue tests from Task 2).

- [ ] **Step 6: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: scaffold queue state in WateringSystem (no behavior yet)"
```

---

## Task 4: Extract dequeue-time startup into `beginValveCycle()`

**Files:**
- Modify: `include/WateringSystem.h`

Goal: isolate the "actually begin this valve's cycle" logic so it can be called from both the queue drain and (temporarily) directly from `startWatering`. This is a pure refactor — behavior unchanged.

- [ ] **Step 1: Declare the new private method**

In the class body (near other private method declarations, e.g. around line 200):

```cpp
  // Called at dequeue time — actually begins a valve cycle after all gates
  // have passed. Sets the active-valve marker, starts the Telegram session,
  // and transitions the valve's state machine to PHASE_OPENING_VALVE.
  void beginValveCycle(const ValveQueueLogic::QueueEntry& entry);
```

- [ ] **Step 2: Implement `beginValveCycle`**

Add the inline definition near the existing `startWatering` implementation (around line 1170):

```cpp
inline void WateringSystem::beginValveCycle(
    const ValveQueueLogic::QueueEntry& entry) {
  int valveIndex = entry.valveIndex;
  ValveController *valve = valves[valveIndex];

  currentlyActiveValve = valveIndex;

  if (entry.triggerType == "Auto") {
    autoWateringValveIndex = valveIndex;
  }

  // Start per-cycle Telegram session if one isn't already running.
  // Batch sessions (triggerType == "Sequential") are started by the batch
  // entry point instead — see startSequentialWatering.
  if (!telegramSessionActive && entry.triggerType != "Sequential") {
    String sessionLabel = entry.triggerType;
    if (entry.triggerType == "Auto") {
      sessionLabel = "Auto (Tray " + String(valveIndex + 1) + ")";
    }
    startTelegramSession(sessionLabel);

    String trayNumber = String(valveIndex + 1);
    queueTelegramNotification(
        TelegramNotifier::formatWateringStarted(entry.triggerType, trayNumber));
  }

  // Transition state machine — this is what the old startWatering tail did.
  valve->phase = PHASE_OPENING_VALVE;
  valve->valveOpenTime = millis();
  valve->wateringRequested = true;
  valve->timeoutOccurred = false;

  if (g_metricsLog) {
    g_metricsLog("info", "queue: dequeued valve " + String(valveIndex) +
                             " (trigger=" + entry.triggerType + ")");
  }

  DebugHelper::debug("▶ beginValveCycle: valve " + String(valveIndex) +
                     " (trigger=" + entry.triggerType + ")");
}
```

**Note:** the exact `valve->phase = ...` / `valveOpenTime` / `wateringRequested` lines must match what `startWatering()` currently does at the end of its body. Open `startWatering` (around line 1100-1170) and copy the final phase-transition statements verbatim so behavior is preserved. Do NOT remove them from `startWatering` yet — Task 7 does that.

- [ ] **Step 3: Compile check**

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: clean build. `beginValveCycle` is defined but not called yet.

- [ ] **Step 4: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: extract beginValveCycle dequeue-time helper"
```

---

## Task 5: Add `enqueueValve()` helper

**Files:**
- Modify: `include/WateringSystem.h`

- [ ] **Step 1: Declare private method**

In the class body near `beginValveCycle`:

```cpp
  // Add a valve to the queue after all startWatering gates have passed.
  // Dedupes: if the valve is already active or already queued, this is a noop.
  void enqueueValve(int valveIndex, const String& triggerType, bool force);
```

- [ ] **Step 2: Implement**

Inline definition next to `beginValveCycle`:

```cpp
inline void WateringSystem::enqueueValve(int valveIndex,
                                          const String& triggerType,
                                          bool force) {
  if (valveIndex == currentlyActiveValve) {
    DebugHelper::debug("queue: valve " + String(valveIndex) +
                       " already active — skip enqueue");
    return;
  }

  ValveQueueLogic::QueueEntry entry{valveIndex, triggerType, force};
  bool added = ValveQueueLogic::enqueue(valveQueue, valveQueueLength,
                                         NUM_VALVES, entry);
  if (!added) {
    DebugHelper::debug("queue: valve " + String(valveIndex) +
                       " already queued — skip enqueue");
    return;
  }

  if (g_metricsLog) {
    g_metricsLog("info", "queue: enqueued valve " + String(valveIndex) +
                             " (trigger=" + triggerType + ")");
  }
  DebugHelper::debug("⊕ enqueued valve " + String(valveIndex) +
                     " (trigger=" + triggerType + ", queue=" +
                     String(valveQueueLength) + ")");
}
```

- [ ] **Step 3: Compile check**

```bash
pio run -e esp32-s3-devkitc-1
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: add enqueueValve helper with dedupe"
```

---

## Task 6: Add `processQueue()` and wire into loop

**Files:**
- Modify: `include/WateringSystem.h`

- [ ] **Step 1: Declare**

```cpp
  // Per-loop queue drain. Detects valve-completion edge, starts gap timer,
  // and dequeues the next entry when all gates allow. Safe to call every tick.
  void processQueue(unsigned long currentTime);
```

- [ ] **Step 2: Implement**

```cpp
inline void WateringSystem::processQueue(unsigned long currentTime) {
  // Step 1: Detect "active valve just became idle" edge. Covers normal
  // completion, early aborts (initial-rain wet), timeouts, and emergency
  // shutdowns — anything that lands the valve in PHASE_IDLE.
  if (currentlyActiveValve != -1 &&
      valves[currentlyActiveValve]->phase == PHASE_IDLE) {
    DebugHelper::debug("↻ valve " + String(currentlyActiveValve) +
                       " idle — gap timer started (" +
                       String(INTER_VALVE_GAP_MS / 1000) + "s)");
    currentlyActiveValve = -1;
    nextValveReadyTime = currentTime + INTER_VALVE_GAP_MS;
  }

  // Step 2: Dequeue when allowed.
  if (!ValveQueueLogic::canDequeue(currentTime, nextValveReadyTime,
                                   currentlyActiveValve, valveQueueLength)) {
    return;
  }

  // Safety gates (defer but don't drop).
  if (overflowDetected || waterLevelLow || haltMode) {
    if (g_metricsLog) {
      String reason = overflowDetected ? "overflow"
                                       : (waterLevelLow ? "water_low" : "halt");
      g_metricsLog("warn",
                   "queue: dequeue deferred — safety gate (" + reason + ")");
    }
    return;
  }

  // Peek head, then re-check learning-interval for non-force entries
  // (may be stale after long waits behind safety gates).
  ValveQueueLogic::QueueEntry head = valveQueue[0];
  ValveController *valve = valves[head.valveIndex];

  if (!head.force && valve->isCalibrated && valve->emptyToFullDuration > 0 &&
      valve->lastWateringCompleteTime > 0 &&
      valve->lastWateringCompleteTime <= currentTime) {
    unsigned long timeSince = currentTime - valve->lastWateringCompleteTime;
    if (timeSince < valve->emptyToFullDuration) {
      ValveQueueLogic::QueueEntry drop;
      ValveQueueLogic::dequeue(valveQueue, valveQueueLength, drop);
      if (g_metricsLog) {
        g_metricsLog("info", "queue: dropped valve " + String(head.valveIndex) +
                                 " at dequeue — no longer due (learning)");
      }
      return;
    }
  }

  // Pop and start.
  ValveQueueLogic::QueueEntry entry;
  ValveQueueLogic::dequeue(valveQueue, valveQueueLength, entry);
  beginValveCycle(entry);
}
```

- [ ] **Step 3: Wire into `processWateringLoop`**

Find `processWateringLoop()` (around line 541). Add a call to `processQueue(currentTime)` after the safety checks but before `processValve` loop. Locate this block:

```cpp
  // Check for automatic watering (time-based)
  if (!sequentialMode) {
    checkAutoWatering(currentTime);
  }
```

Insert after the `checkAutoWatering` call, before the `for (int i = 0; i < NUM_VALVES; i++) { processValve(...) }` loop:

```cpp
  // Drain queue: start next valve if gap elapsed and no valve is active.
  processQueue(currentTime);
```

Do **not** remove any other code in this function yet — Task 11 handles cleanup.

- [ ] **Step 4: Verify compile and tests**

```bash
pio run -e esp32-s3-devkitc-1 && pio test -e native
```

Expected: compiles; all existing tests still pass. Queue remains empty (no call path enqueues anything yet).

- [ ] **Step 5: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: add processQueue drain hook (not yet producing)"
```

---

## Task 7: Route `startWatering()` through the queue

**Files:**
- Modify: `include/WateringSystem.h`

This is the behavior-flip commit. After this, every manual/API/Telegram call flows through the queue.

- [ ] **Step 1: Locate the tail of `startWatering()`**

Open `include/WateringSystem.h` around line 1100-1170. Find the section where — after all safety and learning gates pass — the code does:

```cpp
  valve->phase = PHASE_OPENING_VALVE;
  valve->valveOpenTime = millis();
  valve->wateringRequested = true;
  valve->timeoutOccurred = false;
```

(The exact statements may include Telegram session start and `autoWateringValveIndex = valveIndex`; note what's there.)

- [ ] **Step 2: Replace the tail with an enqueue call**

Delete those phase-transition lines and any Telegram session / autoWatering-index assignments that live *inside* `startWatering` after the gates pass. Replace them with:

```cpp
  // Determine trigger label from call context. startWatering is the single
  // public entry for manual/API/Telegram; 'Manual' is the default label.
  // Auto-watering callers pass force=true via checkAutoWatering but we still
  // want the "Auto" label — see checkAutoWatering changes in Task 8.
  String triggerType = "Manual";
  enqueueValve(valveIndex, triggerType, forceWatering);
```

**Important:** keep ALL the validation gates at the top of `startWatering` (overflow, water-level, halt, invalid index, learning-interval skip, future-timestamp safety). Only the final phase-transition tail moves.

- [ ] **Step 3: Build for ESP32 and run native tests**

```bash
pio run -e esp32-s3-devkitc-1 && pio test -e native
```

Expected: clean build; all 46 tests still pass.

- [ ] **Step 4: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: route startWatering through universal valve queue"
```

---

## Task 8: Move auto-watering session start into `beginValveCycle`

**Files:**
- Modify: `include/WateringSystem.h`

Goal: fix latent race where two auto valves firing in the same loop pass would overwrite `autoWateringValveIndex` and corrupt Telegram state. After this task, all session/index assignment happens exactly at dequeue time.

- [ ] **Step 1: Find `checkAutoWatering` auto-session block**

Around line 1005:

```cpp
      // Start Telegram session for auto-watering
      startTelegramSession("Auto (Tray " + String(i + 1) + ")");
      autoWateringValveIndex = i;

      // Queue start notification (non-blocking, sent from Core 0)
      String trayNumber = String(i + 1);
      queueTelegramNotification(TelegramNotifier::formatWateringStarted("Auto", trayNumber));

      startWatering(i);
```

- [ ] **Step 2: Introduce `requestWatering` as the gated entry point**

We need every trigger (Manual / Auto / Telegram) to pass through the same gate logic but carry its own trigger label. `enqueueValve` alone isn't enough because it skips the safety/learning gates that currently live inside `startWatering`. Solution: move the body of `startWatering` into a new `requestWatering(valveIndex, triggerType, force)`. `startWatering` becomes a one-liner shim.

Declare near `enqueueValve`:

```cpp
  // Internal gated entry point. Runs all safety + learning gates, then
  // enqueues with the given trigger label. All public/internal callers
  // (startWatering, checkAutoWatering, etc.) delegate here.
  void requestWatering(int valveIndex, const String& triggerType, bool force);
```

Rename the existing `startWatering` implementation to `requestWatering` and change its signature to accept `triggerType`. In its final enqueue line, use `triggerType` instead of the hard-coded `"Manual"` from Task 7:

```cpp
inline void WateringSystem::requestWatering(int valveIndex,
                                             const String& triggerType,
                                             bool force) {
  // All existing gates stay unchanged: overflow, water-level, halt,
  // invalid index, valve-not-idle skip, future-timestamp safety,
  // learning-interval skip (when !force).
  // ... (keep entire existing body from Task 7 refactor) ...

  // Tail (replacing the "Manual" hard-code from Task 7):
  enqueueValve(valveIndex, triggerType, force);
}
```

Then make `startWatering` a shim:

```cpp
inline void WateringSystem::startWatering(int valveIndex, bool forceWatering) {
  requestWatering(valveIndex, "Manual", forceWatering);
}
```

And update `checkAutoWatering` — remove the three pre-session lines (`startTelegramSession`, `autoWateringValveIndex = i`, `queueTelegramNotification`) and the `startWatering(i)` call; replace with:

```cpp
      if (g_metricsLog) g_metricsLog("info", "Auto-watering triggered: valve " + String(i));
      requestWatering(i, "Auto", /*force=*/false);
```

- [ ] **Step 3: Build + native tests**

```bash
pio run -e esp32-s3-devkitc-1 && pio test -e native
```

Expected: clean; all tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: centralize auto-watering session start in beginValveCycle"
```

---

## Task 9: Refactor `startSequentialWatering*()` to bulk-enqueue

**Files:**
- Modify: `include/WateringSystem.h`

Goal: `startSequentialWatering` becomes a thin shim that starts a batch Telegram session, sets `batchSessionActive = true`, and enqueues all target valves in order. Batch completion notification fires when the queue drains AND `batchSessionActive` is true.

- [ ] **Step 1: Replace `startSequentialWatering` body**

Find the existing implementation (around line 1167). Replace the entire function body with:

```cpp
inline void WateringSystem::startSequentialWatering(const String &triggerType) {
  DebugHelper::debug("\n╔═══════════════════════════════════════════╗");
  DebugHelper::debug("║  SEQUENTIAL WATERING — BULK ENQUEUE       ║");
  DebugHelper::debug("╚═══════════════════════════════════════════╝");

  // Safety gates up front — same as startWatering, but rejecting the whole
  // batch if blocked (rather than per-valve drop).
  if (overflowDetected) {
    DebugHelper::debug("🚨 Sequential blocked — OVERFLOW DETECTED");
    return;
  }
  if (waterLevelLow) {
    DebugHelper::debug("💧 Sequential blocked — WATER LEVEL LOW");
    return;
  }
  if (haltMode) {
    DebugHelper::debug("🛑 Sequential blocked — HALT MODE");
    return;
  }

  // Build the per-valve list (5→0 so tray 6 waters first — existing behavior)
  int targetValves[NUM_VALVES];
  int targetCount = NUM_VALVES;
  for (int i = 0; i < NUM_VALVES; i++) {
    targetValves[i] = NUM_VALVES - 1 - i;
  }

  // Start the batch session so completion notification fires when queue drains
  startTelegramSession(triggerType);
  batchSessionActive = true;

  String trayNumbers;
  for (int i = 0; i < targetCount; i++) {
    trayNumbers += String(targetValves[i] + 1);
    if (i < targetCount - 1) trayNumbers += ",";
  }
  queueTelegramNotification(
      TelegramNotifier::formatWateringStarted(triggerType, trayNumbers));

  // Enqueue in order. force=true matches prior startSequentialWatering
  // behavior (batch ignores learning interval).
  for (int i = 0; i < targetCount; i++) {
    enqueueValve(targetValves[i], "Sequential", /*force=*/true);
  }

  if (g_metricsLog) {
    g_metricsLog("info", "queue: bulk-enqueued " + String(targetCount) +
                             " valves (trigger=" + triggerType + ")");
  }
}
```

- [ ] **Step 2: Replace `startSequentialWateringCustom` body**

Same shape, using the caller's valve list. Around line 1227:

```cpp
inline void WateringSystem::startSequentialWateringCustom(
    int *valveIndices, int count, const String &triggerType) {
  if (count <= 0 || count > NUM_VALVES) return;

  if (overflowDetected || waterLevelLow || haltMode) {
    DebugHelper::debug("Sequential-custom blocked by safety gate");
    return;
  }

  startTelegramSession(triggerType);
  batchSessionActive = true;

  String trayNumbers;
  for (int i = 0; i < count; i++) {
    trayNumbers += String(valveIndices[i] + 1);
    if (i < count - 1) trayNumbers += ",";
  }
  queueTelegramNotification(
      TelegramNotifier::formatWateringStarted(triggerType, trayNumbers));

  for (int i = 0; i < count; i++) {
    enqueueValve(valveIndices[i], "Sequential", /*force=*/true);
  }

  if (g_metricsLog) {
    g_metricsLog("info", "queue: bulk-enqueued " + String(count) +
                             " valves (custom, trigger=" + triggerType + ")");
  }
}
```

- [ ] **Step 3: Emit batch-complete when queue drains**

Find `processQueue()` (from Task 6). Update the "idle edge" block to detect batch completion:

```cpp
  if (currentlyActiveValve != -1 &&
      valves[currentlyActiveValve]->phase == PHASE_IDLE) {
    DebugHelper::debug("↻ valve " + String(currentlyActiveValve) +
                       " idle — gap timer started (" +
                       String(INTER_VALVE_GAP_MS / 1000) + "s)");
    currentlyActiveValve = -1;
    nextValveReadyTime = currentTime + INTER_VALVE_GAP_MS;

    // Batch completion: active valve just finished AND queue is empty AND
    // we're inside a batch session. Emit the completion notification once.
    if (batchSessionActive && valveQueueLength == 0) {
      DebugHelper::debug("\n╔═══════════════════════════════════════════╗");
      DebugHelper::debug("║  SEQUENTIAL BATCH COMPLETE ✓              ║");
      DebugHelper::debug("╚═══════════════════════════════════════════╝");

      if (telegramSessionActive) {
        String results[NUM_VALVES][3];
        int resultCount = 0;
        for (int i = 0; i < NUM_VALVES; i++) {
          if (sessionData[i].active) {
            results[resultCount][0] = String(sessionData[i].trayNumber);
            results[resultCount][1] = String(sessionData[i].duration, 1);
            results[resultCount][2] = sessionData[i].status;
            resultCount++;
          }
        }
        queueTelegramNotification(
            TelegramNotifier::formatWateringComplete(results, resultCount));
        endTelegramSession();
        sendWateringSchedule("Updated Schedule");
      }
      batchSessionActive = false;
      publishStateChange("system", "sequential_complete");
    }
  }
```

- [ ] **Step 4: Build + native tests**

```bash
pio run -e esp32-s3-devkitc-1 && pio test -e native
```

Expected: clean build; all tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: route startSequentialWatering through bulk enqueue"
```

---

## Task 10: Update `stopWatering` and emergency paths to touch the queue

**Files:**
- Modify: `include/WateringSystem.h`

- [ ] **Step 1: Update `stopWatering(int)` to remove from queue**

Find `stopWatering(int valveIndex)`. At the top of the body, before the existing stop logic, add:

```cpp
  // Remove from queue if pending (never actually opened)
  if (ValveQueueLogic::remove(valveQueue, valveQueueLength, valveIndex)) {
    if (g_metricsLog) {
      g_metricsLog("info",
                   "queue: removed valve " + String(valveIndex) +
                       " (stop requested)");
    }
    DebugHelper::debug("⊖ removed queued valve " + String(valveIndex));
    return;  // nothing else to do — valve was only queued
  }
```

Leave the rest of `stopWatering(int)` unchanged (it handles the case where the valve is actively cycling).

- [ ] **Step 2: Update `emergencyStopAll()` to clear the queue**

Find `emergencyStopAll()` (grep for it). At the top of the body, add:

```cpp
  // Queued valves must NOT pop off once overflow clears — user must re-request.
  if (valveQueueLength > 0) {
    if (g_metricsLog) {
      g_metricsLog("warn", "queue: cleared on emergency (" +
                               String(valveQueueLength) + " entries dropped)");
    }
    ValveQueueLogic::clear(valveQueueLength);
  }
  batchSessionActive = false;
  currentlyActiveValve = -1;
  nextValveReadyTime = 0;
```

- [ ] **Step 3: Update the stop-all API path**

Find `handleStopApi()` in `include/api_handlers.h`. The `valve == "all"` branch currently loops `stopWatering(i)` for 0..5. That now correctly removes each queued entry and stops any active valve. No change needed here — verify by reading the handler.

- [ ] **Step 4: Build + native tests**

```bash
pio run -e esp32-s3-devkitc-1 && pio test -e native
```

Expected: clean build; all tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: integrate stopWatering + emergencyStopAll with queue"
```

---

## Task 11: Remove obsolete `sequentialMode` machinery

**Files:**
- Modify: `include/WateringSystem.h`

Goal: delete dead code (`sequenceValves[]`, `sequenceLength`, `currentSequenceIndex`, `sequentialMode`, `startNextInSequence()`, `stopSequentialWatering()`, and the `delay(1000)` block in `processWateringLoop`). The universal queue has subsumed their function.

- [ ] **Step 1: Remove private fields**

Delete these lines from the class body:

```cpp
  bool sequentialMode;
  int currentSequenceIndex;
  int sequenceValves[NUM_VALVES];
  int sequenceLength;
```

And the corresponding initializer list entries: `sequentialMode(false), currentSequenceIndex(0), sequenceLength(0)`.

- [ ] **Step 2: Remove the `delay(1000)` block in `processWateringLoop`**

Around line 567 in the current file, find and delete:

```cpp
  // Handle sequential watering
  if (sequentialMode && currentSequenceIndex > 0) {
    int lastValveIndex = sequenceValves[currentSequenceIndex - 1];
    if (isValveComplete(lastValveIndex)) {
      delay(1000); // Small delay between valves
      startNextInSequence();
    }
  }
```

Also delete the `if (!sequentialMode)` guard around `checkAutoWatering(currentTime)` — auto-watering is always allowed; dedupe inside the queue handles concurrency:

```cpp
  // Before:
  if (!sequentialMode) {
    checkAutoWatering(currentTime);
  }
  // After:
  checkAutoWatering(currentTime);
```

- [ ] **Step 3: Remove `startNextInSequence()` and `stopSequentialWatering()`**

Delete both method declarations from the class body AND their inline implementations further down the file. `startNextInSequence` lived around line 1314; `stopSequentialWatering` around line 1293.

- [ ] **Step 4: Remove the constructor-body `sequenceValves[i] = 0` initialization loop**

Around line 108, find and remove:

```cpp
    for (int i = 0; i < NUM_VALVES; i++) {
      sequenceValves[i] = 0;
    }
```

- [ ] **Step 5: Grep for any remaining references**

```bash
grep -n "sequentialMode\|sequenceValves\|sequenceLength\|currentSequenceIndex\|startNextInSequence\|stopSequentialWatering" include/ src/
```

Expected: zero matches.

- [ ] **Step 6: Build + native tests**

```bash
pio run -e esp32-s3-devkitc-1 && pio test -e native
```

Expected: clean build; all tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: remove obsolete sequentialMode/startNextInSequence machinery"
```

---

## Task 12: Surface queue state in `/api/status`

**Files:**
- Modify: `include/WateringSystem.h` (the `publishCurrentState` / state-building function)

- [ ] **Step 1: Locate the state JSON builder**

Find the function that builds the `/api/status` JSON — grep for `StaticJsonDocument` or `JsonDocument` in `publishCurrentState`:

```bash
grep -n "publishCurrentState\|JsonDocument" include/WateringSystem.h | head
```

- [ ] **Step 2: Add fields to the root object**

Inside `publishCurrentState` (before the `serializeJson(doc, ...)` call), add:

```cpp
  // Universal single-valve queue state
  JsonArray queueArr = doc.createNestedArray("queue");
  for (int i = 0; i < valveQueueLength; i++) {
    queueArr.add(valveQueue[i].valveIndex + 1);  // 1-indexed for UI
  }
  doc["active_valve"] = (currentlyActiveValve == -1)
                             ? 0
                             : (currentlyActiveValve + 1);  // 1-indexed
  unsigned long gapRemaining = 0;
  unsigned long now = millis();
  if (nextValveReadyTime > now && currentlyActiveValve == -1) {
    gapRemaining = nextValveReadyTime - now;
  }
  doc["inter_valve_gap_remaining_ms"] = gapRemaining;

  // Redefined: true iff anything is queued or active. Keeps the existing
  // field name for UI compatibility as a "system busy" indicator.
  doc["sequential_mode"] =
      (valveQueueLength > 0) || (currentlyActiveValve != -1);
```

- [ ] **Step 3: Build and flash to a test board if available**

```bash
pio run -e esp32-s3-devkitc-1 && pio test -e native
```

Expected: clean; tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/WateringSystem.h
git commit -m "v1.24.0: expose queue/active_valve/gap in /api/status"
```

---

## Task 13: Audit and update web UI

**Files:**
- Modify: `data/web/prod/js/app.js` (and possibly `index.html` / `css/style.css`)

- [ ] **Step 1: Audit existing `sequential_mode` usage**

```bash
grep -rn "sequential_mode\|sequentialMode" data/web/prod/
```

- [ ] **Step 2: Verify the new semantics are compatible**

Open `data/web/prod/js/app.js`. For each hit:
- If the code reads `sequential_mode` as a boolean "is a batch running" indicator, the new semantics (`queue_length > 0 OR active_valve > 0`) are a strict superset — it will now also be true for single manual waterings in progress. This is usually a compatible change; verify no logic special-cases "exactly one valve in a batch" (e.g., disabling the start-all button).
- If problematic, split logic by reading the new fields: `data.queue.length > 0` for "batch queued", `data.active_valve > 0` for "any valve active".

- [ ] **Step 3: Add queue display (optional but recommended)**

Add a compact panel showing: active valve number, queue preview, and gap countdown. Keep styling consistent with existing `index.html` structure. If scope creep, defer to a follow-up PR and note it in CLAUDE.md.

- [ ] **Step 4: Rebuild filesystem and flash test board**

```bash
pio run -t buildfs -e esp32-s3-devkitc-1
pio run -t uploadfs -e esp32-s3-devkitc-1-test   # use test env so prod isn't disturbed
```

- [ ] **Step 5: Smoke test in browser**

Open the device's web page. Verify:
- Idle state: `active_valve` shows 0, queue empty, sequential_mode false.
- Fire two `/api/water?valve=1` then `/api/water?valve=3` in rapid succession → queue shows [3], active_valve = 1.
- After valve 1 completes: countdown visible, then valve 3 starts.

- [ ] **Step 6: Commit**

```bash
git add data/web/prod/
git commit -m "v1.24.0: display queue/active_valve/gap in web UI"
```

---

## Task 14: Hardware smoke test and version bump

**Files:**
- Modify: `include/config.h` (version string)
- Modify: `CLAUDE.md`

- [ ] **Step 1: Bump version**

In `include/config.h` find the existing version string (around line 10) and bump:

```cpp
#define FIRMWARE_VERSION "1.24.0"
```

- [ ] **Step 2: Update CLAUDE.md**

In the "Architecture" → "Watering Flow" section of `CLAUDE.md`, add a line:

```
**Single-valve invariant (v1.24.0+)**: At most one valve is non-IDLE at any
time. All watering requests (manual/API/Telegram/auto/sequential batch) funnel
through a universal FIFO queue in WateringSystem; the next queued valve starts
INTER_VALVE_GAP_MS (30s default) after the previous completes. Learning
baselines depend on this — concurrent valves corrupt flow-rate calibration.
```

Update the **Version** line at the top of CLAUDE.md: `**Version**: 1.24.0`.

- [ ] **Step 3: Full clean deploy to real hardware**

```bash
pio run -t erase -e esp32-s3-devkitc-1 && \
  pio run -t buildfs -e esp32-s3-devkitc-1 && \
  pio run -t uploadfs -e esp32-s3-devkitc-1 && \
  pio run -t upload -e esp32-s3-devkitc-1 && \
  pio device monitor -b 115200 --raw
```

- [ ] **Step 4: Run smoke scenarios**

With serial monitor open:

1. **Rapid manual requests (different valves):** via web UI or Telegram issue `/water 1` then `/water 3` within 1 second. Expected serial:
   - `⊕ enqueued valve 0 (trigger=Manual, queue=1)` then `⊕ enqueued valve 2 (trigger=Manual, queue=2)`
   - Valve 0 cycles to IDLE → `↻ gap timer started (30s)`
   - 30s later → `▶ beginValveCycle: valve 2`

2. **Rapid manual requests (same valve):** issue `/water 1` twice. Expected: second shows `queue: valve 0 already queued — skip enqueue`.

3. **Sequential batch:** press "Start All" button. Expected: `queue: bulk-enqueued 6 valves (trigger=Manual)`, then 6 valves cycle with 30s gaps. Total time ~6 minutes. Batch-complete Telegram fires at end.

4. **Overflow mid-queue:** enqueue 3 valves via `/start_all` wait 20s, then short GPIO 42 to ground (simulate overflow). Expected: `queue: cleared on emergency (N entries dropped)`. After `/reset_overflow`, no phantom opens.

5. **Water-low mid-queue:** start a batch, then unplug water tank sensor (pull GPIO 19 LOW). After 11s: `queue: dequeue deferred — safety gate (water_low)`. Restore the sensor. Expected: queue resumes draining.

- [ ] **Step 5: If all scenarios pass, commit**

```bash
git add include/config.h CLAUDE.md
git commit -m "v1.24.0: document single-valve invariant and bump version"
```

- [ ] **Step 6: Tag and push (only after explicit user approval)**

Ask the user before pushing. If approved:

```bash
git tag v1.24.0
git push origin main --tags
```

---

## Self-review notes

- **Spec coverage:** every spec section has a task (data model → T3; enqueue/dequeue → T5/T6; startWatering refactor → T7; auto-watering fix → T8; sequential → T9; safety gates → T6/T10; stop semantics → T10; state JSON → T12; tests → T2; web UI → T13; docs → T14).
- **Known remaining risk:** the dequeue-time learning-skip check (T6) uses the same conditions as the enqueue-time check in `requestWatering`. Keep them in sync. If the learning algorithm changes, both call sites need updating — flag via a `// KEEP IN SYNC with requestWatering` comment at both sites.
- **Compilation guard in tests:** `ValveQueueLogic.h` uses `String` (from `<Arduino.h>`). The native test environment includes `ArduinoFake` which provides `String` — existing tests use it. Should work without extra setup.

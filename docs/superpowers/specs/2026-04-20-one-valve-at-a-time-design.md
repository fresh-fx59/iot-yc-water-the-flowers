# One-Valve-At-A-Time Constraint — Design

**Date:** 2026-04-20
**Status:** Design approved, pending implementation plan

## Problem

Water flow rate per valve changes when multiple valves are open simultaneously
(pump splits pressure across open paths). The adaptive-learning algorithm
(`LearningAlgorithm.h`, `WateringSystem::processLearningData`) assumes a stable
flow rate for its fill-duration baselines. Concurrent valve openings therefore
corrupt calibration and produce inconsistent watering.

The system today has no global mutual-exclusion between valves:

- `startWatering(valveIndex, force)` only rejects *re-opening the same valve*
  (`phase != PHASE_IDLE`). Different valves can be in `PHASE_WATERING`
  concurrently.
- `checkAutoWatering()` iterates all valves and invokes `startWatering(i)` for
  each overdue valve in the same loop pass — multiple auto-waterings can begin
  within a few loop ticks.
- Manual requests (REST `/api/water`, Telegram `/water N`) hit
  `startWatering` directly with no awareness of in-progress work.
- Existing `startSequentialWatering()` is the only code path that serializes,
  and it only applies to explicit "water all" requests.

## Goal

Enforce a system-wide invariant: **at most one valve is in any non-IDLE phase
at a time**, with a configurable inter-valve settle gap. All existing entry
points (manual API, Telegram, auto-watering, sequential batch, smart boot
watering) funnel through one universal single-valve queue.

## Non-goals

- Queue persistence across reboots. Consistent with existing non-persistent
  state (halt mode, overflow flag). Smart boot watering already re-enqueues
  overdue valves at startup.
- Priority ordering within the queue. FIFO is sufficient.
- Changing the state machine's per-valve phases (IDLE → OPENING_VALVE →
  WAITING_STABILIZATION → CHECKING_INITIAL_RAIN → WATERING → CLOSING_VALVE).
  Only the *entry point* into that sequence is gated.

## Design

### Data model changes (`WateringSystem.h`)

```cpp
struct QueueEntry {
  int valveIndex;         // 0-5
  String triggerType;     // "Auto" | "Manual" | "Telegram" | "Sequential"
  bool force;             // skip learning-interval gate
};

QueueEntry queue[NUM_VALVES];     // FIFO, bounded to 6 (one per valve)
int queueLength;
int currentlyActiveValve;         // -1 if none
unsigned long nextValveReadyTime; // earliest millis() allowed for next dequeue
```

**Removed:** `sequentialMode`, `sequenceValves[]`, `sequenceLength`,
`currentSequenceIndex`, the blocking `delay(1000)` in `processWateringLoop()`.

**Added to `config.h`:**

```cpp
const unsigned long INTER_VALVE_GAP_MS = 30000;  // 30s pause between valves
```

### Invariants

1. At most one valve has `phase != PHASE_IDLE` at any time.
2. Queue contains at most one entry per valve index (dedupe).
3. A valve is either `currentlyActiveValve` or queued, never both.
4. Dequeue only proceeds when `millis() >= nextValveReadyTime` AND
   `currentlyActiveValve == -1`.

### Control flow

**Public API `startWatering(i, force)`** — signature unchanged:

1. Safety gates (overflow / water-level / halt / invalid index) — unchanged.
2. Learning-interval skip check for `force == false` — unchanged.
3. On pass, call `enqueueValve(i, triggerType, force)`.

**`enqueueValve(i, triggerType, force)`** — new, private:

- If `i == currentlyActiveValve` or `i` already in queue → noop (dedupe;
  first-wins preserves original `triggerType`).
- Else append `QueueEntry` to queue, log `enqueue` event via `g_metricsLog`.

**`processQueue()`** — new, called each tick from `processWateringLoop()`:

1. If `currentlyActiveValve != -1` and that valve's phase is now `PHASE_IDLE`:
   - Clear `currentlyActiveValve`
   - Set `nextValveReadyTime = millis() + INTER_VALVE_GAP_MS`
2. If queue non-empty AND `currentlyActiveValve == -1` AND
   `millis() >= nextValveReadyTime` AND safety gates clear:
   - Pop head entry
   - Re-run learning-skip check for `force == false` entries (entry may be
     stale after long waits)
   - Call `beginValveCycle(entry)`

**`beginValveCycle(entry)`** — new, private:

- Set `currentlyActiveValve = entry.valveIndex`
- Start Telegram session with `entry.triggerType`
- Set `autoWateringValveIndex = entry.valveIndex` if `triggerType == "Auto"`
- Queue the start notification
- Transition the valve's state machine to `PHASE_OPENING_VALVE`

This is the *single* dequeue hook. All prior call sites of
`startTelegramSession()` and `autoWateringValveIndex` assignment move here:

- `checkAutoWatering()` drops its direct `startTelegramSession(...)` and
  `autoWateringValveIndex = i` — it just calls `startWatering(i)`.
- `startSequentialWatering*()` drops its direct session start — it just
  enqueues all target valves.
- Main.cpp "smart boot watering" is unaffected (already calls into
  `startSequentialWatering`).

### Idle-edge detection

`processQueue()` step 1 triggers on *any* transition to `PHASE_IDLE` — including
early aborts (rain detected at `CHECKING_INITIAL_RAIN`), timeouts, manual
stops, and emergency shutdowns via `globalSafetyWatchdog()`. This avoids the
bug where a non-watering completion leaks a stuck `currentlyActiveValve`.

### Safety-gate interactions

| Event | Queue behavior |
|---|---|
| Overflow detected (`emergencyStopAll`) | **Cleared.** Re-request required after reset. |
| Water level LOW | **Paused.** Resumes when level recovers. |
| Halt mode | **Paused.** Resumes on `/resume`. |
| Manual `stopWatering(i)` | Removes `i` from queue if present; stops if active. |
| API `/api/stop?valve=all` | Clears queue; stops active valve; resets gap timer. |
| Water level / halt clears | Queue drains naturally; each dequeue re-runs learning-skip for `force=false` entries. |

### Sequential watering

`startSequentialWatering(triggerType)` and `startSequentialWateringCustom()`
become thin shims that enqueue each target valve with `triggerType="Sequential"`
and `force=true`. The "sequential mode" concept disappears as a distinct state
— it's just "multiple queue entries enqueued at once".

**Note:** Full 6-valve "start all" now takes roughly 5 × 30s = 2.5 min longer
than before (was 5 × 1s between valves). Telegram session naming and totals
remain correct because each valve runs its own cycle.

### State JSON (`/api/status`)

Additive, backwards-compatible:

```json
{
  "queue": [2, 4],
  "active_valve": 3,
  "inter_valve_gap_remaining_ms": 18450,
  "sequential_mode": true
}
```

- `queue` — 1-indexed valve numbers waiting
- `active_valve` — 1-indexed; 0 if none
- `inter_valve_gap_remaining_ms` — 0 unless inside gap
- `sequential_mode` — retained for UI compatibility, redefined to
  `queue_length > 0 OR active_valve > 0`. Web UI
  (`data/web/prod/js/app.js`) uses it as a "system busy" indicator;
  redefinition preserves that intent. Audit during implementation.

### Observability (Loki via `g_metricsLog`)

New log events:
- `info` "queue: enqueued valve N (trigger=X)"
- `info` "queue: dequeued valve N → begin cycle"
- `info` "queue: dequeue skipped — not yet due (learning)"
- `warn` "queue: dequeue deferred — safety gate (overflow/water-low/halt)"
- `warn` "queue: cleared on emergency (overflow)"

## Testing

### Native unit tests (new `test/test_valve_queue.cpp`)

1. `enqueue_dedupes_same_valve`
2. `enqueue_dedupes_active_valve`
3. `dequeue_fifo_order`
4. `gap_respected` — no dequeue until `millis() >= nextValveReadyTime`
5. `safety_gate_pauses_queue` (water-low)
6. `halt_pauses_queue_resume_drains`
7. `emergency_clears_queue` (overflow)
8. `stop_valve_removes_from_queue`
9. `early_abort_clears_active` (wet sensor at CHECKING_INITIAL_RAIN)
10. `learning_skip_reevaluated_at_dequeue`
11. `sequential_preserves_first_trigger_on_dedupe` — manual-3 queued, then
    `startSequentialWatering` → dedupe preserves `triggerType="Manual"` on
    valve 3 entry.

Existing 36 tests must keep passing — public `startWatering()` surface is
preserved.

### Hardware smoke plan

- Three rapid `/api/water` calls on different valves → serial log shows each
  valve runs with ~30s gap between completion and next start.
- `startSequentialWatering()` → 6 valves with 30s gaps, ~6 minute total.
- Mid-queue overflow trip → queue cleared, no phantom opens after
  `/reset_overflow`.
- Mid-queue water-low trip → queue pauses; on refill, queue resumes.

## Risks

- **Behavioral change — Telegram session move.** Session start moves from
  enqueue to dequeue. Implementation must sweep all existing call sites
  (`checkAutoWatering`, `startSequentialWatering*`, main.cpp boot path). Miss
  one and notifications go missing/duplicate.
- **Web UI `sequential_mode` semantics.** Audit `data/web/prod/js/app.js`
  for strict batch-mode assumptions during implementation.
- **Gap timing in native tests.** Verify the existing mocked-`millis()`
  harness supports the gap-timer assertions before building out the full
  suite.

## Files affected

- `include/config.h` — add `INTER_VALVE_GAP_MS`
- `include/WateringSystem.h` — queue data model, `enqueueValve`,
  `processQueue`, `beginValveCycle`, refactor of `startWatering`,
  `checkAutoWatering`, `startSequentialWatering*`, `stopWatering`,
  `emergencyStopAll`, `publishCurrentState`
- `test/test_valve_queue.cpp` — new
- `data/web/prod/js/app.js` — audit for `sequential_mode` usage, add
  `queue` / `active_valve` display if useful
- `CLAUDE.md` — brief update noting new invariant and config constant

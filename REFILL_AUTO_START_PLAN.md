# Auto-Watering After Refill Implementation Plan

## Requirements
- Start watering automatically 10 seconds after successful water refill detection
- Refill is successful only after 5 consecutive HIGH readings from water level sensor (GPIO 19)
- Prevents false positives from sensor noise or brief water detection

## Current Implementation Analysis

### Water Level Sensor (GPIO 19)
- **Current behavior**: Single reading triggers immediate state change
- **Location**: `WateringSystem.h:616-758` - `checkWaterLevelSensor()`
- **Polling**: Every 100ms
- **State**: `waterLevelLow` flag blocks all watering operations
- **Recovery**: Currently resumes immediately on HIGH detection (no debouncing)

### Blocking Points
All four watering entry points check `waterLevelLow`:
1. `startWatering()` - line 825-829
2. `checkAutoWatering()` - line 776-779
3. `startSequentialWatering()` - line 959-963
4. `startSequentialWateringCustom()` - line 1022-1026

## Implementation Approach

### Phase 1: Add Consecutive Reading Counter
**File**: `WateringSystem.h`

**New State Variables** (add after line 85):
```cpp
int waterLevelConsecutiveHighReadings;  // Counter for stable HIGH readings
unsigned long waterRefillCompletedTime; // Timestamp when 5 consecutive readings achieved
bool waterRefillAutoWateringPending;    // Flag: waiting for 10s delay before auto-start
```

**Initialize in Constructor** (line 89-100):
```cpp
waterLevelConsecutiveHighReadings = 0;
waterRefillCompletedTime = 0;
waterRefillAutoWateringPending = false;
```

### Phase 2: Modify Water Level Detection Logic
**File**: `WateringSystem.h` - function `checkWaterLevelSensor()` (lines 616-758)

**Changes to LOW→HIGH transition** (currently lines 701-758):

1. **On each HIGH reading during LOW state**:
   - Increment `waterLevelConsecutiveHighReadings`
   - If counter reaches 5:
     - Set `waterLevelLow = false`
     - Set `waterRefillCompletedTime = millis()`
     - Set `waterRefillAutoWateringPending = true`
     - Send Telegram "water restored" notification
     - Reset `waterLevelConsecutiveHighReadings = 0`

2. **On any LOW reading while counting**:
   - Reset `waterLevelConsecutiveHighReadings = 0`
   - Keep `waterLevelLow = true`

**Pseudocode**:
```cpp
if (waterLevelLow) {  // Currently in LOW state
    if (currentReading == HIGH) {
        waterLevelConsecutiveHighReadings++;
        if (waterLevelConsecutiveHighReadings >= 5) {
            // Successful refill detected
            waterLevelLow = false;
            waterRefillCompletedTime = millis();
            waterRefillAutoWateringPending = true;
            // Send Telegram notification
            waterLevelConsecutiveHighReadings = 0;
        }
    } else {
        // Reset counter on any LOW reading
        waterLevelConsecutiveHighReadings = 0;
    }
}
```

### Phase 3: Add Auto-Start Timer Logic
**File**: `WateringSystem.h` - function `processWateringLoop()` (lines 425-461)

**Add new check** after `checkWaterLevelSensor()` (around line 430):

```cpp
// Check for pending auto-watering after refill
if (waterRefillAutoWateringPending) {
    unsigned long elapsed = millis() - waterRefillCompletedTime;
    if (elapsed >= 10000) {  // 10 seconds elapsed
        waterRefillAutoWateringPending = false;
        DebugHelper::debugImportant("💧 Auto-starting sequential watering after refill (10s delay)");
        startSequentialWatering();
    }
}
```

**Timing**: 5 readings × 100ms = 500ms minimum + 10s delay = ~10.5s total from first HIGH reading

### Phase 4: Cancel Pending Auto-Start on Manual Actions
**File**: `WateringSystem.h`

**Add cancellation logic** to:

1. **`startSequentialWatering()`** (line ~960):
   - Clear `waterRefillAutoWateringPending = false` before starting
   - Prevents double-start if user manually triggers during 10s window

2. **`startWatering(int valveIndex)`** (line ~830):
   - Clear `waterRefillAutoWateringPending = false`
   - Prevents auto-start after manual individual valve start

3. **`checkAutoWatering()`** (line ~780):
   - No changes needed (auto-watering will coexist)

### Phase 5: Enhanced Telegram Notification
**File**: `WateringSystem.h` - `checkWaterLevelSensor()` notification

**Update "water restored" message** to include:
```
✅ WATER LEVEL RESTORED ✅
⏰ [DateTime]
💧 Water tank refilled (5 consecutive readings)
🔄 System resuming normal operation

⏰ Automatic watering will start in 10 seconds
✓ Watering operations enabled
```

### Phase 6: Reset Logic on Halt/Stop
**File**: `WateringSystem.h`

**In `setHaltMode()` function** (line ~196):
- If `halt = true`, clear `waterRefillAutoWateringPending = false`
- Prevents auto-start when system is halted

### Phase 7: State Publishing (Optional)
**File**: `WateringSystemStateMachine.h` - `publishCurrentState()`

**Optionally add to water_level object** (line ~237):
```json
"water_level": {
    "status": "low" or "ok",
    "blocked": false,
    "refill_auto_watering_pending": true/false,
    "seconds_until_auto_start": X
}
```

## Critical Files to Modify

1. **WateringSystem.h** (primary changes)
   - Add 3 new state variables (line ~85)
   - Modify `checkWaterLevelSensor()` (lines 616-758)
   - Modify `processWateringLoop()` (lines 425-461)
   - Modify `startWatering()` (line ~825)
   - Modify `startSequentialWatering()` (line ~960)
   - Modify `setHaltMode()` (line ~196)

2. **WateringSystemStateMachine.h** (optional MQTT enhancement)
   - Update `publishCurrentState()` (line ~237)

3. **config.h** (optional constants)
   - Add `WATER_REFILL_CONSECUTIVE_READINGS` = 5
   - Add `WATER_REFILL_AUTO_START_DELAY_MS` = 10000

## Testing Strategy

### Unit Tests (desktop)
- Cannot test hardware sensor, but can test:
  - Consecutive reading counter logic (mock sensor values)
  - Timer expiration logic
  - Cancellation on manual start

### Hardware Testing (ESP32)
1. **Trigger LOW water level**:
   - Disconnect water level sensor or physically empty tank
   - Verify watering blocked, Telegram notification sent

2. **Simulate refill with noise**:
   - Reconnect sensor with intermittent connection
   - Verify counter resets on LOW readings
   - Verify requires 5 consecutive HIGH readings

3. **Verify 10s auto-start**:
   - Achieve stable refill (5× HIGH)
   - Wait 10 seconds
   - Verify sequential watering starts automatically
   - Check Telegram notification mentions 10s delay

4. **Test manual cancellation**:
   - Trigger refill countdown
   - Manually start watering during 10s window
   - Verify no double-start

5. **Test halt mode**:
   - Trigger refill countdown
   - Send `/halt` command during 10s window
   - Verify auto-start cancelled

### Serial Monitor Verification
- Watch for debug messages:
  - `"💧 Water tank refilled (5 consecutive HIGH readings)"`
  - `"💧 Auto-starting sequential watering after refill (10s delay)"`
  - Counter increments during refill detection

### MQTT Verification
- Subscribe to `$devices/{ID}/state`
- Verify `water_level.refill_auto_watering_pending` transitions
- Check timestamps align with 10s delay

## Safety Considerations

1. **No risk of overwatering**: Uses existing `startSequentialWatering()` which has all safety checks
2. **Halt mode integration**: Auto-start cancelled if halt activated
3. **Overflow protection**: Existing master overflow sensor (GPIO 42) still active
4. **Timeouts**: All valves still subject to 25s/30s timeouts
5. **Debouncing**: 5 consecutive readings = 500ms minimum stability
6. **Manual override**: User can cancel pending auto-start by manual action

## Edge Cases Handled

1. **millis() overflow**: Uses relative time comparison (elapsed calculation)
2. **WiFi down**: Auto-start works offline, only Telegram notification requires WiFi
3. **Power cycle during countdown**: State resets (non-persistent), no auto-start on boot
4. **Multiple refills**: Counter resets properly on each LOW→HIGH transition
5. **Sensor noise**: Requires 5 consecutive readings, resets on any LOW

## Implementation Order

1. Add state variables and initialization
2. Modify water level detection logic (consecutive counter)
3. Add 10s timer check in processWateringLoop()
4. Add cancellation logic to manual start functions
5. Update Telegram notification message
6. Add halt mode integration
7. (Optional) Add MQTT state publishing
8. Test on hardware with serial monitor
9. Verify with realistic refill scenarios

## Constants Summary

| Constant | Value | Notes |
|----------|-------|-------|
| WATER_REFILL_CONSECUTIVE_READINGS | 5 | Readings required for stable detection |
| WATER_REFILL_AUTO_START_DELAY_MS | 10000 | 10 seconds delay before auto-start |
| WATER_LEVEL_CHECK_INTERVAL | 100 | Existing, sensor poll rate (ms) |

**Total detection time**: 5 readings × 100ms = 500ms minimum + 10s delay = ~10.5s from first HIGH reading

## Verification Checklist

- [ ] Water level LOW blocks all watering (existing)
- [ ] Requires 5 consecutive HIGH readings to consider refill successful
- [ ] Counter resets to 0 on any LOW reading during detection
- [ ] 10 second countdown starts after 5th HIGH reading
- [ ] Sequential watering auto-starts after 10s delay
- [ ] Manual watering during countdown cancels auto-start
- [ ] Halt mode during countdown cancels auto-start
- [ ] Telegram notification mentions 10s auto-start
- [ ] No double-start scenarios
- [ ] Works offline (WiFi down)
- [ ] All existing safety features remain active

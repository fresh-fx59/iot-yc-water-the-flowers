# Overwatering Risk Analysis

## Overview
This document identifies all potential overwatering scenarios in the smart watering system and how they are mitigated.

## Risk Categories

### 1. Sensor Hardware Failures ⚠️ HIGH RISK

#### 1.1 Rain Sensor Stuck DRY (reads HIGH)
- **Cause**: Sensor physically damaged, corroded, or malfunctioning
- **Result**: System never detects water, continues pumping until timeout
- **Duration**: 25 seconds (MAX_WATERING_TIME)
- **Mitigation**: Timeout protection + emergency cutoff at 30s

#### 1.2 Rain Sensor Disconnected
- **Cause**: Wire broken, connector loose
- **Result**: INPUT_PULLUP pulls line HIGH → reads as DRY
- **Duration**: 25 seconds (MAX_WATERING_TIME)
- **Mitigation**: Timeout protection + emergency cutoff at 30s

#### 1.3 Sensor Power Failure (GPIO 18)
- **Cause**: GPIO 18 stuck LOW, transistor failure
- **Result**: All sensors unpowered, read as DRY
- **Duration**: 25 seconds per valve × 6 valves = 2.5 minutes total
- **Mitigation**: Timeout protection per valve

#### 1.4 Multiple Sensor Failures
- **Cause**: Power surge, water damage to sensor board
- **Result**: Multiple trays overwatered in sequence
- **Duration**: Up to 2.5 minutes for all 6 valves
- **Mitigation**: Master overflow sensor should catch this

#### 1.5 Sensor Short Circuit (always reads WET)
- **Cause**: Water bridges sensor permanently
- **Result**: No watering ever (opposite problem - plants die)
- **Risk**: Low for overwatering, but system ineffective

### 2. Timeout System Failures ⚠️ CRITICAL RISK

#### 2.1 Timeout Checks Bypassed
- **Cause**: Logic error in state machine
- **Result**: Infinite watering
- **Mitigation**: Multiple timeout layers:
  - MAX_WATERING_TIME (25s)
  - ABSOLUTE_SAFETY_TIMEOUT (30s)
  - Global safety watchdog

#### 2.2 millis() Overflow Edge Case
- **Cause**: millis() wraps at ~49 days, calculations fail
- **Result**: Timeout calculations incorrect
- **Mitigation**: System handles overflow in learning data, but state machine arithmetic could fail
- **Test**: Simulate millis() near overflow boundary

#### 2.3 Emergency Timeout Not Triggered
- **Cause**: ABSOLUTE_SAFETY_TIMEOUT check fails
- **Result**: Watering continues beyond 30s
- **Mitigation**: Global safety watchdog runs independently

### 3. State Machine Logic Errors ⚠️ HIGH RISK

#### 3.1 Stuck in PHASE_WATERING
- **Cause**: Phase transition logic error
- **Result**: Never exits watering phase
- **Mitigation**: Timeout forces transition to PHASE_CLOSING_VALVE

#### 3.2 Invalid Phase Transitions
- **Cause**: Race condition, logic error
- **Result**: Skips PHASE_CLOSING_VALVE, pump stays on
- **Mitigation**: Each phase explicitly handles cleanup

#### 3.3 wateringRequested Flag Not Cleared
- **Cause**: Manual stop doesn't clear flag
- **Result**: Valve immediately restarts watering
- **Mitigation**: Explicit flag clearing in PHASE_CLOSING_VALVE and PHASE_IDLE

### 4. Pump Control Failures ⚠️ CRITICAL RISK

#### 4.1 Pump Stays ON When All Valves Close
- **Cause**: updatePumpState() logic error
- **Result**: Pump runs with all valves closed (damage to pump)
- **Mitigation**: updatePumpState() explicitly checks all valves

#### 4.2 Pump Not Turned OFF on Emergency Stop
- **Cause**: Emergency stop doesn't control pump GPIO
- **Result**: Pump continues running
- **Mitigation**: emergencyStopAll() uses direct GPIO control

#### 4.3 Multiple Valves Open Simultaneously
- **Cause**: Sequential mode logic error
- **Result**: Pressure drop, overflow from multiple trays
- **Mitigation**: Sequential mode strictly enforces one-at-a-time

### 5. Control Flow Failures ⚠️ MEDIUM RISK

#### 5.1 Manual Stop Ignored
- **Cause**: wateringRequested flag not checked
- **Result**: Cannot stop watering manually
- **Mitigation**: Check in PHASE_WATERING every 100ms

#### 5.2 Halt Mode Bypassed
- **Cause**: Halt check missing from critical path
- **Result**: Watering during firmware update
- **Mitigation**: Checks in startWatering(), checkAutoWatering()

#### 5.3 Overflow Flag Ignored
- **Cause**: overflowDetected check missing
- **Result**: Watering continues after overflow
- **Mitigation**: Checks in checkAutoWatering(), startWatering()

### 6. Master Overflow Sensor Failures ⚠️ CRITICAL RISK

#### 6.1 Overflow Sensor Stuck HIGH (dry)
- **Cause**: Sensor disconnected, damaged
- **Result**: Never detects overflow
- **Mitigation**: Timeout protection still active

#### 6.2 Overflow Sensor Check Not Running
- **Cause**: Loop logic error, checkMasterOverflowSensor() skipped
- **Result**: Overflow goes undetected
- **Mitigation**: Must be first check in processWateringLoop()

#### 6.3 Emergency Stop Not Executed
- **Cause**: emergencyStopAll() fails
- **Result**: Valves/pump continue despite overflow
- **Mitigation**: Direct GPIO control (digitalWrite)

### 7. Global Safety Watchdog Failures ⚠️ CRITICAL RISK

#### 7.1 Watchdog Not Running
- **Cause**: globalSafetyWatchdog() skipped in loop
- **Result**: No independent safety check
- **Mitigation**: Must be called every loop iteration

#### 7.2 Watchdog Checks Wrong Timeout
- **Cause**: Logic uses wrong time reference
- **Result**: Timeout never triggers
- **Mitigation**: Test all timeout calculations

### 8. Sequential Watering Failures ⚠️ MEDIUM RISK

#### 8.1 Next Valve Starts Before Previous Closes
- **Cause**: isValveComplete() logic error
- **Result**: Multiple valves open → overflow
- **Mitigation**: Check phase == PHASE_IDLE

#### 8.2 Sequential Mode Never Ends
- **Cause**: currentSequenceIndex never reaches sequenceLength
- **Result**: Repeats watering cycle infinitely
- **Mitigation**: Explicit index increment + bounds check

### 9. Edge Cases ⚠️ LOW-MEDIUM RISK

#### 9.1 Watering During millis() Overflow
- **Cause**: millis() wraps during active watering
- **Result**: Timeout calculation fails (currentTime < startTime)
- **Mitigation**: Need explicit overflow handling

#### 9.2 Very Long Watering Duration Overflow
- **Cause**: currentTime - startTime > ULONG_MAX
- **Result**: Arithmetic overflow
- **Likelihood**: Impossible (timeout kills watering first)

#### 9.3 Concurrent Auto-Watering + Manual Start
- **Cause**: Auto-watering triggers while manual start pending
- **Result**: Double-watering of same valve
- **Mitigation**: Check phase == PHASE_IDLE before starting

## Risk Summary

| Risk Level | Count | Examples |
|-----------|-------|----------|
| CRITICAL | 5 | Timeout bypass, pump control, overflow sensor, watchdog |
| HIGH | 3 | Sensor failures, state machine stuck |
| MEDIUM | 3 | Control flow, sequential watering |
| LOW | 2 | Edge cases, millis() overflow |

## Testing Priorities

1. **Timeout Protection** (CRITICAL)
   - Test all timeout calculations
   - Test millis() overflow edge cases
   - Test emergency timeout triggers

2. **Sensor Failure Modes** (HIGH)
   - Test stuck sensors (DRY/WET)
   - Test disconnected sensors
   - Test power failure

3. **State Machine Robustness** (HIGH)
   - Test all phase transitions
   - Test stuck states
   - Test race conditions

4. **Pump Control** (CRITICAL)
   - Test pump ON/OFF logic
   - Test emergency stop
   - Test multiple valve scenarios

5. **Safety Layers** (CRITICAL)
   - Test global watchdog
   - Test overflow sensor
   - Test halt mode

## Recommended Improvements

1. **Sensor Health Monitoring**
   - Track sensor read patterns
   - Detect stuck sensors (always DRY or WET for N consecutive reads)
   - Alert if sensor never changes state

2. **Watchdog Timer Enhancement**
   - Add independent hardware watchdog (ESP32 built-in)
   - Require explicit watchdog reset every loop
   - System resets if watchdog not fed

3. **Pump Protection**
   - Add current sensor to detect pump running dry
   - Add pressure sensor to detect valve failures

4. **Redundant Overflow Detection**
   - Multiple overflow sensors at different heights
   - Require majority vote for overflow detection

5. **State Validation**
   - Add state invariant checks (e.g., "pump ON requires at least one valve in PHASE_WATERING")
   - Assert failures trigger emergency stop

6. **Time Validation**
   - Validate all time arithmetic handles millis() overflow
   - Use signed difference: `(long)(currentTime - startTime)`

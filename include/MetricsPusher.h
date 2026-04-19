#ifndef METRICS_PUSHER_H
#define METRICS_PUSHER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "config.h"
#include "ValveController.h"

// Forward declaration - WateringSystem is included after class definition
class WateringSystem;
extern WateringSystem* g_wateringSystem_ptr;

// ============================================
// Metrics Log Entry
// ============================================
struct MetricsLogEntry {
    String message;
    String level;          // "debug", "info", "warn", "error"
    unsigned long epochSeconds;
    unsigned long millisFraction;
    bool valid;
};

// ============================================
// MetricsPusher - Prometheus/Loki push gateway
// Header-only static class (same pattern as DebugHelper)
// ============================================
class MetricsPusher {
private:
    static MetricsLogEntry logBuffer[METRICS_LOG_BUFFER_SIZE];
    static int logHead;
    static int logTail;
    static int logCount;

    static unsigned long lastPushTime;
    static int lastLogPushHttpCode;
    static int logPushAttempts;
    static int logPushSuccesses;

    // HTTP helpers (same pattern as TelegramNotifier)
    static bool useProxy() {
        return String(METRICS_PROXY_BASE_URL).length() > 0;
    }

    static String proxyBaseUrl() {
        String base = String(METRICS_PROXY_BASE_URL);
        while (base.endsWith("/")) {
            base.remove(base.length() - 1);
        }
        return base;
    }

    static void applyAuthHeader(HTTPClient& http) {
        String token = String(TELEGRAM_PROXY_AUTH_TOKEN);
        token.trim();
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }
    }

    static bool beginHttp(HTTPClient& http, const String& url, WiFiClientSecure& secureClient, WiFiClient& plainClient) {
        if (url.startsWith("https://")) {
            secureClient.setInsecure();
            return http.begin(secureClient, url);
        }
        return http.begin(plainClient, url);
    }

    static void addLogEntry(const String& level, const String& msg) {
        if (logCount >= METRICS_LOG_BUFFER_SIZE) {
            // Drop oldest entry
            logTail = (logTail + 1) % METRICS_LOG_BUFFER_SIZE;
            logCount--;
        }

        time_t now;
        time(&now);

        logBuffer[logHead].message = msg;
        logBuffer[logHead].level = level;
        logBuffer[logHead].epochSeconds = (unsigned long)now - RTC_TIMEZONE_OFFSET_SEC;
        logBuffer[logHead].millisFraction = millis() % 1000;
        logBuffer[logHead].valid = true;

        logHead = (logHead + 1) % METRICS_LOG_BUFFER_SIZE;
        logCount++;
    }

    static bool isAnyValveActive();
    static String buildMetricsJson();
    static String buildLogsJson();
    static bool pushMetrics(const String& json);
    static bool pushLogs(const String& json);

public:
    // Callback for g_metricsLog function pointer (set in init)
    static void metricsLogCallback(const String& level, const String& msg) {
        addLogEntry(level, msg);
    }

    static void init() {
        logHead = 0;
        logTail = 0;
        logCount = 0;
        lastPushTime = 0;
        lastLogPushHttpCode = 0;
        logPushAttempts = 0;
        logPushSuccesses = 0;
        // Set global callback so all headers can route logs to Loki
        g_metricsLog = metricsLogCallback;
    }

    static void loop();

    // Log convenience methods
    static void log(const String& level, const String& msg) {
        addLogEntry(level, msg);
    }

    static void logDebug(const String& msg) { addLogEntry("debug", msg); }
    static void logInfo(const String& msg) { addLogEntry("info", msg); }
    static void logWarn(const String& msg) { addLogEntry("warn", msg); }
    static void logError(const String& msg) { addLogEntry("error", msg); }

};

// ============================================
// Static Member Initialization
// ============================================
MetricsLogEntry MetricsPusher::logBuffer[METRICS_LOG_BUFFER_SIZE];
int MetricsPusher::logHead = 0;
int MetricsPusher::logTail = 0;
int MetricsPusher::logCount = 0;
unsigned long MetricsPusher::lastPushTime = 0;
int MetricsPusher::lastLogPushHttpCode = 0;
int MetricsPusher::logPushAttempts = 0;
int MetricsPusher::logPushSuccesses = 0;

// ============================================
// Include WateringSystem AFTER static member init to avoid circular deps
// ============================================
#include "WateringSystem.h"

// ============================================
// Implementation (needs WateringSystem)
// ============================================

inline bool MetricsPusher::isAnyValveActive() {
    if (!g_wateringSystem_ptr) return false;
    for (int i = 0; i < NUM_VALVES; i++) {
        ValveController* v = g_wateringSystem_ptr->getValve(i);
        if (v && v->phase != PHASE_IDLE) return true;
    }
    return false;
}

inline void MetricsPusher::loop() {
    if (!useProxy() || !WiFi.isConnected()) return;

    unsigned long now = millis();
    unsigned long interval = isAnyValveActive() ? METRICS_PUSH_INTERVAL_ACTIVE_MS : METRICS_PUSH_INTERVAL_IDLE_MS;

    if (lastPushTime != 0 && (now - lastPushTime) < interval) return;
    lastPushTime = now;

    // Push metrics
    String metricsJson = buildMetricsJson();
    pushMetrics(metricsJson);

    // Push logs if buffer non-empty
    if (logCount > 0) {
        String logsJson = buildLogsJson();
        if (pushLogs(logsJson)) {
            // Clear buffer only after successful push
            logTail = logHead;
            logCount = 0;
        }
    } else {
        Serial.println("[MetricsPusher] Log buffer empty, nothing to push");
    }
}

inline String MetricsPusher::buildMetricsJson() {
    String json = "{";

    // Uptime
    json += "\"uptime_s\":" + String(millis() / 1000);

    // Free heap
    json += ",\"free_heap\":" + String(ESP.getFreeHeap());

    // WiFi RSSI
    json += ",\"wifi_rssi\":" + String(WiFi.RSSI());

    if (g_wateringSystem_ptr) {
        // Pump
        json += ",\"pump\":" + String(g_wateringSystem_ptr->getPumpState() == PUMP_ON ? 1 : 0);

        // Overflow
        json += ",\"overflow\":" + String(g_wateringSystem_ptr->isOverflowDetected() ? 1 : 0);
        json += ",\"overflow_streak\":" + String(g_wateringSystem_ptr->getOverflowDetectionStreak());

        // Water tank
        json += ",\"water_tank_ok\":" + String(g_wateringSystem_ptr->isWaterLevelLow() ? 0 : 1);

        // Plant light
        json += ",\"plant_light\":" + String(g_wateringSystem_ptr->isPlantLightOn() ? 1 : 0);

        // Telegram failures
        json += ",\"telegram_failures\":" + String(g_telegramFailures);

        // Valves
        unsigned long currentTime = millis();
        json += ",\"valves\":[";
        for (int i = 0; i < NUM_VALVES; i++) {
            ValveController* v = g_wateringSystem_ptr->getValve(i);
            if (!v) continue;

            if (i > 0) json += ",";
            json += "{";
            json += "\"id\":" + String(i);
            json += ",\"state\":" + String(v->state == VALVE_OPEN ? 1 : 0);
            json += ",\"phase\":" + String((int)v->phase);
            json += ",\"rain\":" + String(v->rainDetected ? 1 : 0);

            // Watering duration in seconds
            unsigned long wateringSec = 0;
            if (v->phase == PHASE_WATERING && v->wateringStartTime > 0) {
                wateringSec = (currentTime - v->wateringStartTime) / 1000;
            }
            json += ",\"watering_s\":" + String(wateringSec);

            // Water level percentage
            float waterLevel = calculateCurrentWaterLevel(v, currentTime);
            json += ",\"water_level_pct\":" + String((int)waterLevel);

            // Learning data
            json += ",\"calibrated\":" + String(v->isCalibrated ? 1 : 0);
            json += ",\"auto_watering\":" + String(v->autoWateringEnabled ? 1 : 0);
            json += ",\"interval_mult\":" + String(v->intervalMultiplier, 2);
            json += ",\"total_cycles\":" + String(v->totalWateringCycles);

            // Time since last watering
            unsigned long timeSince = 0;
            if (hasLastWateringReference(v)) {
                timeSince = getTimeSinceLastWatering(v, currentTime);
            }
            json += ",\"time_since_ms\":" + String(timeSince);

            // Time until empty
            unsigned long timeUntilEmpty = 0;
            if (v->isCalibrated && v->emptyToFullDuration > 0 && hasLastWateringReference(v)) {
                unsigned long ts = getTimeSinceLastWatering(v, currentTime);
                if (ts < v->emptyToFullDuration) {
                    timeUntilEmpty = v->emptyToFullDuration - ts;
                }
            }
            json += ",\"time_until_empty_ms\":" + String(timeUntilEmpty);

            // Time since last watering attempt (for 24h safety interval tracking)
            unsigned long timeSinceAttempt = 0;
            if (hasLastWateringAttemptReference(v)) {
                timeSinceAttempt = getTimeSinceLastWateringAttempt(v, currentTime);
            }
            json += ",\"time_since_attempt_ms\":" + String(timeSinceAttempt);

            // Time until next watering (mirrors shouldWaterNow logic)
            // max(emptyToFullDuration - timeSince, 24h_min - timeSinceAttempt, 0)
            unsigned long timeUntilNext = 0;
            if (v->autoWateringEnabled && (v->isCalibrated || v->emptyToFullDuration > 0)) {
                unsigned long consumptionRemaining = 0;
                if (v->emptyToFullDuration > 0 && hasLastWateringReference(v) && timeSince < v->emptyToFullDuration) {
                    consumptionRemaining = v->emptyToFullDuration - timeSince;
                }
                unsigned long safetyRemaining = 0;
                if (hasLastWateringAttemptReference(v) && timeSinceAttempt < AUTO_WATERING_MIN_INTERVAL_MS) {
                    safetyRemaining = AUTO_WATERING_MIN_INTERVAL_MS - timeSinceAttempt;
                }
                timeUntilNext = consumptionRemaining > safetyRemaining ? consumptionRemaining : safetyRemaining;
            }
            json += ",\"time_until_next_ms\":" + String(timeUntilNext);

            json += ",\"baseline_fill_ms\":" + String(v->baselineFillDuration);
            json += ",\"last_fill_ms\":" + String(v->lastFillDuration);
            json += ",\"empty_duration_ms\":" + String(v->emptyToFullDuration);

            json += "}";
        }
        json += "]";
    }

    // Log push diagnostics (visible in Prometheus for debugging)
    json += ",\"log_buffer_count\":" + String(logCount);
    json += ",\"log_push_last_code\":" + String(lastLogPushHttpCode);
    json += ",\"log_push_attempts\":" + String(logPushAttempts);
    json += ",\"log_push_successes\":" + String(logPushSuccesses);

    json += "}";
    return json;
}

inline String MetricsPusher::buildLogsJson() {
    // Group entries by level
    // Loki push format: {"streams":[{"stream":{...},"values":[[ts, msg], ...]}, ...]}

    // Collect entries by level
    struct LevelGroup {
        String level;
        String values; // JSON array contents
        int count;
    };
    LevelGroup groups[4] = {
        {"debug", "", 0},
        {"info", "", 0},
        {"warn", "", 0},
        {"error", "", 0}
    };

    int idx = logTail;
    for (int i = 0; i < logCount; i++) {
        MetricsLogEntry& entry = logBuffer[idx];
        if (entry.valid) {
            // Find matching group
            int g = -1;
            for (int j = 0; j < 4; j++) {
                if (groups[j].level == entry.level) {
                    g = j;
                    break;
                }
            }
            if (g < 0) g = 1; // Default to info

            // Build Loki timestamp: epochSeconds + millisFraction + 000000 (nanoseconds)
            char ts[24];
            snprintf(ts, sizeof(ts), "%lu%03lu000000", entry.epochSeconds, entry.millisFraction);

            // Escape message for JSON
            String escaped = entry.message;
            escaped.replace("\\", "\\\\");
            escaped.replace("\"", "\\\"");
            escaped.replace("\n", "\\n");

            if (groups[g].count > 0) groups[g].values += ",";
            groups[g].values += "[\"" + String(ts) + "\",\"" + escaped + "\"]";
            groups[g].count++;
        }
        idx = (idx + 1) % METRICS_LOG_BUFFER_SIZE;
    }

    // NOTE: buffer is NOT cleared here — cleared in loop() only after successful push

    // Build JSON
    String json = "{\"streams\":[";
    bool first = true;
    for (int g = 0; g < 4; g++) {
        if (groups[g].count == 0) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"stream\":{\"job\":\"esp32\",\"device\":\"watering-system\",\"level\":\"" + groups[g].level + "\"},";
        json += "\"values\":[" + groups[g].values + "]}";
    }
    json += "]}";

    return json;
}

inline bool MetricsPusher::pushMetrics(const String& json) {
    HTTPClient http;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;

    String url = proxyBaseUrl() + "/v1/metrics/push";
    if (!beginHttp(http, url, secureClient, plainClient)) {
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    applyAuthHeader(http);
    http.setTimeout(METRICS_HTTP_TIMEOUT_MS);

    int httpCode = http.POST(json);
    http.end();

    return (httpCode >= 200 && httpCode < 300);
}

inline bool MetricsPusher::pushLogs(const String& json) {
    logPushAttempts++;

    HTTPClient http;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;

    String url = proxyBaseUrl() + "/v1/logs/push";
    if (!beginHttp(http, url, secureClient, plainClient)) {
        lastLogPushHttpCode = -1;
        Serial.println("[MetricsPusher] Log push: beginHttp failed");
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    applyAuthHeader(http);
    http.setTimeout(METRICS_HTTP_TIMEOUT_MS);

    int httpCode = http.POST(json);
    lastLogPushHttpCode = httpCode;
    http.end();

    bool success = (httpCode >= 200 && httpCode < 300);
    if (success) {
        logPushSuccesses++;
        Serial.println("[MetricsPusher] Log push OK (" + String(logCount) + " entries)");
    } else {
        Serial.println("[MetricsPusher] Log push FAILED, HTTP " + String(httpCode) + " (" + String(logCount) + " entries, " + String(json.length()) + " bytes)");
    }
    return success;
}

#endif // METRICS_PUSHER_H

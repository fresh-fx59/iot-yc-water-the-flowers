#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include <Arduino.h>
#include <WebServer.h>

// External references
extern WebServer httpServer;
class WateringSystem;
extern WateringSystem* g_wateringSystem_ptr;

// ============================================
// API Handler Implementations
// ============================================

inline void handleWaterApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }

    String valveStr = httpServer.arg("valve");
    int valve = valveStr.toInt();

    if (valve < 1 || valve > 6) {
        httpServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid valve number\"}");
        return;
    }

    Serial.printf("✓ API: Starting watering for valve %d\n", valve);
    g_wateringSystem_ptr->startWatering(valve - 1);
    httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Watering started\"}");
}

inline void handleStopApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }

    String valveStr = httpServer.arg("valve");

    if (valveStr == "all") {
        Serial.println("✓ API: Stopping all valves");
        for (int i = 0; i < 6; i++) {
            g_wateringSystem_ptr->stopWatering(i);
        }
        httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"All watering stopped\"}");
    } else {
        int valve = valveStr.toInt();
        if (valve < 1 || valve > 6) {
            httpServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid valve number\"}");
            return;
        }
        Serial.printf("✓ API: Stopping valve %d\n", valve);
        g_wateringSystem_ptr->stopWatering(valve - 1);
        httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Watering stopped\"}");
    }
}

inline void handleStatusApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }

    String stateJson = g_wateringSystem_ptr->getLastState();

    if (stateJson.length() == 0) {
        stateJson = "{\"pump\":\"off\",\"valves\":[";
        for (int i = 0; i < 6; i++) {
            stateJson += "{\"id\":" + String(i) + ",\"state\":\"closed\",\"phase\":\"idle\",\"rain\":false}";
            if (i < 5) stateJson += ",";
        }
        stateJson += "]}";
    }

    httpServer.send(200, "application/json", stateJson);
}

#endif // API_HANDLERS_H

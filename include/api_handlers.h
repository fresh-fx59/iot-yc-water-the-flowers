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
    g_wateringSystem_ptr->startWatering(valve - 1, true);
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

inline void handlePlantLightApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }

    String action = httpServer.arg("action");

    if (action == "on") {
        g_wateringSystem_ptr->setPlantLightManualOn();
        httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Plant light turned on manually\"}");
        return;
    }

    if (action == "off") {
        g_wateringSystem_ptr->setPlantLightManualOff();
        httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Plant light turned off manually\"}");
        return;
    }

    if (action == "auto") {
        g_wateringSystem_ptr->setPlantLightAuto();
        httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Plant light returned to automatic schedule\"}");
        return;
    }

    httpServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid action (use on, off, or auto)\"}");
}

inline void handleStartAllApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }

    Serial.println("✓ API: Starting sequential watering (all valves)");
    g_wateringSystem_ptr->startSequentialWatering("Web API");
    httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Sequential watering started\"}");
}

inline void handleResetCalibrationApi() {
    if (!g_wateringSystem_ptr) {
        httpServer.send(500, "application/json", "{\"success\":false,\"message\":\"System not initialized\"}");
        return;
    }

    String valveStr = httpServer.arg("valve");

    // Handle "all" parameter
    if (valveStr == "all") {
        Serial.println("✓ API: Resetting calibration for all valves");
        g_wateringSystem_ptr->resetAllCalibrations();
        httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"All calibrations reset\"}");
        return;
    }

    // Handle specific valve
    int valve = valveStr.toInt();
    if (valve < 1 || valve > 6) {
        httpServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid valve number (use 1-6 or 'all')\"}");
        return;
    }

    Serial.printf("✓ API: Resetting calibration for valve %d\n", valve);
    g_wateringSystem_ptr->resetCalibration(valve - 1); // Convert 1-indexed to 0-indexed
    httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"Calibration reset for valve " + String(valve) + "\"}");
}

#endif // API_HANDLERS_H

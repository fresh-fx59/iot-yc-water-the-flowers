#ifndef TELEGRAM_NOTIFIER_H
#define TELEGRAM_NOTIFIER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "config.h"
#include "secret.h"
#include "DebugHelper.h"

// ============================================
// Telegram Notifier Class
// Sends watering notifications via Telegram Bot API
// ============================================
class TelegramNotifier {
private:
    static String urlEncode(const String& str) {
        String encoded = "";
        char c;
        for (size_t i = 0; i < str.length(); i++) {
            c = str.charAt(i);
            if (c == ' ') {
                encoded += "+";
            } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded += c;
            } else {
                encoded += '%';
                char hex[3];
                sprintf(hex, "%02X", c);
                encoded += hex;
            }
        }
        return encoded;
    }

    static bool sendMessage(const String& message) {
        if (!WiFi.isConnected()) {
            DebugHelper::debug("❌ Cannot send Telegram: WiFi not connected");
            return false;
        }

        HTTPClient http;
        WiFiClientSecure client;
        client.setInsecure();  // For simplicity - use proper cert verification in production

        String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                     "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID +
                     "&text=" + urlEncode(message) +
                     "&parse_mode=HTML";

        http.begin(client, url);
        http.setTimeout(10000);  // 10 second timeout

        int httpCode = http.GET();
        bool success = (httpCode == 200);

        if (success) {
            DebugHelper::debug("✓ Telegram message sent");
        } else {
            DebugHelper::debug("❌ Telegram send failed, HTTP code: " + String(httpCode));
            if (httpCode > 0) {
                DebugHelper::debug("Response: " + http.getString());
            }
        }

        http.end();
        return success;
    }

public:
    // Format current time as "DD-MM-YYYY HH:MM:SS"
    static String getCurrentDateTime() {
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo)) {
            return "Time not synced";
        }

        char buffer[20];
        strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
        return String(buffer);
    }

    // Send device online notification
    static void sendDeviceOnline(const String& version, const String& deviceType) {
        if (!WiFi.isConnected()) {
            DebugHelper::debug("❌ Cannot send Telegram: WiFi not connected");
            return;
        }

        String message = "🟢 <b>Device Online</b>\n";
        message += "⏰ " + getCurrentDateTime() + "\n";
        message += "📍 IP: " + WiFi.localIP().toString() + "\n";
        message += "📶 WiFi: " + String(WiFi.RSSI()) + " dBm\n";
        message += "🔧 Version: " + version;

        DebugHelper::debug("\n📱 Sending Telegram online notification...");
        sendMessage(message);
    }

    // Send watering start notification
    static void sendWateringStarted(const String& triggerType, const String& trayNumbers) {
        String timestamp = "Session " + getCurrentDateTime();

        String message = "🚿 <b>Watering Started</b>\n";
        message += "⏰ " + timestamp + "\n";
        message += "🔧 Trigger: " + triggerType + "\n";
        message += "🌱 Trays: " + trayNumbers;

        DebugHelper::debug("\n📱 Sending Telegram start notification...");
        sendMessage(message);
    }

    // Send watering completion notification with results table
    static void sendWateringComplete(const String results[][3], int numTrays) {
        String message = "✅ <b>Watering Complete</b>\n\n";
        message += "<pre>";
        message += "tray | duration(sec) | status\n";
        message += "-----|---------------|-------\n";

        for (int i = 0; i < numTrays; i++) {
            String tray = results[i][0];
            String duration = results[i][1];
            String status = results[i][2];

            // Column 1: tray (4 chars, right-aligned)
            while (tray.length() < 4) tray = " " + tray;
            message += tray + " | ";

            // Column 2: duration (13 chars, right-aligned)
            while (duration.length() < 13) duration = " " + duration;
            message += duration + " | ";

            // Column 3: status
            message += status + "\n";
        }

        message += "</pre>";

        DebugHelper::debug("\n📱 Sending Telegram completion notification...");
        sendMessage(message);
    }

    // Send watering schedule notification showing planned watering times
    // scheduleData[i][0] = tray number, [1] = status, [2] = planned time, [3] = duration
    static void sendWateringSchedule(const String scheduleData[][4], int numTrays, const String& title) {
        String message = "📅 <b>" + title + "</b>\n";
        message += "⏰ " + getCurrentDateTime() + "\n\n";
        message += "<pre>";
        message += "tray | status | planned     | dur\n";
        message += "-----|--------|-------------|----\n";

        for (int i = 0; i < numTrays; i++) {
            String tray = scheduleData[i][0];
            String status = scheduleData[i][1];
            String planned = scheduleData[i][2];
            String duration = scheduleData[i][3];

            // Column 1: tray (4 chars, right-aligned)
            while (tray.length() < 4) tray = " " + tray;
            message += tray + " | ";

            // Column 2: status (6 chars, centered)
            while (status.length() < 6) status = " " + status + " ";
            if (status.length() > 6) status = status.substring(0, 6);
            message += status + " | ";

            // Column 3: planned time (11 chars, left-aligned)
            while (planned.length() < 11) planned = planned + " ";
            message += planned + " | ";

            // Column 4: duration
            message += duration + "\n";
        }

        message += "</pre>";

        DebugHelper::debug("\n📱 Sending Telegram schedule notification...");
        sendMessage(message);
    }
};

// ============================================
// Global Function for DebugHelper
// ============================================
inline bool sendTelegramDebug(const String& message) {
    if (!WiFi.isConnected()) {
        return false; // Fail if no WiFi
    }

    // Use the private sendMessage method
    // Create a simple wrapper that calls the Telegram API directly
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                 "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID +
                 "&text=";

    // URL encode the message
    String encoded = "";
    for (size_t i = 0; i < message.length(); i++) {
        char c = message.charAt(i);
        if (c == ' ') {
            encoded += "+";
        } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            encoded += '%';
            char hex[3];
            sprintf(hex, "%02X", c);
            encoded += hex;
        }
    }

    url += encoded + "&parse_mode=HTML";

    http.begin(client, url);
    http.setTimeout(10000);
    int httpCode = http.GET();
    bool success = (httpCode == 200);
    http.end();

    return success;
}

#endif // TELEGRAM_NOTIFIER_H


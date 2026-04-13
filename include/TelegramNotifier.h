#ifndef TELEGRAM_NOTIFIER_H
#define TELEGRAM_NOTIFIER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "config.h"
#include "secret.h"
#include "DebugHelper.h"
#include "DS3231RTC.h"

// ============================================ 
// Telegram Notifier Class
// Sends watering notifications via Telegram Bot API
// ============================================ 
class TelegramNotifier {
private:
    static bool &botCommandsConfigured() {
        static bool value = false;
        return value;
    }

    static unsigned long &lastBotCommandsAttemptMs() {
        static unsigned long value = 0;
        return value;
    }

    // --- Debug/command cooldown (shared by DebugHelper and checkForCommands) ---
    static unsigned long &telegramCooldownUntilMs() {
        static unsigned long value = 0;
        return value;
    }

    static unsigned long &telegramFailureBackoffMs() {
        static unsigned long value = TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS;
        return value;
    }

    static bool isInCooldown() {
        return millis() < telegramCooldownUntilMs();
    }

    static void onTelegramSuccess() {
        telegramCooldownUntilMs() = 0;
        telegramFailureBackoffMs() = TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS;
    }

    static void onTelegramFailure() {
        unsigned long currentBackoff = telegramFailureBackoffMs();
        telegramCooldownUntilMs() = millis() + currentBackoff;
        telegramFailureBackoffMs() = min(currentBackoff * 2, TELEGRAM_FAILURE_COOLDOWN_MAX_MS);
    }

    // --- Notification cooldown (independent, so debug failures don't block notifications) ---
    static unsigned long &notifCooldownUntilMs() {
        static unsigned long value = 0;
        return value;
    }

    static unsigned long &notifFailureBackoffMs() {
        static unsigned long value = TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS;
        return value;
    }

    static bool isNotifInCooldown() {
        return millis() < notifCooldownUntilMs();
    }

    static void onNotifSuccess() {
        notifCooldownUntilMs() = 0;
        notifFailureBackoffMs() = TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS;
    }

    static void onNotifFailure() {
        unsigned long currentBackoff = notifFailureBackoffMs();
        notifCooldownUntilMs() = millis() + currentBackoff;
        notifFailureBackoffMs() = min(currentBackoff * 2, TELEGRAM_FAILURE_COOLDOWN_MAX_MS);
    }

    static bool useMonitoringProxy() {
        return String(TELEGRAM_PROXY_BASE_URL).length() > 0;
    }

    static String monitoringProxyBaseUrl() {
        String base = String(TELEGRAM_PROXY_BASE_URL);
        while (base.endsWith("/")) {
            base.remove(base.length() - 1);
        }
        return base;
    }

    static void applyProxyAuthHeader(HTTPClient& http) {
        String token = String(TELEGRAM_PROXY_AUTH_TOKEN);
        token.trim();
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }
    }

    static bool beginHttpClient(HTTPClient& http, const String& url, WiFiClientSecure& secureClient, WiFiClient& plainClient) {
        if (url.startsWith("https://")) {
            secureClient.setInsecure();  // For simplicity - use proper cert verification in production
            return http.begin(secureClient, url);
        }

        return http.begin(plainClient, url);
    }

    static unsigned long httpTimeoutMs(bool usingProxy) {
        return usingProxy ? TELEGRAM_PROXY_HTTP_TIMEOUT_MS : TELEGRAM_HTTP_TIMEOUT_MS;
    }

    static String getBotCommandsJson() {
        return String("[") +
               "{\"command\":\"menu\",\"description\":\"Quick-access button panel\"}," +
               "{\"command\":\"help\",\"description\":\"Show command reference\"}," +
               "{\"command\":\"water\",\"description\":\"Water tray N (e.g. /water 2)\"}," +
               "{\"command\":\"start_all\",\"description\":\"Water all trays sequentially\"}," +
               "{\"command\":\"halt\",\"description\":\"Block watering for firmware updates\"}," +
               "{\"command\":\"resume\",\"description\":\"Resume normal operation\"}," +
               "{\"command\":\"time\",\"description\":\"Show RTC time and battery\"}," +
               "{\"command\":\"settime\",\"description\":\"Sync or set device time\"}," +
               "{\"command\":\"test_sensors\",\"description\":\"Test all rain sensors\"}," +
               "{\"command\":\"reset_overflow\",\"description\":\"Clear overflow lock\"}," +
               "{\"command\":\"reinit_gpio\",\"description\":\"Reinitialize relay GPIOs\"}," +
               "{\"command\":\"overflow_status\",\"description\":\"Show overflow sensor readings\"}," +
               "{\"command\":\"lamp_status\",\"description\":\"Show plant light status\"}," +
               "{\"command\":\"lamp_on\",\"description\":\"Turn plant light on manually\"}," +
               "{\"command\":\"lamp_off\",\"description\":\"Turn plant light off manually\"}," +
               "{\"command\":\"lamp_auto\",\"description\":\"Return plant light to schedule\"}" +
               "]";
    }

    static void logTransportLocalOnly(const String& message) {
        #if IS_DEBUG_TO_SERIAL_ENABLED
        DEBUG_SERIAL.println(message);
        #endif
    }

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

    // Last callback query ID from button press (answered after command processing)
    static String &pendingCallbackQueryId() {
        static String value = "";
        return value;
    }

    static bool sendMessage(const String& message, const String& replyMarkup = "") {
        if (!WiFi.isConnected()) {
            logTransportLocalOnly("❌ Cannot send Telegram: WiFi not connected");
            return false;
        }
        if (isInCooldown()) {
            return false;
        }

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        int httpCode = -1;
        if (usingProxy) {
            String url = monitoringProxyBaseUrl() + "/v1/telegram/sendMessage";
            if (!beginHttpClient(http, url, client, plainClient)) {
                onTelegramFailure();
                logTransportLocalOnly("❌ Telegram proxy send begin failed");
                return false;
            }
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            applyProxyAuthHeader(http);
            String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                          "&chat_id=" + urlEncode(String(TELEGRAM_CHAT_ID)) +
                          "&text=" + urlEncode(message) +
                          "&parse_mode=HTML";
            if (replyMarkup.length() > 0) {
                body += "&reply_markup=" + urlEncode(replyMarkup);
            }
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                         "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID +
                         "&text=" + urlEncode(message) +
                         "&parse_mode=HTML";
            if (replyMarkup.length() > 0) {
                url += "&reply_markup=" + urlEncode(replyMarkup);
            }

            if (!beginHttpClient(http, url, client, plainClient)) {
                onTelegramFailure();
                logTransportLocalOnly("❌ Telegram send begin failed");
                return false;
            }
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.GET();
        }

        bool success = (httpCode == 200);

        if (success) {
            onTelegramSuccess();
            logTransportLocalOnly("✓ Telegram message sent");
            if (g_metricsLog) g_metricsLog("debug", "Telegram sent OK");
        } else {
            onTelegramFailure();
            logTransportLocalOnly("❌ Telegram send failed (" + String(usingProxy ? "proxy" : "direct") + "), HTTP code: " + String(httpCode));
            if (g_metricsLog) g_metricsLog("warn", "Telegram failed HTTP " + String(httpCode));
            g_telegramFailures++;
            if (httpCode > 0) {
                logTransportLocalOnly("Response: " + http.getString());
            }
        }

        http.end();
        return success;
    }

public:
    // Format current time as "DD-MM-YYYY HH:MM:SS" (using system time)
    static String getCurrentDateTime() {
        time_t now;
        time(&now);
        struct tm *timeinfo = localtime(&now);

        char buffer[20];
        strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
        return String(buffer);
    }

    // Send device online notification
    static void sendDeviceOnline(const String& version, const String& deviceType) {
        if (!WiFi.isConnected()) {
            logTransportLocalOnly("❌ Cannot send Telegram: WiFi not connected");
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

    static bool sendDebugMessage(const String& message) {
        return sendMessage(message);
    }

    static bool sendMessageWithKeyboard(const String& message, const String& replyMarkup) {
        return sendMessage(message, replyMarkup);
    }

    // Send a watering notification with its own independent cooldown.
    // Debug/command failures won't block notification delivery.
    static bool sendNotificationMessage(const String& message) {
        if (!WiFi.isConnected()) {
            return false;
        }
        if (isNotifInCooldown()) {
            return false;
        }

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        int httpCode = -1;
        if (usingProxy) {
            String url = monitoringProxyBaseUrl() + "/v1/telegram/sendMessage";
            if (!beginHttpClient(http, url, client, plainClient)) {
                onNotifFailure();
                logTransportLocalOnly("❌ Telegram notification send begin failed (proxy)");
                return false;
            }
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            applyProxyAuthHeader(http);
            String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                          "&chat_id=" + urlEncode(String(TELEGRAM_CHAT_ID)) +
                          "&text=" + urlEncode(message) +
                          "&parse_mode=HTML";
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                         "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID +
                         "&text=" + urlEncode(message) +
                         "&parse_mode=HTML";

            if (!beginHttpClient(http, url, client, plainClient)) {
                onNotifFailure();
                logTransportLocalOnly("❌ Telegram notification send begin failed");
                return false;
            }
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.GET();
        }

        bool success = (httpCode == 200);

        if (success) {
            onNotifSuccess();
            logTransportLocalOnly("✓ Telegram notification sent");
        } else {
            onNotifFailure();
            logTransportLocalOnly("❌ Telegram notification send failed (" + String(usingProxy ? "proxy" : "direct") + "), HTTP code: " + String(httpCode));
            if (httpCode > 0) {
                logTransportLocalOnly("Response: " + http.getString());
            }
        }

        http.end();
        return success;
    }

    static String getHelpMessage() {
        String message = "📘 <b>AVAILABLE COMMANDS</b>\n\n";
        message += "<b>Watering</b>\n";
        message += "/water N - Water tray N (1-6)\n";
        message += "/start_all - Water all trays sequentially\n";
        message += "/halt - Block watering (OTA/web stay active)\n";
        message += "/resume - Exit halt mode\n\n";
        message += "<b>Diagnostics</b>\n";
        message += "/time - RTC time, battery, lamp status\n";
        message += "/settime - Sync from NTP\n";
        message += "/test_sensors - Test all rain sensors\n";
        message += "/overflow_status - Overflow sensor readings\n\n";
        message += "<b>Safety</b>\n";
        message += "/reset_overflow - Clear overflow lock\n";
        message += "/reinit_gpio - Reinitialize relay GPIOs\n\n";
        message += "<b>Plant Light</b>\n";
        message += "/lamp_status - Show light status\n";
        message += "/lamp_on /lamp_off /lamp_auto\n\n";
        message += "💡 Use /menu for quick-access buttons";

        return message;
    }

    static String getMainMenuKeyboard() {
        String kb = "{\"inline_keyboard\":[";
        kb += "[{\"text\":\"💧 1\",\"callback_data\":\"water_1\"},{\"text\":\"💧 2\",\"callback_data\":\"water_2\"},{\"text\":\"💧 3\",\"callback_data\":\"water_3\"}],";
        kb += "[{\"text\":\"💧 4\",\"callback_data\":\"water_4\"},{\"text\":\"💧 5\",\"callback_data\":\"water_5\"},{\"text\":\"💧 6\",\"callback_data\":\"water_6\"}],";
        kb += "[{\"text\":\"🚿 Water All\",\"callback_data\":\"start_all\"}],";
        kb += "[{\"text\":\"⏸ Halt\",\"callback_data\":\"halt\"},{\"text\":\"▶️ Resume\",\"callback_data\":\"resume\"}],";
        kb += "[{\"text\":\"🕐 Time\",\"callback_data\":\"time\"},{\"text\":\"🔍 Sensors\",\"callback_data\":\"test_sensors\"}],";
        kb += "[{\"text\":\"🌊 Overflow\",\"callback_data\":\"overflow_status\"},{\"text\":\"🔄 Reset\",\"callback_data\":\"reset_overflow\"}],";
        kb += "[{\"text\":\"💡 On\",\"callback_data\":\"lamp_on\"},{\"text\":\"🔌 Off\",\"callback_data\":\"lamp_off\"},{\"text\":\"🔄 Auto\",\"callback_data\":\"lamp_auto\"}]";
        kb += "]}";
        return kb;
    }

    static void answerCallbackQuery() {
        String& cbId = pendingCallbackQueryId();
        if (cbId.isEmpty() || !WiFi.isConnected()) return;

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        if (usingProxy) {
            String url = monitoringProxyBaseUrl() + "/v1/telegram/answerCallbackQuery";
            if (!beginHttpClient(http, url, client, plainClient)) { cbId = ""; return; }
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            applyProxyAuthHeader(http);
            String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                          "&callback_query_id=" + urlEncode(cbId);
            http.setTimeout(httpTimeoutMs(usingProxy));
            http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                         "/answerCallbackQuery?callback_query_id=" + urlEncode(cbId);
            if (!beginHttpClient(http, url, client, plainClient)) { cbId = ""; return; }
            http.setTimeout(httpTimeoutMs(usingProxy));
            http.GET();
        }

        http.end();
        cbId = "";
    }

    static void resetBotCommandsFlag() {
        botCommandsConfigured() = false;
    }

    static void ensureBotCommandsRegistered() {
        if (!WiFi.isConnected() || botCommandsConfigured()) {
            return;
        }

        unsigned long now = millis();
        if (lastBotCommandsAttemptMs() != 0 &&
            now - lastBotCommandsAttemptMs() < 60000) {
            return;
        }
        lastBotCommandsAttemptMs() = now;

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        int httpCode = -1;
        if (usingProxy) {
            String url = monitoringProxyBaseUrl() + "/v1/telegram/setMyCommands";
            if (!beginHttpClient(http, url, client, plainClient)) {
                logTransportLocalOnly("❌ Telegram setMyCommands begin failed (proxy)");
                return;
            }

            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            applyProxyAuthHeader(http);
            String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                          "&commands=" + urlEncode(getBotCommandsJson());
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        } else {
            String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                         "/setMyCommands";
            if (!beginHttpClient(http, url, client, plainClient)) {
                logTransportLocalOnly("❌ Telegram setMyCommands begin failed");
                return;
            }

            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            String body = "commands=" + urlEncode(getBotCommandsJson());
            http.setTimeout(httpTimeoutMs(usingProxy));
            httpCode = http.POST(body);
        }

        if (httpCode == 200) {
            botCommandsConfigured() = true;
            logTransportLocalOnly("✓ Telegram bot commands configured");
        } else {
            logTransportLocalOnly("❌ Telegram setMyCommands failed (" +
                                  String(usingProxy ? "proxy" : "direct") +
                                  "), HTTP code: " + String(httpCode));
            if (httpCode > 0) {
                logTransportLocalOnly("Response: " + http.getString());
            }
        }

        http.end();
    }

    // Format watering start notification (no network call)
    static String formatWateringStarted(const String& triggerType, const String& trayNumbers) {
        String timestamp = "Session " + getCurrentDateTime();

        String message = "🚿 <b>Watering Started</b>\n";
        message += "⏰ " + timestamp + "\n";
        message += "🔧 Trigger: " + triggerType + "\n";
        message += "🌱 Trays: " + trayNumbers;

        return message;
    }

    // Send watering start notification
    static void sendWateringStarted(const String& triggerType, const String& trayNumbers) {
        DebugHelper::debug("\n📱 Sending Telegram start notification...");
        sendMessage(formatWateringStarted(triggerType, trayNumbers));
    }

    // Format watering completion notification (no network call)
    static String formatWateringComplete(const String results[][3], int numTrays) {
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

        return message;
    }

    // Send watering completion notification with results table
    static void sendWateringComplete(const String results[][3], int numTrays) {
        DebugHelper::debug("\n📱 Sending Telegram completion notification...");
        sendMessage(formatWateringComplete(results, numTrays));
    }

    // Format watering schedule notification (no network call)
    // scheduleData[i][0] = tray number, [1] = planned time, [2] = duration, [3] = cycle (hours)
    static String formatWateringSchedule(const String scheduleData[][4], int numTrays, const String& title) {
        String message = "📅 <b>" + title + "</b>\n";
        message += "⏰ " + getCurrentDateTime() + "\n\n";
        message += "<pre>";
        message += " tr | planned     | dur  | cycle\n";
        message += "----|-------------|------|------\n";

        for (int i = 0; i < numTrays; i++) {
            String tray = scheduleData[i][0];
            String planned = scheduleData[i][1];
            String duration = scheduleData[i][2];
            String cycle = scheduleData[i][3];

            // Column 1: tray (3 chars, right-aligned)
            while (tray.length() < 3) tray = " " + tray;
            message += tray + " | ";

            // Column 2: planned time (11 chars, left-aligned)
            while (planned.length() < 11) planned = planned + " ";
            message += planned + " | ";

            // Column 3: duration (4 chars, right-aligned)
            while (duration.length() < 4) duration = " " + duration;
            message += duration + " | ";

            // Column 4: cycle
            message += cycle + "\n";
        }

        message += "</pre>";

        return message;
    }

    // Send watering schedule notification showing planned watering times
    static void sendWateringSchedule(const String scheduleData[][4], int numTrays, const String& title) {
        DebugHelper::debug("\n📱 Sending Telegram schedule notification...");
        sendMessage(formatWateringSchedule(scheduleData, numTrays, title));
    }

    // Check for Telegram commands using long polling.
    // Returns the command string or an empty string if no new command is found.
    // Handles both text messages and inline keyboard callback queries.
    // timeoutSeconds: How long Telegram server should wait for a new message (0 = immediate return)
    static String checkForCommands(int &lastUpdateId, int timeoutSeconds = 10) {
        if (!WiFi.isConnected()) {
            return "";
        }
        if (isInCooldown()) {
            return "";
        }

        HTTPClient http;
        WiFiClientSecure client;
        WiFiClient plainClient;
        bool usingProxy = useMonitoringProxy();

        String allowedUpdates = "[\"message\",\"callback_query\"]";
        String url;
        if (usingProxy) {
            url = monitoringProxyBaseUrl() + "/v1/telegram/getUpdates" +
                  String("?bot_token=") + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
                  "&offset=" + String(lastUpdateId) +
                  "&timeout=" + String(timeoutSeconds) +
                  "&allowed_updates=" + urlEncode(allowedUpdates);
        } else {
            url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                  "/getUpdates?offset=" + String(lastUpdateId) +
                  "&timeout=" + String(timeoutSeconds) +
                  "&allowed_updates=" + urlEncode(allowedUpdates);
        }

        if (!beginHttpClient(http, url, client, plainClient)) {
            onTelegramFailure();
            logTransportLocalOnly("❌ Telegram getUpdates begin failed (" + String(usingProxy ? "proxy" : "direct") + ")");
            return "";
        }
        if (usingProxy) {
            applyProxyAuthHeader(http);
        }

        // HTTP timeout must be slightly longer than the Telegram long poll timeout
        if (timeoutSeconds > 0) {
            http.setTimeout((timeoutSeconds + 1) * 1000);
        } else {
            http.setTimeout(httpTimeoutMs(usingProxy));
        }
        int httpCode = http.GET();

        if (httpCode == 200) {
            onTelegramSuccess();
            String payload = http.getString();

            int updateIdPos = payload.indexOf("\"update_id\":");
            if (updateIdPos > 0) {
                int updateIdStart = updateIdPos + 12;
                int updateIdEnd = payload.indexOf(",", updateIdStart);
                if (updateIdEnd > updateIdStart) {
                    String updateIdStr = payload.substring(updateIdStart, updateIdEnd);
                    int newUpdateId = updateIdStr.toInt();

                    // Check for callback_query (inline keyboard button press)
                    int cbPos = payload.indexOf("\"callback_query\"", updateIdPos);
                    if (cbPos > 0) {
                        // Extract callback query ID (string value)
                        int cbIdPos = payload.indexOf("\"id\":\"", cbPos);
                        if (cbIdPos > 0) {
                            int cbIdStart = cbIdPos + 6;
                            int cbIdEnd = payload.indexOf("\"", cbIdStart);
                            if (cbIdEnd > cbIdStart) {
                                pendingCallbackQueryId() = payload.substring(cbIdStart, cbIdEnd);
                            }
                        }
                        // Extract callback data (the command)
                        int dataPos = payload.indexOf("\"data\":\"", cbPos);
                        if (dataPos > 0) {
                            int dataStart = dataPos + 8;
                            int dataEnd = payload.indexOf("\"", dataStart);
                            if (dataEnd > dataStart) {
                                String command = payload.substring(dataStart, dataEnd);
                                lastUpdateId = newUpdateId + 1;
                                http.end();
                                return command;
                            }
                        }
                    }

                    // Fall back to regular message text
                    int textPos = payload.indexOf("\"text\":\"", updateIdPos);
                    if (textPos > 0) {
                        int textStart = textPos + 8;
                        int textEnd = payload.indexOf("\"", textStart);
                        if (textEnd > textStart) {
                            String command = payload.substring(textStart, textEnd);
                            lastUpdateId = newUpdateId + 1;
                            http.end();
                            return command;
                        }
                    }
                }
            }
        } else {
            onTelegramFailure();
            logTransportLocalOnly("❌ Telegram getUpdates failed (" + String(usingProxy ? "proxy" : "direct") + "), HTTP code: " + String(httpCode));
        }

        http.end();
        return "";
    }
};

// ============================================ 
// Global Function for DebugHelper
// ============================================ 
inline bool sendTelegramDebug(const String& message) {
    return TelegramNotifier::sendDebugMessage(message);
}

#endif // TELEGRAM_NOTIFIER_H

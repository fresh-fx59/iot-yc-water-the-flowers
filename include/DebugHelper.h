#ifndef DEBUG_HELPER_H
#define DEBUG_HELPER_H

#include <Arduino.h>
#include <time.h>
#include "config.h"

// Forward declaration
extern bool sendTelegramDebug(const String& msg);

// ============================================
// Telegram Message Queue Structure
// ============================================
struct TelegramQueueMessage {
    String message;
    String timestamp;
    int retryCount;
    unsigned long lastRetryTime;
    bool valid;
};

// ============================================
// Debug Helper - Queue-Based Telegram Logging
// ============================================
class DebugHelper {
private:
    // Message queue (circular buffer)
    static TelegramQueueMessage messageQueue[TELEGRAM_QUEUE_SIZE];
    static int queueHead;  // Next write position
    static int queueTail;  // Next read position
    static int queueCount; // Number of messages in queue

    // Current message being sent
    static int currentMessageIndex;
    static bool sendInProgress;
    static unsigned long lastProcessTime;

    // Message grouping (batch messages that arrive close together)
    static String groupingBuffer;
    static unsigned long lastGroupMessageTime;
    static unsigned long firstGroupMessageTime;  // Track when group started (for max age)

public:
    // Get current timestamp with milliseconds
    static String getCurrentTimestamp() {
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo)) {
            // Time not synced - use millis() as fallback
            unsigned long ms = millis();
            unsigned long seconds = ms / 1000;
            unsigned long minutes = seconds / 60;
            unsigned long hours = minutes / 60;
            int milliseconds = ms % 1000;

            char buffer[30];
            sprintf(buffer, "UPTIME %02lu:%02lu:%02lu.%03d",
                    hours % 24, minutes % 60, seconds % 60, milliseconds);
            return String(buffer);
        }

        // Format: DD-MM-YYYY HH:MM:SS.mmm
        char buffer[30];
        int milliseconds = millis() % 1000;
        sprintf(buffer, "%02d-%02d-%04d %02d:%02d:%02d.%03d",
                timeinfo.tm_mday,
                timeinfo.tm_mon + 1,
                timeinfo.tm_year + 1900,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec,
                milliseconds);
        return String(buffer);
    }

    // Queue a message for Telegram delivery with grouping
    static bool queueMessage(const String& message, bool important = false) {
        #if !IS_DEBUG_TO_TELEGRAM_ENABLED
        return false;
        #endif

        unsigned long currentTime = millis();
        String timestamp = getCurrentTimestamp();
        String formattedMessage = "[" + timestamp + "] " + message;

        // Grouping logic: batch messages that arrive within 2 seconds
        if (groupingBuffer.length() == 0) {
            // First message in group
            groupingBuffer = formattedMessage;
            lastGroupMessageTime = currentTime;
            firstGroupMessageTime = currentTime;  // Track group start time
        } else {
            // Check time delta from last message
            unsigned long delta = currentTime - lastGroupMessageTime;

            // SAFETY: Check if group is too old (prevents infinite buffering)
            unsigned long groupAge = currentTime - firstGroupMessageTime;
            if (groupAge >= MESSAGE_GROUP_MAX_AGE_MS) {
                // Group older than 10s - flush and start new group
                flushGroupBuffer();
                groupingBuffer = formattedMessage;
                lastGroupMessageTime = currentTime;
                firstGroupMessageTime = currentTime;
            } else if (delta < MESSAGE_GROUP_INTERVAL_MS) {
                // Within grouping window - add to buffer
                groupingBuffer += "\n" + formattedMessage;
                lastGroupMessageTime = currentTime;
            } else {
                // Delta >= 2s - flush current group and start new one
                flushGroupBuffer();
                groupingBuffer = formattedMessage;
                lastGroupMessageTime = currentTime;
                firstGroupMessageTime = currentTime;
            }
        }

        #if IS_DEBUG_TO_SERIAL_ENABLED
        DEBUG_SERIAL.println("📥 Grouped: " + formattedMessage);
        #endif

        return true;
    }

    // Flush the grouping buffer to the queue
    static void flushGroupBuffer() {
        if (groupingBuffer.length() == 0) return;

        // Check if queue is full
        if (queueCount >= TELEGRAM_QUEUE_SIZE) {
            #if IS_DEBUG_TO_SERIAL_ENABLED
            DEBUG_SERIAL.println("⚠️ Telegram queue FULL - dropping grouped message");
            #endif
            groupingBuffer = "";
            return;
        }

        // Add grouped message to queue
        messageQueue[queueHead].message = groupingBuffer;
        messageQueue[queueHead].timestamp = getCurrentTimestamp();
        messageQueue[queueHead].retryCount = 0;
        messageQueue[queueHead].lastRetryTime = 0;
        messageQueue[queueHead].valid = true;

        // Move head forward (circular)
        queueHead = (queueHead + 1) % TELEGRAM_QUEUE_SIZE;
        queueCount++;

        #if IS_DEBUG_TO_SERIAL_ENABLED
        DEBUG_SERIAL.println("📤 Flushed group to queue (Queue: " + String(queueCount) + "/" + String(TELEGRAM_QUEUE_SIZE) + ")");
        #endif

        // Clear buffer
        groupingBuffer = "";
    }

    // Send debug message (buffered)
    static void debug(const String& message) {
        #if IS_DEBUG_TO_SERIAL_ENABLED
        DEBUG_SERIAL.println(message);
        #endif

        #if IS_DEBUG_TO_TELEGRAM_ENABLED
        queueMessage(message, false);
        #endif
    }

    // Send important debug message (queued with priority handling)
    static void debugImportant(const String& message) {
        #if IS_DEBUG_TO_SERIAL_ENABLED
        DEBUG_SERIAL.println(message);
        #endif

        #if IS_DEBUG_TO_TELEGRAM_ENABLED
        queueMessage("🔴 " + message, true);
        #endif
    }

    // Process queue - call this in main loop
    static void loop() {
        #if !IS_DEBUG_TO_TELEGRAM_ENABLED
        return;
        #endif

        unsigned long currentTime = millis();

        // Check if grouping buffer should be flushed
        if (groupingBuffer.length() > 0) {
            unsigned long timeSinceLastMessage = currentTime - lastGroupMessageTime;
            unsigned long groupAge = currentTime - firstGroupMessageTime;

            // Flush if: 2s silence OR group older than 10s (safety)
            if (timeSinceLastMessage >= MESSAGE_GROUP_INTERVAL_MS || groupAge >= MESSAGE_GROUP_MAX_AGE_MS) {
                flushGroupBuffer();
            }
        }

        // Don't process if WiFi not connected
        if (!WiFi.isConnected()) {
            return;
        }

        // Check if queue is empty
        if (queueCount == 0) {
            sendInProgress = false;
            return;
        }

        // If no send in progress, start sending next message
        if (!sendInProgress) {
            currentMessageIndex = queueTail;
            sendInProgress = true;
            lastProcessTime = currentTime;
        }

        // Check retry delay
        if (currentTime - lastProcessTime < TELEGRAM_RETRY_DELAY_MS) {
            return; // Not time to retry yet
        }

        // Get current message
        TelegramQueueMessage* msg = &messageQueue[currentMessageIndex];

        if (!msg->valid) {
            // Skip invalid messages
            dequeueMessage();
            sendInProgress = false;
            return;
        }

        // Try to send message
        bool success = trySendToTelegram(msg->message);

        if (success) {
            #if IS_DEBUG_TO_SERIAL_ENABLED
            DEBUG_SERIAL.println("✓ Telegram sent: " + msg->timestamp + " (Queue: " + String(queueCount - 1) + ")");
            #endif

            // Remove from queue
            dequeueMessage();
            sendInProgress = false;
        } else {
            // Increment retry count
            msg->retryCount++;
            msg->lastRetryTime = currentTime;
            lastProcessTime = currentTime;

            #if IS_DEBUG_TO_SERIAL_ENABLED
            DEBUG_SERIAL.println("❌ Telegram failed: Retry " + String(msg->retryCount) + "/" + String(TELEGRAM_MAX_RETRY_ATTEMPTS));
            #endif

            // Check if max retries reached
            if (msg->retryCount >= TELEGRAM_MAX_RETRY_ATTEMPTS) {
                #if IS_DEBUG_TO_SERIAL_ENABLED
                DEBUG_SERIAL.println("⚠️ Message dropped after " + String(TELEGRAM_MAX_RETRY_ATTEMPTS) + " attempts");
                #endif

                // Give up and remove from queue
                dequeueMessage();
                sendInProgress = false;
            }
        }
    }

    // Get queue status
    static String getQueueStatus() {
        return "Queue: " + String(queueCount) + "/" + String(TELEGRAM_QUEUE_SIZE);
    }

    // Force flush grouping buffer (use before sending important notifications)
    static void flushBuffer() {
        if (groupingBuffer.length() > 0) {
            flushGroupBuffer();
        }
    }

    // Force flush (kept for compatibility, but queue handles everything now)
    static void flush() {
        // Flush grouping buffer first
        flushBuffer();
        // Process queue immediately if possible
        loop();
    }

private:
    // Remove message from queue
    static void dequeueMessage() {
        if (queueCount == 0) return;

        // Mark as invalid
        messageQueue[queueTail].valid = false;
        messageQueue[queueTail].message = "";

        // Move tail forward (circular)
        queueTail = (queueTail + 1) % TELEGRAM_QUEUE_SIZE;
        queueCount--;
    }

    // Try to send message to Telegram
    static bool trySendToTelegram(const String& message) {
        if (!WiFi.isConnected()) {
            return false;
        }

        // Format as code block for better readability
        String formattedMessage = "🐛 <b>Debug</b>\n<pre>" + message + "</pre>";

        // Call external Telegram send function and return its result
        bool success = sendTelegramDebug(formattedMessage);
        return success;
    }
};

// ============================================
// Static Member Initialization
// ============================================
TelegramQueueMessage DebugHelper::messageQueue[TELEGRAM_QUEUE_SIZE];
int DebugHelper::queueHead = 0;
int DebugHelper::queueTail = 0;
int DebugHelper::queueCount = 0;
int DebugHelper::currentMessageIndex = -1;
bool DebugHelper::sendInProgress = false;
unsigned long DebugHelper::lastProcessTime = 0;
String DebugHelper::groupingBuffer = "";
unsigned long DebugHelper::lastGroupMessageTime = 0;
unsigned long DebugHelper::firstGroupMessageTime = 0;

#endif // DEBUG_HELPER_H

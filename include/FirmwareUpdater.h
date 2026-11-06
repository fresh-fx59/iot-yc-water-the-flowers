#ifndef FIRMWARE_UPDATER_H
#define FIRMWARE_UPDATER_H

// Telegram firmware update with automatic rollback. Ported from the mini
// watering firmware (iot-yc-water-the-flowers-mini). Differences on this rig:
//  - the request runs from the top of networkTask, not inside command parsing;
//  - the server certificate is verified;
//  - a separate manifest folder and a board check, so mini images cannot land here;
//  - watering is held for the whole update and refused if a cycle is running;
//  - the Telegram command is confirmed before reboot, since the offset is RAM only;
//  - the health check needs metrics, a Telegram poll AND a verified manifest fetch:
//    an image that passes only the first two could not install the next update.

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/md.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#include "config.h"
#include "FirmwareUpdateLogic.h"
#include "OtaTrustRoots.h"
#include "TelegramNotifier.h"
#include "MetricsPusher.h"
#include "WateringSystem.h"

extern WateringSystem wateringSystem;
extern int lastUpdateId;

class FirmwareUpdater {
public:
    // Called from command parsing or the web API. Only records the request;
    // networkTask runs it after parsing has returned.
    static void requestCheck(bool force) { s_request = force ? REQ_FORCE : REQ_CHECK; }
    static void requestRollback() { s_request = REQ_ROLLBACK; }

    static void runPendingRequest() {
        uint8_t request = s_request;
        if (request == REQ_NONE) return;
        s_request = REQ_NONE;
        if (request == REQ_ROLLBACK) {
            rollbackToOtherPartition();
        } else {
            checkAndApply(request == REQ_FORCE);
        }
    }

    // First thing in setup(), before any hardware or network start, so a crash
    // anywhere later still counts as a failed boot.
    static void handleBootTrial() {
        Preferences prefs;
        prefs.begin("ota", false);
        bool trial = prefs.isKey("target_label");
        if (!trial) {
            prefs.end();
            return;
        }

        const esp_partition_t* running = esp_ota_get_running_partition();
        String runningLabel = running ? String(running->label) : String("");
        String targetLabel = prefs.getString("target_label", "");
        String targetVersion = prefs.getString("target_version", "");
        String prevVersion = prefs.getString("prev_version", "");
        uint8_t attempts = prefs.getUChar("attempts", 0);

        switch (FirmwareUpdateLogic::decideTrialAction(trial, targetLabel, attempts, runningLabel)) {
        case FirmwareUpdateLogic::TrialAction::NewBoot:
            prefs.putUChar("attempts", 1);
            prefs.end();
            s_trialArmed = true;
            s_trialStartMs = millis();
            s_trialVersion = targetVersion;
            Serial.printf("[FirmwareUpdater] Health check started for v%s\n", targetVersion.c_str());
            return;
        case FirmwareUpdateLogic::TrialAction::PendingRollback: {
            Serial.println("[FirmwareUpdater] New image did not confirm; flipping back");
            const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
            if (other != nullptr && esp_ota_set_boot_partition(other) == ESP_OK) {
                prefs.end();
                delay(200);
                ESP.restart();
            }
            // Never restart in a loop when the flip is refused.
            prefs.clear();
            prefs.end();
            s_bootNotice = "⚠️ Rollback failed: v" + targetVersion + " keeps running. Check it.";
            return;
        }
        case FirmwareUpdateLogic::TrialAction::RolledBack:
            s_bootNotice = "↩️ Rollback: v" + targetVersion +
                           " failed its health check. Running v" + prevVersion + " again.";
            prefs.clear();
            prefs.end();
            return;
        case FirmwareUpdateLogic::TrialAction::NoTrial:
        default:
            prefs.end();
            return;
        }
    }

    static void sendBootNoticeIfAny() {
        if (s_bootNotice.length() == 0) return;
        // Retried by the network loop until sent; the Telegram cooldown limits the rate.
        if (TelegramNotifier::sendNotificationMessage(s_bootNotice)) {
            MetricsPusher::logInfo("OTA: " + s_bootNotice);
            s_bootNotice = "";
        }
    }

    // Runs with or without WiFi: an image that breaks WiFi must still roll back.
    static void loopHealthCheck() {
        if (!s_trialArmed) return;

#ifndef OTA_TEST_FAIL_HEALTH
        if (MetricsPusher::successfulMetricsPushes() >= 1 && TelegramNotifier::successfulPolls() >= 1 &&
            canReachManifest()) {
            esp_ota_mark_app_valid_cancel_rollback();
            clearTrialNvs();
            s_trialArmed = false;
            notify("✅ v" + s_trialVersion + " confirmed healthy.");
            MetricsPusher::logInfo("OTA: v" + s_trialVersion + " confirmed healthy, stack free " +
                                   String(uxTaskGetStackHighWaterMark(NULL)));
            return;
        }
#endif

        if (millis() - s_trialStartMs > OTA_HEALTH_DEADLINE_MS) {
            s_trialArmed = false;
            notify("⏱️ v" + s_trialVersion + " failed the health check. Rolling back...");
            // Keep the trial record: the next boot reports the rollback from it.
            const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
            bool wasHalted = wateringSystem.isHaltMode();
            wateringSystem.setHaltMode(true);  // closes valves, stops the pump
            if (other != nullptr && esp_ota_set_boot_partition(other) == ESP_OK) {
                delay(500);
                ESP.restart();
            }
            clearTrialNvs();
            wateringSystem.setHaltMode(wasHalted);
            notify("⚠️ Rollback failed: v" + s_trialVersion + " keeps running. Check it.");
        }
    }

private:
    enum : uint8_t { REQ_NONE, REQ_CHECK, REQ_FORCE, REQ_ROLLBACK };

    static volatile uint8_t s_request;
    static String   s_bootNotice;
    static bool     s_trialArmed;
    static uint32_t s_trialStartMs;
    static String   s_trialVersion;
    static uint8_t  s_downloadBuffer[OTA_DOWNLOAD_CHUNK_BYTES];

    // Stack left at the deepest point so far. Shows the margin for the TLS handshake.
    static String stackFree() {
        return "stack free " + String(uxTaskGetStackHighWaterMark(NULL));
    }

    // Verified fetch and parse of the manifest. Rate limited: it runs in the
    // health loop until it succeeds or the deadline passes.
    static bool canReachManifest() {
        static unsigned long lastTryMs = 0;
        if (!WiFi.isConnected()) return false;
        if (lastTryMs != 0 && millis() - lastTryMs < OTA_HEALTH_MANIFEST_RETRY_MS) return false;
        lastTryMs = millis();
        HTTPClient http;
        WiFiClientSecure client;
        if (!beginHttp(http, proxyBaseUrl() + OTA_MANIFEST_PATH, client)) return false;
        http.setTimeout(OTA_MANIFEST_HTTP_TIMEOUT_MS);
        int code = http.GET();
        bool ok = false;
        if (code == 200) {
            ok = FirmwareUpdateLogic::parseManifest(http.getString(), OTA_BOARD_ID, OTA_URL_PREFIX).valid;
        } else {
            MetricsPusher::logWarn("OTA: health manifest check HTTP " + String(code) + tlsErrorSuffix(client));
        }
        http.end();
        return ok;
    }

    static bool notify(const String& message) {
        MetricsPusher::logInfo("OTA: " + message);
        return TelegramNotifier::sendNotificationMessage(message);
    }

    static String proxyBaseUrl() {
        String base = String(TELEGRAM_PROXY_BASE_URL);
        while (base.endsWith("/")) base.remove(base.length() - 1);
        return base;
    }

    static void clearTrialNvs() {
        Preferences prefs;
        prefs.begin("ota", false);
        prefs.clear();
        prefs.end();
    }

    static bool beginHttp(HTTPClient& http, const String& url, WiFiClientSecure& client) {
        if (!url.startsWith("https://")) return false;  // firmware never over plain HTTP
        client.setCACert(OTA_TRUST_ROOTS_PEM);
        if (!http.begin(client, url)) return false;
        http.addHeader("Authorization", "Bearer " + String(TELEGRAM_PROXY_AUTH_TOKEN));
        return true;
    }

    static String tlsErrorSuffix(WiFiClientSecure& client) {
        char text[100] = {0};
        int code = client.lastError(text, sizeof(text));
        return code == 0 ? String("") : " (TLS " + String(code) + ": " + String(text) + ")";
    }

    static bool downloadInto(const String& url, mbedtls_md_context_t& md, uint32_t expectedSize, String& error) {
        HTTPClient http;
        WiFiClientSecure client;
        if (!beginHttp(http, url, client)) {
            error = "bad URL";
            return false;
        }
        http.setTimeout(OTA_DOWNLOAD_HTTP_TIMEOUT_MS);
        int code = http.GET();
        if (code != 200) {
            error = "HTTP " + String(code) + tlsErrorSuffix(client);
            http.end();
            return false;
        }

        WiFiClient* stream = http.getStreamPtr();
        uint32_t written = 0;
        unsigned long lastByteMs = millis();
        while (written < expectedSize) {
            size_t available = stream->available();
            if (available == 0) {
                if (!http.connected()) break;
                if (millis() - lastByteMs > OTA_DOWNLOAD_HTTP_TIMEOUT_MS) {
                    error = "stalled at " + String(written) + " bytes";
                    http.end();
                    return false;
                }
                vTaskDelay(1);
                continue;
            }
            size_t toRead = min(available, sizeof(s_downloadBuffer));
            toRead = min(toRead, (size_t)(expectedSize - written));
            int got = stream->readBytes(s_downloadBuffer, toRead);
            if (got <= 0) break;
            mbedtls_md_update(&md, s_downloadBuffer, got);
            if (Update.write(s_downloadBuffer, got) != (size_t)got) {
                error = "flash write failed";
                http.end();
                return false;
            }
            written += got;
            lastByteMs = millis();
            vTaskDelay(1);  // let the idle task feed the watchdog
        }
        http.end();
        if (written != expectedSize) {
            error = "got " + String(written) + " of " + String(expectedSize) + " bytes";
            return false;
        }
        return true;
    }

    static String toHex(const uint8_t* bytes) {
        static const char* digits = "0123456789abcdef";
        String out;
        out.reserve(64);
        for (int i = 0; i < 32; i++) {
            out += digits[bytes[i] >> 4];
            out += digits[bytes[i] & 0x0F];
        }
        return out;
    }

    // Without this the command stays unconfirmed at Telegram and the reboot
    // replays it. Fail closed: no confirm, no reboot.
    static bool confirmCommandOrReport() {
        if (TelegramNotifier::confirmUpdates(lastUpdateId)) return true;
        notify("❌ Stopped: could not confirm the command with Telegram. Nothing changed.");
        return false;
    }

    static void checkAndApply(bool force) {
        using namespace FirmwareUpdateLogic;

        notify("🔎 Checking for update...");
        if (s_trialArmed) {
            notify("⏳ Busy: v" + s_trialVersion + " is still in its health check. Retry in a few minutes.");
            return;
        }

        // 1. Manifest
        String manifestJson;
        {
            HTTPClient http;
            WiFiClientSecure client;
            if (!beginHttp(http, proxyBaseUrl() + OTA_MANIFEST_PATH, client)) {
                notify("❌ Update failed: proxy URL is not HTTPS.");
                return;
            }
            http.setTimeout(OTA_MANIFEST_HTTP_TIMEOUT_MS);
            int code = http.GET();
            if (code != 200) {
                notify("❌ Update failed: manifest HTTP " + String(code) + tlsErrorSuffix(client) + " (" + stackFree() + ")");
                http.end();
                return;
            }
            manifestJson = http.getString();
            http.end();
        }
        MetricsPusher::logInfo("OTA: manifest fetched, " + stackFree());
        ParsedManifest manifest = parseManifest(manifestJson, OTA_BOARD_ID, OTA_URL_PREFIX);
        if (!manifest.valid) {
            notify("❌ Update failed: bad manifest.");
            return;
        }

        // 2. Decide
        switch (decideUpdate(FIRMWARE_VERSION, manifest, force, wateringSystem.isWateringBusy())) {
        case UpdateDecision::AlreadyUpToDate:
            notify("✅ Up to date (v" + String(FIRMWARE_VERSION) + "). /check_update force re-flashes.");
            return;
        case UpdateDecision::Busy:
            notify("⏳ Busy: watering in progress. Retry after the cycle, or send /halt first.");
            return;
        case UpdateDecision::Apply:
            break;
        }

        const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
        if (target == nullptr) {
            notify("❌ Update failed: no update partition.");
            return;
        }
        if (!confirmCommandOrReport()) return;
        if (!wateringSystem.beginFirmwareUpdateHold()) {
            notify("⏳ Busy: watering just started. Retry after the cycle.");
            return;
        }

        String announce = "⬇️ Updating v" + String(FIRMWARE_VERSION) + " → v" + manifest.version +
                          " (" + String(manifest.size / 1024) + " KB). Watering is on hold.";
        if (manifest.notes.length() > 0) announce += " Notes: " + manifest.notes;
        notify(announce);

        // 3. Download and hash
        String failure;
        if (!Update.begin(manifest.size, U_FLASH)) {
            failure = "image does not fit";
        } else {
            mbedtls_md_context_t md;
            mbedtls_md_init(&md);
            mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
            mbedtls_md_starts(&md);
            String downloadError;
            bool downloaded = downloadInto(proxyBaseUrl() + manifest.url, md, manifest.size, downloadError);
            uint8_t digest[32];
            mbedtls_md_finish(&md, digest);
            mbedtls_md_free(&md);

            // 4. Verify, record the trial, activate
            if (!downloaded) {
                failure = "download: " + downloadError;
            } else if (toHex(digest) != manifest.sha256) {
                failure = "checksum mismatch";
            } else {
                Preferences prefs;
                prefs.begin("ota", false);
                prefs.clear();
                prefs.putString("target_label", String(target->label));
                prefs.putString("target_version", manifest.version);
                prefs.putString("prev_version", String(FIRMWARE_VERSION));
                prefs.putUChar("attempts", 0);
                prefs.end();
                if (Update.end(true)) {
                    notify("🔁 Flashed v" + manifest.version + ". Rebooting... (" + stackFree() + ")");
                    delay(500);
                    ESP.restart();
                }
                clearTrialNvs();
                failure = "image not accepted (error " + String(Update.getError()) + ")";
            }
            if (Update.isRunning()) Update.abort();
        }

        wateringSystem.endFirmwareUpdateHold();
        notify("❌ Update failed: " + failure + ". Still on v" + String(FIRMWARE_VERSION) + ". Watering resumed. (" + stackFree() + ")");
    }

    static void rollbackToOtherPartition() {
        const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
        if (other == nullptr) {
            notify("❌ Rollback failed: no other partition.");
            return;
        }
        esp_app_desc_t description;
        if (esp_ota_get_partition_description(other, &description) != ESP_OK) {
            notify("❌ Rollback failed: other partition has no valid image.");
            return;
        }
        if (wateringSystem.isWateringBusy()) {
            notify("⏳ Busy: watering in progress. Retry after the cycle, or send /halt first.");
            return;
        }
        if (!confirmCommandOrReport()) return;
        if (!wateringSystem.beginFirmwareUpdateHold()) {
            notify("⏳ Busy: watering just started. Retry after the cycle.");
            return;
        }
        clearTrialNvs();
        s_trialArmed = false;
        notify("↩️ Rolling back to the other partition...");
        if (esp_ota_set_boot_partition(other) == ESP_OK) {
            delay(500);
            ESP.restart();
        }
        wateringSystem.endFirmwareUpdateHold();
        notify("❌ Rollback failed: other partition refused. Watering resumed.");
    }
};

volatile uint8_t FirmwareUpdater::s_request = FirmwareUpdater::REQ_NONE;
String   FirmwareUpdater::s_bootNotice = "";
bool     FirmwareUpdater::s_trialArmed = false;
uint32_t FirmwareUpdater::s_trialStartMs = 0;
String   FirmwareUpdater::s_trialVersion = "";
uint8_t  FirmwareUpdater::s_downloadBuffer[OTA_DOWNLOAD_CHUNK_BYTES];

#endif  // FIRMWARE_UPDATER_H

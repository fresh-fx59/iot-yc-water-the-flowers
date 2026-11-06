#ifndef FIRMWARE_UPDATE_LOGIC_H
#define FIRMWARE_UPDATE_LOGIC_H

// Pure decisions behind the Telegram firmware update. No hardware calls, so
// the native test env covers all of it. FirmwareUpdater.h does the I/O.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace FirmwareUpdateLogic {

struct ParsedManifest {
    String   version;
    String   url;
    String   sha256;
    String   notes;
    uint32_t size;
    bool     valid;
};

enum class UpdateDecision { AlreadyUpToDate, Apply, Busy };

enum class TrialAction {
    NoTrial,          // no trial recorded: normal boot
    NewBoot,          // first boot of the new image: start the health check
    PendingRollback,  // the last boot of the new image never confirmed: flip back
    RolledBack,       // running image is not the trial image: tell the operator
};

// Dotted numeric compare ("1.1.10" > "1.1.2"). Returns -1, 0 or +1.
inline int compareVersion(const char* a, const char* b) {
    if (a == nullptr) a = "";
    if (b == nullptr) b = "";
    while (*a || *b) {
        long va = 0, vb = 0;
        while (*a && *a != '.') {
            if (*a >= '0' && *a <= '9') va = va * 10 + (*a - '0');
            a++;
        }
        while (*b && *b != '.') {
            if (*b >= '0' && *b <= '9') vb = vb * 10 + (*b - '0');
            b++;
        }
        if (va != vb) return (va < vb) ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

// Exactly three dot-separated digit groups.
inline bool isStrictVersion(const String& v) {
    int dots = 0;
    bool digitInGroup = false;
    for (unsigned int i = 0; i < v.length(); i++) {
        char c = v[i];
        if (c == '.') {
            if (!digitInGroup) return false;
            dots++;
            digitInGroup = false;
        } else if (c >= '0' && c <= '9') {
            digitInGroup = true;
        } else {
            return false;
        }
    }
    return dots == 2 && digitInGroup;
}

inline bool isLowerHexOfLength(const String& s, unsigned int length) {
    if (s.length() != length) return false;
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// expectedBoard stops an image for other hardware. urlPrefix pins the download
// to this device's own folder: url must equal urlPrefix + version + ".bin".
inline ParsedManifest parseManifest(const String& json,
                                    const char* expectedBoard,
                                    const char* urlPrefix) {
    ParsedManifest m;
    m.size = 0;
    m.valid = false;

    StaticJsonDocument<768> doc;
    if (deserializeJson(doc, json.c_str())) return m;

    if (!doc["version"].is<const char*>()) return m;
    if (!doc["url"].is<const char*>()) return m;
    if (!doc["sha256"].is<const char*>()) return m;
    if (!doc["board"].is<const char*>()) return m;
    if (!doc["size"].is<uint32_t>()) return m;

    if (String(doc["board"].as<const char*>()) != String(expectedBoard)) return m;

    m.version = String(doc["version"].as<const char*>());
    m.url     = String(doc["url"].as<const char*>());
    m.sha256  = String(doc["sha256"].as<const char*>());
    m.size    = doc["size"].as<uint32_t>();
    if (doc["notes"].is<const char*>()) m.notes = String(doc["notes"].as<const char*>());

    m.sha256.toLowerCase();
    if (!isStrictVersion(m.version)) return m;
    if (m.url != String(urlPrefix) + m.version + ".bin") return m;
    if (!isLowerHexOfLength(m.sha256, 64)) return m;
    if (m.size == 0) return m;

    m.valid = true;
    return m;
}

inline UpdateDecision decideUpdate(const char* runningVersion,
                                   const ParsedManifest& manifest,
                                   bool force,
                                   bool wateringBusy) {
    if (wateringBusy) return UpdateDecision::Busy;
    if (force) return UpdateDecision::Apply;
    if (compareVersion(runningVersion, manifest.version.c_str()) < 0) return UpdateDecision::Apply;
    return UpdateDecision::AlreadyUpToDate;
}

inline TrialAction decideTrialAction(bool trialPresent,
                                     const String& targetLabel,
                                     uint8_t attempts,
                                     const String& runningLabel) {
    if (!trialPresent) return TrialAction::NoTrial;
    if (targetLabel != runningLabel) return TrialAction::RolledBack;
    if (attempts == 0) return TrialAction::NewBoot;
    return TrialAction::PendingRollback;
}

// One update's text from a getUpdates payload: from its "update_id" up to the
// next one. Parsing inside this slice keeps a later update's fields out.
inline String firstUpdateSlice(const String& payload) {
    const char* key = "\"update_id\":";
    int start = payload.indexOf(key);
    if (start < 0) return String();
    int next = payload.indexOf(key, start + 12);
    return next < 0 ? payload.substring(start) : payload.substring(start, next);
}

// The sender id of an update. Works for message and callback_query. Quotes in
// message text are JSON-escaped, so user text cannot imitate this key; the
// leading quote also skips "forward_from".
inline String parseFromId(const String& update) {
    const char* key = "\"from\":{\"id\":";
    int p = update.indexOf(key);
    if (p < 0) return String();
    int start = p + 13;
    int end = update.indexOf(',', start);
    if (end < 0) end = update.indexOf('}', start);
    if (end <= start) return String();
    String id = update.substring(start, end);
    id.trim();
    return id;
}

inline bool isAuthorizedSender(const String& update, const char* allowedId) {
    String sender = parseFromId(update);
    return sender.length() > 0 && sender == String(allowedId);
}

}  // namespace FirmwareUpdateLogic

#endif  // FIRMWARE_UPDATE_LOGIC_H

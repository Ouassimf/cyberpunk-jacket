#include "storage.h"

#include "config.h"
#include "log.h"

bool NvsStorage::begin() {
    open_ = prefs_.begin("jacket", false);
    if (!open_) logLine("[nvs] open FAILED: running on defaults, nothing will be saved");
    return open_;
}

size_t NvsStorage::read(const char* key, uint8_t* out, size_t cap) {
    if (!open_ || key == nullptr || out == nullptr || !prefs_.isKey(key)) return 0;
    const size_t len = prefs_.getBytesLength(key);
    if (len == 0 || len > cap) return 0;
    return prefs_.getBytes(key, out, len) == len ? len : 0;
}

bool NvsStorage::write(const char* key, const uint8_t* data, size_t len) {
    if (!open_ || key == nullptr || data == nullptr || len == 0) return false;
    return prefs_.putBytes(key, data, len) == len;
}

bool NvsStorage::erase(const char* key) {
    if (!open_ || key == nullptr) return false;
    if (!prefs_.isKey(key)) return true;  // removing what is not there is not an error
    return prefs_.remove(key);
}

void Saver::poll(uint32_t now_ms) {
    if (!engine_.needsSave()) {
        failed_ = false;
        return;
    }
    if (failed_) {
        if (static_cast<int32_t>(now_ms - retry_at_ms_) < 0) return;
    } else if (now_ms - engine_.lastChangeMs() < cfg::kSaveDelayMs) {
        return;
    }
    if (engine_.save()) {
        failed_ = false;
        logLine("[nvs] saved");
    } else {
        if (!failed_) logLine("[nvs] save FAILED, retrying every %lus", static_cast<unsigned long>(cfg::kSaveRetryMs / 1000));
        failed_ = true;
        retry_at_ms_ = now_ms + cfg::kSaveRetryMs;
    }
}

void Saver::flush() {
    if (!engine_.needsSave()) return;
    logLine(engine_.save() ? "[nvs] saved" : "[nvs] save FAILED");
}

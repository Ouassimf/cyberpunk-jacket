// Persistence: the engine's Storage on the ESP32's NVS flash partition, and
// the debounce that decides when the engine's pending changes are written.
#pragma once

#include <Preferences.h>
#include <jacket.h>

// Blobs in the NVS namespace "jacket". Until begin() succeeds every read finds
// nothing and every write fails, so the jacket still runs, with defaults.
class NvsStorage : public jacket::Storage {
public:
    bool begin();

    size_t read(const char* key, uint8_t* out, size_t cap) override;
    bool write(const char* key, const uint8_t* data, size_t len) override;
    bool erase(const char* key) override;

private:
    Preferences prefs_;
    bool open_ = false;
};

// Writes the engine's scene and settings to flash once they have stopped
// changing for cfg::kSaveDelayMs, so dragging a slider costs one flash write.
class Saver {
public:
    explicit Saver(jacket::Engine& engine) : engine_(engine) {}

    void poll(uint32_t now_ms);  // call every loop pass
    void flush();                // write now: before a reboot or a firmware upload

private:
    jacket::Engine& engine_;
    bool failed_ = false;
    uint32_t retry_at_ms_ = 0;
};

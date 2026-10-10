#include "boot_guard.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <rtc_wdt.h>
#include <string.h>

#include "config.h"
#include "log.h"

// Arduino's start-up code asks this before confirming a pending image. True:
// leave it pending, BootGuard::poll() confirms it later.
extern "C" bool verifyRollbackLater() { return true; }

namespace {

constexpr char kRecordKey[] = "boot";        // jacket::BootRecord
constexpr char kLegacyCrashKey[] = "crash";  // the bare crash counter of firmware 2.0.0

// Resets the whole chip, RTC domain included, as power-on would (see
// boot_guard.h): the RTC watchdog, set to expire in a moment with its
// "reset the system and the RTC" action. Does not return.
constexpr unsigned kFullResetAfterMs = 50;

void fullChipReset() {
    Serial.flush();
    delay(50);  // the last line leaves the serial port
    rtc_wdt_protect_off();
    rtc_wdt_disable();
    rtc_wdt_set_length_of_reset_signal(RTC_WDT_SYS_RESET_SIG, RTC_WDT_LENGTH_3_2us);
    rtc_wdt_set_stage(RTC_WDT_STAGE0, RTC_WDT_STAGE_ACTION_RESET_RTC);
    rtc_wdt_set_time(RTC_WDT_STAGE0, kFullResetAfterMs);
    rtc_wdt_enable();
    rtc_wdt_protect_on();
    const uint32_t armed_ms = millis();
    while (millis() - armed_ms < 1000) {
    }
    // It should have happened 950 ms ago. Better some restart than none.
    logLine("[boot] the full reset did NOT happen: restarting the ordinary way");
    Serial.flush();
    esp_restart();
}

bool isCrash(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

const char* reasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "reset-pin";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INTERRUPT-WATCHDOG";
        case ESP_RST_TASK_WDT: return "TASK-WATCHDOG";
        case ESP_RST_WDT: return "WATCHDOG";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT: return "BROWN-OUT";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

// Whether the slot begins like a firmware image. Asked first: ESP-IDF answers
// questions about an empty slot with error lines in the log.
bool looksLikeImage(const esp_partition_t* slot) {
    uint8_t magic = 0;
    return esp_partition_read(slot, 0, &magic, 1) == ESP_OK && magic == ESP_IMAGE_HEADER_MAGIC;
}

// The identity of the image in a slot: the start of the SHA-256 that the
// build appended to it. Read from the image's end, not computed.
bool imageId(const esp_partition_t* slot, uint8_t* out) {
    uint8_t sha[32];
    if (!looksLikeImage(slot) || esp_partition_get_sha256(slot, sha) != ESP_OK) return false;
    memcpy(out, sha, jacket::kImageIdSize);
    return true;
}

void hexId(const uint8_t* id, char* out) {  // 2 * kImageIdSize + 1 characters
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < jacket::kImageIdSize; ++i) {
        *out++ = digits[id[i] >> 4];
        *out++ = digits[id[i] & 15];
    }
    *out = '\0';
}

}  // namespace

void BootGuard::inspectOther() {
    other_usable_ = false;
    other_ = esp_ota_get_next_update_partition(nullptr);
    if (other_ == nullptr || other_ == running_) {
        other_ = nullptr;
        snprintf(other_note_, sizeof other_note_, "none: the partition table has a single app slot");
        return;
    }
    if (!looksLikeImage(other_)) {
        snprintf(other_note_, sizeof other_note_, "empty");
        return;
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(other_, &state) == ESP_OK && (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)) {
        snprintf(other_note_, sizeof other_note_, "a firmware the bootloader marked as failed");
        return;
    }
    if (!imageId(other_, other_image_)) {
        snprintf(other_note_, sizeof other_note_, "not a valid firmware image");
        return;
    }
    char id[2 * jacket::kImageIdSize + 1];
    hexId(other_image_, id);
    esp_app_desc_t about;
    if (esp_ota_get_partition_description(other_, &about) == ESP_OK) {
        // Every Arduino build carries the same project name; the ESP-IDF
        // version is what tells this firmware's family from another.
        snprintf(other_note_, sizeof other_note_, "a firmware, image %s, ESP-IDF %.20s%s", id, about.idf_ver,
                 strncmp(about.idf_ver, esp_get_idf_version(), sizeof about.idf_ver) == 0 ? " (as this one)" : "");
    } else {
        snprintf(other_note_, sizeof other_note_, "a firmware, image %s, without a description", id);
    }
    other_usable_ = true;
}

void BootGuard::logSlots() const {
    if (running_ == nullptr) {
        logLine("[boot] slots: the running partition is UNKNOWN");
        return;
    }
    char id[2 * jacket::kImageIdSize + 1];
    hexId(image_, id);
    esp_image_metadata_t image = {};
    const esp_partition_pos_t where = {running_->address, running_->size};
    const bool sized = esp_image_get_metadata(&where, &image) == ESP_OK;
    logLine("[boot] slot %s at 0x%lx, %lu bytes, holds image %s of %lu bytes, %s", running_->label,
            static_cast<unsigned long>(running_->address), static_cast<unsigned long>(running_->size), id,
            static_cast<unsigned long>(sized ? image.image_len : 0),
            record_.confirmed ? "confirmed healthy earlier" : record_.arrived_by_switch ? "started by a switch back, not confirmed since"
                                                                                       : "not confirmed yet");
    if (other_ != nullptr) {
        logLine("[boot] other slot %s at 0x%lx, %lu bytes: %s%s", other_->label, static_cast<unsigned long>(other_->address),
                static_cast<unsigned long>(other_->size), other_note_,
                other_usable_ ? " (checked in full before any switch)" : "");
    } else {
        logLine("[boot] other slot: %s", other_note_);
    }
}

bool BootGuard::store() {
    uint8_t blob[jacket::kBootRecordSize];
    const size_t n = jacket::serializeBootRecord(record_, blob, sizeof blob);
    const bool ok = storage_ != nullptr && n != 0 && storage_->write(kRecordKey, blob, n);
    if (!ok && !store_failed_) logLine("[boot] storing the boot record FAILED, retrying");
    store_failed_ = !ok;
    record_dirty_ = !ok;
    return ok;
}

void BootGuard::flushRecord(uint32_t now_ms) {
    if (!record_dirty_ || (store_failed_ && static_cast<int32_t>(now_ms - store_retry_ms_) < 0)) return;
    if (!store()) store_retry_ms_ = now_ms + cfg::kSaveRetryMs;
}

void BootGuard::begin(jacket::Storage& storage) {
    storage_ = &storage;
    const esp_reset_reason_t reason = esp_reset_reason();
    reason_ = reasonName(reason);

    running_ = esp_ota_get_running_partition();
    if (running_ != nullptr) {
        slot_ = running_->label;
        if (!imageId(running_, image_)) {  // cannot happen for an image that was just booted; any stable value will do
            memcpy(image_, esp_app_get_description()->app_elf_sha256, sizeof image_);
        }
    }
    esp_ota_img_states_t state;
    pending_ = running_ != nullptr && esp_ota_get_state_partition(running_, &state) == ESP_OK &&
               state == ESP_OTA_IMG_PENDING_VERIFY;
    inspectOther();

    uint8_t blob[jacket::kBootRecordSize];
    jacket::BootRecord stored;
    const bool have_record = storage.read(kRecordKey, blob, sizeof blob) == sizeof blob &&
                             jacket::deserializeBootRecord(blob, sizeof blob, stored);
    storage.erase(kLegacyCrashKey);

    jacket::BootFacts facts;
    facts.image = image_;
    facts.crashed = isCrash(reason);
    facts.other_usable = other_usable_;
    facts.crash_threshold = cfg::kSafeModeCrashes;
    const jacket::BootDecision decision = jacket::decideBoot(have_record ? &stored : nullptr, facts);
    record_ = decision.record;
    record_dirty_ = decision.changed;
    // Started by the reset another firmware of ours announced: a watchdog
    // reset by its looks, a hand-over by the record.
    if (decision.handed_over && isCrash(reason)) reason_ = "handed-over";

    if (decision.action == jacket::BootAction::SwitchSlot) {
        logLine(" ");
        logLine("[boot] jacket fw=%s build=\"%s\" slot=%s reset=%s: this firmware crashed %u times in a row and never ran healthy",
                JACKET_FW_VERSION, JACKET_BUILD " " __DATE__ " " __TIME__, slot_, reason_, static_cast<unsigned>(record_.crashes));
        logSlots();
        switchToOtherSlot("going back to the previous firmware");
        logLine("[boot] no way back: starting in safe mode instead");
    }
    if (record_dirty_) store();
    safe_mode_ = decision.action != jacket::BootAction::Normal;
    counter_settled_ = record_.crashes == 0;
}

bool BootGuard::switchToOtherSlot(const char* why, void (*before_leaving)()) {
    if (other_ == nullptr || !other_usable_) {
        logLine("[boot] %s: NOT possible, the other slot holds %s", why, other_note_);
        return false;
    }
    // Reading megabytes of flash twice (selecting verifies again): a loop
    // pass of seconds. Measured at 80 MHz: 0.7 s per pass over 1.3 MB.
    disableLoopWDT();
    bool selected = false;
    esp_image_metadata_t image = {};
    do {
        // 1. The whole image, read and checked.
        const esp_partition_pos_t where = {other_->address, other_->size};
        const esp_err_t verified = esp_image_verify(ESP_IMAGE_VERIFY, &where, &image);
        if (verified != ESP_OK) {
            logLine("[boot] %s: NOT possible, the firmware in slot %s does not verify (error 0x%x)", why, other_->label,
                    static_cast<unsigned>(verified));
            other_usable_ = false;
            snprintf(other_note_, sizeof other_note_, "a firmware that does not verify");
            break;
        }
        // 2. The record first, and only if it is really there afterwards: it
        // is what keeps the other firmware, if it is one of ours, from
        // switching straight back, and from taking the reset for a crash.
        const jacket::BootRecord next = jacket::recordForSwitch(other_image_);
        uint8_t blob[jacket::kBootRecordSize];
        uint8_t check[jacket::kBootRecordSize];
        const size_t n = jacket::serializeBootRecord(next, blob, sizeof blob);
        if (storage_ == nullptr || n == 0 || !storage_->write(kRecordKey, blob, n) ||
            storage_->read(kRecordKey, check, sizeof check) != n || memcmp(blob, check, n) != 0) {
            logLine("[boot] %s: NOT done, the boot record could not be stored", why);
            record_dirty_ = true;  // put this image's own record back
            store();
            break;
        }
        // 3. Select.
        const esp_err_t set = esp_ota_set_boot_partition(other_);
        if (set != ESP_OK) {
            logLine("[boot] %s: NOT done, selecting slot %s was refused (error 0x%x)", why, other_->label, static_cast<unsigned>(set));
            record_dirty_ = true;
            store();
            break;
        }
        selected = true;
    } while (false);
    enableLoopWDT();
    if (!selected) return false;

    logLine("[boot] %s: slot %s verified (%lu bytes) and selected. Resetting the whole chip into it", why, other_->label,
            static_cast<unsigned long>(image.image_len));
    // 4. and 5.
    if (before_leaving != nullptr) before_leaving();
    feedLoopWDT();
    fullChipReset();
    return true;  // not reached
}

void BootGuard::poll(uint32_t now_ms, bool healthy, bool update_path_ok, const char* why_not) {
    flushRecord(now_ms);  // a write that failed earlier
    const bool proven = record_.confirmed && !record_.arrived_by_switch;
    if ((counter_settled_ && !pending_ && proven) || now_ms < cfg::kHealthyAfterMs || !healthy) return;

    if (!counter_settled_) {
        counter_settled_ = true;
        if (jacket::clearCrashes(record_)) record_dirty_ = true;
        logLine("[boot] ran %lus without a crash: crash counter cleared",
                static_cast<unsigned long>(cfg::kHealthyAfterMs / 1000));
    }
    if (!update_path_ok) {
        if (waiting_for_ != why_not) {  // once per reason
            waiting_for_ = why_not;
            if (pending_) {
                logLine("[boot] new firmware NOT CONFIRMED: waiting for %s. Do not power off: a reset now brings the previous firmware back",
                        why_not);
            } else {
                logLine("[boot] firmware not confirmed yet: waiting for %s", why_not);
            }
        }
    } else if (!proven) {
        jacket::confirmImage(record_);
        record_dirty_ = true;
        logLine("[boot] this firmware is confirmed healthy");
    }
    flushRecord(now_ms);
    if (!pending_ || !update_path_ok) return;

    if (confirm_failed_ && static_cast<int32_t>(now_ms - retry_at_ms_) < 0) return;
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        pending_ = false;
        logLine("[boot] new firmware confirmed");
    } else {
        // Still pending, and still protected by the rollback: try again.
        if (!confirm_failed_) logLine("[boot] new firmware NOT CONFIRMED: storing the confirmation FAILED (error %d), retrying", static_cast<int>(err));
        confirm_failed_ = true;
        retry_at_ms_ = now_ms + cfg::kSaveRetryMs;
    }
}

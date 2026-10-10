// Boot health: notices a firmware or scene that keeps crashing, confirms a
// freshly installed firmware only once it has proven itself, and knows the way
// back to the firmware in the other slot.
//
// Crash guard. Each boot caused by a panic, a watchdog or a brown-out adds one
// to a counter in flash; running for cfg::kHealthyAfterMs clears it. At
// cfg::kSafeModeCrashes in a row:
//   - an image that was never confirmed (below) gives way to the other slot,
//     if that holds a firmware that verifies: the boot partition is switched
//     and the chip restarts, before LEDs, Bluetooth or WiFi are touched;
//   - otherwise the jacket starts in safe mode: the stored scene is not
//     restored (the default one plays instead), the microphone stays off and,
//     if the build has WiFi credentials, WiFi and the upload listener run
//     whatever dev mode says. The stored scene is untouched, so the next clean
//     boot tries it again.
// The decision, and why it cannot make two images bounce to each other for
// ever, is jacket::decideBoot (lib/core, jacket/boot_policy.h), tested on the
// host. What is remembered is one record in flash under the key "boot".
//
// Leaving for the other slot is always the same, whoever asks for it (the
// crash guard, the button, BOOT_OTHER_SLOT):
//   1. read and verify the whole image in the other slot; stop if it is bad
//   2. store the record for the image that takes over, and read it back
//   3. select the other slot
//   4. quiet the radios
//   5. reset the whole chip
// Up to 3 a failure leaves everything as it was. Nothing is erased: both
// firmwares keep their settings (rehearsed with WLED 0.14.3: it is not
// bothered by this firmware's entries in the shared settings partition, and
// this firmware finds its own again after WLED has run).
//
// Step 5 is not esp_restart(). That resets the two CPUs and nothing else, and
// what ESP-IDF 5 has set up outside them stays: rehearsed, WLED (ESP-IDF 3.3)
// aborted five seconds into every start when this firmware handed over
// before it had ever started its radio, and ran normally after a reset by the
// EN pin. So the other firmware is started the way power-on would start it:
// by the RTC watchdog with its "reset the system and the RTC" action
// (RTCWDT_RTC_RESET). That reads the boot-mode pin again, hence the button's
// "switch on release". A firmware of ours that starts after it sees a
// watchdog reset; the record says it was coming (jacket::BootRecord::expect_reset).
// Restarts that stay in this firmware are ordinary ones.
//
// Confirmation. An image is confirmed after cfg::kHealthyAfterMs of healthy
// running, and on a build that takes uploads over WiFi only once that path
// works again, so a firmware that cannot be replaced over the air does not
// count as proven. This is kept in the record, per image, and works with any
// bootloader. Where the bootloader supports rollback (the devkit's does, the
// one WLED boards ship with does not) it starts a newly uploaded image
// "pending" as well, and brings the previous firmware back by itself if the
// chip resets before the same moment: Arduino would confirm at once, so this
// file overrides verifyRollbackLater() and poll() tells the bootloader. Until
// then no further upload is accepted (see dev.h): it would overwrite the
// firmware the bootloader falls back to.
//
// Nothing here assumes a partition layout or a bootloader: slots are whatever
// the partition table on the chip says, and an image in the other slot is
// read and verified before anything is switched.
#pragma once

#include <esp_partition.h>
#include <jacket.h>

class BootGuard {
public:
    // Once at boot, after the storage is open and before anything else is
    // started. Does not return if it switches to the other slot.
    void begin(jacket::Storage& storage);

    // The [boot] lines about the two slots: where this image runs, and what
    // the other slot holds.
    void logSlots() const;

    bool safeMode() const { return safe_mode_; }
    uint8_t crashCount() const { return record_.crashes; }
    const char* resetReason() const { return reason_; }
    bool imagePending() const { return pending_; }           // the bootloader still waits for this image to be confirmed
    bool imageConfirmed() const { return record_.confirmed; }  // this image has proven itself, now or at an earlier boot
    const char* slot() const { return slot_; }               // the app partition this image runs from

    // The other slot looks bootable (header and state; not yet read in full).
    bool otherSlotUsable() const { return other_usable_; }

    // Resets the chip into the firmware in the other slot, as described
    // above. Returns (false) only if that could not be done: then nothing was
    // changed, and the log says why. `before_leaving` is called once the other
    // slot is selected and nothing can go wrong any more: quiet the radios,
    // show what there is to show.
    bool switchToOtherSlot(const char* why, void (*before_leaving)() = nullptr);

    // Every loop pass in which flash may be written. `healthy`: everything
    // that must run is running. `update_path_ok`: if this build is meant to be
    // reachable for uploads right now, it is. An image is confirmed only
    // when both hold, so a firmware that cannot be replaced over the air never
    // becomes permanent. `why_not` says, for the log, what is being waited for.
    void poll(uint32_t now_ms, bool healthy, bool update_path_ok, const char* why_not = "the upload path");

private:
    void inspectOther();
    bool store();
    void flushRecord(uint32_t now_ms);

    jacket::Storage* storage_ = nullptr;
    const char* reason_ = "?";
    const char* slot_ = "?";
    const esp_partition_t* running_ = nullptr;
    const esp_partition_t* other_ = nullptr;
    bool other_usable_ = false;
    char other_note_[96] = "none";  // what the other slot holds, for the log
    uint8_t image_[jacket::kImageIdSize] = {};
    uint8_t other_image_[jacket::kImageIdSize] = {};
    jacket::BootRecord record_;
    bool record_dirty_ = false;     // the record in flash is behind record_
    bool store_failed_ = false;
    uint32_t store_retry_ms_ = 0;
    bool safe_mode_ = false;
    bool pending_ = false;
    bool counter_settled_ = false;
    const char* waiting_for_ = nullptr;  // the reason last logged for not confirming
    bool confirm_failed_ = false;
    uint32_t retry_at_ms_ = 0;
};

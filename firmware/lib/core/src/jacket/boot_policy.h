// What to do at boot when the firmware keeps crashing, and the recovery
// button. Pure decisions: the device code (src/boot_guard, src/main) supplies
// the facts and carries the answer out, so every case can be tested on the host.
//
// The flash holds two firmware images ("slots"). One runs; the other is
// whatever ran before the last update: an earlier version of this firmware,
// or the firmware the board was sold with. Two things can go wrong with a
// freshly installed image and neither may strand a board that has no serial
// port:
//
//   it crashes at every start    -> decideBoot(): after `crash_threshold`
//                                   abnormal resets in a row, an image that
//                                   never proved itself gives way to the other
//                                   slot; one that did prove itself starts in
//                                   safe mode instead, as it always has
//   it runs but cannot be        -> RecoveryHold: the board's button, held
//   reached for an update           shortly after power-on, boots the other slot
//
// Why this cannot loop
// --------------------
// An automatic switch needs an image that was never confirmed healthy and
// that did not itself arrive by a switch (`arrived_by_switch`). Before
// switching, the record is rewritten to name the image being switched to,
// with that mark set. So the image that takes over can never switch back by
// itself: if it crashes too, it ends in safe mode and stays there. The mark is
// cleared in two ways only: the image is confirmed healthy (and a confirmed
// image never switches), or a different image appears in the running slot,
// which takes someone uploading one. Hence at most one automatic switch per
// installed image, and the worst case is `crash_threshold` crashes of the new
// image, one switch, `crash_threshold` crashes of the old one, safe mode.
// The device code adds one condition: no switch unless the record was stored
// and read back, so a flash that does not keep the record cannot cause one.
//
// The other image may know nothing of this record (the firmware the board was
// sold with). Then it simply runs; the record waits until this firmware is
// installed again, and that installation is a new image with a clean slate.
//
// The reset that hands over
// -------------------------
// The other firmware is started by a reset of the whole chip, not by the
// restart a firmware normally gives itself (see src/boot_guard.h for why).
// To a firmware that starts after it, that reset looks like a watchdog's.
// The record says it is coming (`expect_reset`), so that this firmware, should
// it be the one that starts, does not count it as a crash.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jacket {

constexpr size_t kImageIdSize = 8;  // the first bytes of an image's SHA-256

// What is kept in flash between boots (storage key "boot").
struct BootRecord {
    uint8_t image[kImageIdSize] = {};  // the image the rest is about
    uint8_t crashes = 0;               // abnormal resets in a row
    bool confirmed = false;            // it has been confirmed healthy at least once
    bool arrived_by_switch = false;    // it was booted by a switch away from the other slot, and not confirmed since
    bool expect_reset = false;         // the next start follows a reset made on purpose: not a crash, whatever it looks like
};

constexpr size_t kBootRecordSize = 4 + kImageIdSize;
size_t serializeBootRecord(const BootRecord& record, uint8_t* out, size_t cap);  // 0 if `cap` is too small
bool deserializeBootRecord(const uint8_t* data, size_t len, BootRecord& out);

enum class BootAction : uint8_t {
    Normal,
    SafeMode,     // too many crashes: start without the stored scene, keep the upload path open
    SwitchSlot,   // too many crashes of an image that never worked: boot the other slot
};

struct BootFacts {
    const uint8_t* image = nullptr;   // identity of the running image, kImageIdSize bytes
    bool crashed = false;             // this boot follows a panic, a watchdog or a brown-out
    bool other_usable = false;        // the other slot holds an image that may be booted
    uint8_t crash_threshold = 3;
};

struct BootDecision {
    BootAction action = BootAction::Normal;
    BootRecord record;        // as it is now; store it if `changed`
    bool changed = false;
    bool new_image = false;   // the running image is not the one the stored record was about
    bool handed_over = false; // this start follows the reset the record announced: `crashed` was not counted
};

// `stored` is the record read from flash, or nullptr if there is none (or it
// could not be read). A SwitchSlot that turns out to be impossible must be
// treated as SafeMode.
BootDecision decideBoot(const BootRecord* stored, const BootFacts& facts);

// The record to store before starting the other slot, whose image is `other`.
BootRecord recordForSwitch(const uint8_t* other);

// The running image proved itself. Returns true if the record changed.
bool confirmImage(BootRecord& record);
// The running image ran long enough without crashing. Returns true if the record changed.
bool clearCrashes(BootRecord& record);

// ---- Recovery button -----------------------------------------------------------
//
// "Shortly after power-on, press the button and keep it down for `hold_ms`;
// let go when the jacket acknowledges."
//
// Not "hold it while plugging in": on most ESP32 boards the one button is
// wired to GPIO0, and GPIO0 held low at power-on puts the chip in its ROM's
// serial download mode, where no firmware runs at all. So a press counts if
// it begins within `window_ms` of the start (or is already there at the first
// sample, for a button on another pin).
//
// The switch happens on release, not at the end of the hold. A button that
// never comes up again within `release_ms` is a stuck pin or a wrong pin
// number, and must not switch firmware at every boot; and the reset that
// starts the other firmware reads GPIO0 again, so the button must be up by then.
class RecoveryHold {
public:
    enum class State : uint8_t {
        Watching,  // inside the window, nothing pressed
        Holding,   // pressed, not long enough yet
        Armed,     // held long enough: acknowledge it, wait for the release
        Fire,      // released: switch now. Reported by exactly one sample.
        Done,      // the window passed, the press was too short and the window is over, it fired, or it never let go
    };

    RecoveryHold(uint32_t window_ms, uint32_t hold_ms, uint32_t release_ms)
        : window_ms_(window_ms), hold_ms_(hold_ms), release_ms_(release_ms) {}

    // One reading of the button at `since_boot_ms`. Sample at least a few
    // times a second; times must not go backwards.
    State sample(bool pressed, uint32_t since_boot_ms);

    State state() const { return state_; }

private:
    uint32_t window_ms_;
    uint32_t hold_ms_;
    uint32_t release_ms_;
    State state_ = State::Watching;
    uint32_t since_ms_ = 0;  // when the press began (Holding) or was long enough (Armed)
};

}  // namespace jacket

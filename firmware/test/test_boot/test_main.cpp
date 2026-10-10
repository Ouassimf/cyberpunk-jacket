// What the jacket does at boot when its firmware keeps crashing, and the
// recovery button (jacket/boot_policy.h). The device code only carries these
// decisions out, so the cases that cannot be staged on a bench are played
// through here: two slots, a flash record, and boots that crash or do not.

#include <string.h>

#include "../support/test_support.h"

using namespace jacket;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint8_t kThreshold = 3;

struct Id {
    uint8_t bytes[kImageIdSize];
};
constexpr Id kImageA{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7}};
constexpr Id kImageB{{0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7}};
constexpr Id kImageC{{0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7}};

BootFacts facts(const Id& image, bool crashed, bool other_usable) {
    BootFacts f;
    f.image = image.bytes;
    f.crashed = crashed;
    f.other_usable = other_usable;
    f.crash_threshold = kThreshold;
    return f;
}

bool sameImage(const BootRecord& record, const Id& image) { return memcmp(record.image, image.bytes, kImageIdSize) == 0; }

// ---- The record -----------------------------------------------------------------------

void test_record_round_trip() {
    BootRecord record;
    memcpy(record.image, kImageA.bytes, kImageIdSize);
    record.crashes = 7;
    record.confirmed = true;
    record.arrived_by_switch = true;
    record.expect_reset = true;

    uint8_t blob[kBootRecordSize];
    TEST_ASSERT_EQUAL(kBootRecordSize, serializeBootRecord(record, blob, sizeof blob));
    TEST_ASSERT_EQUAL_MESSAGE(0, serializeBootRecord(record, blob, sizeof blob - 1), "a buffer that is too small");

    BootRecord back;
    TEST_ASSERT_TRUE(deserializeBootRecord(blob, sizeof blob, back));
    TEST_ASSERT_TRUE(sameImage(back, kImageA));
    TEST_ASSERT_EQUAL(7, back.crashes);
    TEST_ASSERT_TRUE(back.confirmed);
    TEST_ASSERT_TRUE(back.arrived_by_switch);
    TEST_ASSERT_TRUE(back.expect_reset);

    // Each flag is its own bit.
    for (int flag = 0; flag < 3; ++flag) {
        BootRecord one;
        one.confirmed = flag == 0;
        one.arrived_by_switch = flag == 1;
        one.expect_reset = flag == 2;
        TEST_ASSERT_EQUAL(kBootRecordSize, serializeBootRecord(one, blob, sizeof blob));
        TEST_ASSERT_TRUE(deserializeBootRecord(blob, sizeof blob, back));
        TEST_ASSERT_EQUAL(flag == 0, back.confirmed);
        TEST_ASSERT_EQUAL(flag == 1, back.arrived_by_switch);
        TEST_ASSERT_EQUAL(flag == 2, back.expect_reset);
    }
    serializeBootRecord(record, blob, sizeof blob);

    // Anything that is not exactly a record of this version is "no record".
    TEST_ASSERT_FALSE(deserializeBootRecord(blob, sizeof blob - 1, back));
    TEST_ASSERT_FALSE(deserializeBootRecord(nullptr, sizeof blob, back));
    blob[0] ^= 0xFF;
    TEST_ASSERT_FALSE_MESSAGE(deserializeBootRecord(blob, sizeof blob, back), "another version");
    const uint8_t legacy_counter[1] = {2};  // what firmware 2.0.0 kept under its own key
    TEST_ASSERT_FALSE(deserializeBootRecord(legacy_counter, sizeof legacy_counter, back));
}

// ---- One boot ------------------------------------------------------------------------

void test_first_boot_of_an_image_starts_clean() {
    // No record at all: a board that never ran this firmware.
    BootDecision d = decideBoot(nullptr, facts(kImageA, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(d.action));
    TEST_ASSERT_TRUE(d.new_image);
    TEST_ASSERT_TRUE_MESSAGE(d.changed, "the record is written on the first boot");
    TEST_ASSERT_TRUE(sameImage(d.record, kImageA));
    TEST_ASSERT_EQUAL(0, d.record.crashes);
    TEST_ASSERT_FALSE(d.record.confirmed);
    TEST_ASSERT_FALSE(d.record.arrived_by_switch);

    // The same image again, cleanly: nothing to write.
    const BootRecord stored = d.record;
    d = decideBoot(&stored, facts(kImageA, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(d.action));
    TEST_ASSERT_FALSE(d.new_image);
    TEST_ASSERT_FALSE_MESSAGE(d.changed, "a clean boot of a known image writes nothing to flash");
}

void test_a_new_image_does_not_inherit_the_old_ones_crashes() {
    // The previous firmware was in safe mode with five crashes when a fixed
    // one was uploaded. The fixed one must not start in safe mode, and above
    // all must not be thrown out at once for crashes it did not have.
    BootRecord old;
    memcpy(old.image, kImageA.bytes, kImageIdSize);
    old.crashes = 5;
    old.confirmed = true;

    const BootDecision d = decideBoot(&old, facts(kImageB, false, true));
    TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(d.action));
    TEST_ASSERT_TRUE(d.new_image);
    TEST_ASSERT_TRUE(sameImage(d.record, kImageB));
    TEST_ASSERT_EQUAL(0, d.record.crashes);
    TEST_ASSERT_FALSE_MESSAGE(d.record.confirmed, "nor its good name");

    // If its very first boot seen is already a crash, that one counts.
    const BootDecision crashed = decideBoot(&old, facts(kImageB, true, true));
    TEST_ASSERT_EQUAL(1, crashed.record.crashes);
    TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(crashed.action));
}

void test_unconfirmed_image_gives_way_at_the_threshold() {
    BootRecord record = decideBoot(nullptr, facts(kImageA, false, true)).record;
    for (uint8_t crash = 1; crash <= kThreshold; ++crash) {
        const BootRecord stored = record;
        const BootDecision d = decideBoot(&stored, facts(kImageA, true, true));
        TEST_ASSERT_EQUAL(crash, d.record.crashes);
        TEST_ASSERT_TRUE_MESSAGE(d.changed, "every crash is written down");
        const BootAction want = crash < kThreshold ? BootAction::Normal : BootAction::SwitchSlot;
        TEST_ASSERT_EQUAL(static_cast<int>(want), static_cast<int>(d.action));
        record = d.record;
    }
}

void test_no_switch_without_a_usable_other_slot() {
    BootRecord record = decideBoot(nullptr, facts(kImageA, false, false)).record;
    BootDecision d;
    for (uint8_t crash = 1; crash <= kThreshold + 2; ++crash) {
        const BootRecord stored = record;
        d = decideBoot(&stored, facts(kImageA, true, false));
        record = d.record;
        const BootAction want = crash < kThreshold ? BootAction::Normal : BootAction::SafeMode;
        TEST_ASSERT_EQUAL(static_cast<int>(want), static_cast<int>(d.action));
    }
    TEST_ASSERT_EQUAL(kThreshold + 2, record.crashes);
}

void test_confirmed_image_keeps_safe_mode() {
    BootRecord record = decideBoot(nullptr, facts(kImageA, false, true)).record;
    TEST_ASSERT_TRUE(confirmImage(record));
    TEST_ASSERT_FALSE_MESSAGE(confirmImage(record), "confirming twice changes nothing");

    for (uint8_t crash = 1; crash <= kThreshold + 3; ++crash) {
        const BootRecord stored = record;
        const BootDecision d = decideBoot(&stored, facts(kImageA, true, true));
        record = d.record;
        const BootAction want = crash < kThreshold ? BootAction::Normal : BootAction::SafeMode;
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(want), static_cast<int>(d.action),
                                  "an image that worked once is never abandoned, however often it crashes");
    }
    // A healthy run clears the counter, and safe mode with it.
    TEST_ASSERT_TRUE(clearCrashes(record));
    TEST_ASSERT_FALSE(clearCrashes(record));
    const BootRecord stored = record;
    TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(decideBoot(&stored, facts(kImageA, false, true)).action));
}

void test_healthy_runs_between_crashes_never_reach_the_threshold() {
    // Crashes that are not in a row: each healthy run clears the counter.
    BootRecord record = decideBoot(nullptr, facts(kImageA, false, true)).record;
    for (int round = 0; round < 10; ++round) {
        const BootRecord stored = record;
        const BootDecision d = decideBoot(&stored, facts(kImageA, true, true));
        TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(d.action));
        record = d.record;
        clearCrashes(record);  // it then ran 20 s
    }
}

void test_a_threshold_of_zero_never_acts() {
    BootFacts f = facts(kImageA, true, true);
    f.crash_threshold = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(decideBoot(nullptr, f).action));
}

// ---- Two slots, played through ----------------------------------------------------------
//
// A flash with two slots and one record, and boots that crash or not, the way
// the device code drives the policy: decide; on SwitchSlot store
// recordForSwitch(), start the other slot with a reset of the whole chip
// (which the firmware that starts sees as a watchdog reset); otherwise store
// the record and (if the image is good) confirm it after a while.

struct Flash {
    Flash(Id slot0, Id slot1, bool crashes0, bool crashes1, bool ours0, bool ours1)
        : slot{slot0, slot1}, crashes{crashes0, crashes1}, knows_record{ours0, ours1} {}

    Id slot[2];
    bool crashes[2];         // the image in this slot crashes at every start
    bool knows_record[2];    // false: a foreign firmware (WLED), which neither reads nor writes the record
    bool usable[2] = {true, true};
    int running = 0;
    bool have_record = false;
    BootRecord record = BootRecord();

    int switches = 0;
    int boots = 0;
    bool safe_mode = false;
    bool write_fails = false;  // the record cannot be stored

    // One boot. `crashed`: the reset before it looks like a crash. Returns
    // whether this run will end in one.
    bool boot(bool crashed) {
        ++boots;
        safe_mode = false;
        if (!knows_record[running]) return crashes[running];  // somebody else's firmware: it just runs

        const int other = 1 - running;
        const BootDecision d = decideBoot(have_record ? &record : nullptr, facts(slot[running], crashed, usable[other]));
        if (d.action == BootAction::SwitchSlot && !write_fails) {
            record = recordForSwitch(slot[other].bytes);
            have_record = true;
            running = other;
            ++switches;
            return boot(true);  // the hand-over reset looks like a watchdog's
        }
        if (d.changed && !write_fails) {
            record = d.record;
            have_record = true;
        }
        safe_mode = d.action != BootAction::Normal;
        // Safe mode does not cure a firmware that crashes at every start.
        if (crashes[running]) return true;
        BootRecord now = have_record ? record : d.record;
        const bool cleared = clearCrashes(now);
        const bool confirmed = confirmImage(now);
        if ((cleared || confirmed) && !write_fails) {
            record = now;
            have_record = true;
        }
        return false;
    }

    // Boots until the board stays up, or `limit` boots have passed.
    void run(int limit = 50) {
        bool crashed = false;
        for (int i = 0; i < limit; ++i) {
            crashed = boot(crashed);
            if (!crashed) return;
        }
    }
};

void test_bad_first_install_returns_to_the_original_firmware() {
    // The board runs the firmware it was sold with in slot 0. Ours is
    // installed into slot 1 through that firmware's update page, and crashes.
    Flash flash(kImageA, kImageB, false, true, false, true);
    flash.running = 1;
    flash.run();
    TEST_ASSERT_EQUAL_MESSAGE(0, flash.running, "back in the original firmware");
    TEST_ASSERT_EQUAL(1, flash.switches);
    TEST_ASSERT_EQUAL_MESSAGE(kThreshold + 2, flash.boots, "three crashing boots, the boot that switches, the original firmware");

    // The original firmware knows nothing of the record and leaves it alone.
    // A fixed image installed from it is a new image with a clean slate ...
    flash.slot[1] = kImageC;
    flash.crashes[1] = false;
    flash.running = 1;
    flash.boots = 0;
    flash.run();
    TEST_ASSERT_EQUAL(1, flash.running);
    TEST_ASSERT_EQUAL(1, flash.boots);
    TEST_ASSERT_TRUE(flash.record.confirmed);
    TEST_ASSERT_FALSE(flash.record.arrived_by_switch);

    // ... and so is the same bad image installed a second time: it gets its
    // three crashes again, and goes again.
    flash.slot[1] = kImageB;
    flash.crashes[1] = true;
    flash.running = 1;
    flash.switches = 0;
    flash.run();
    TEST_ASSERT_EQUAL(0, flash.running);
    TEST_ASSERT_EQUAL(1, flash.switches);
}

void test_two_crashing_images_do_not_bounce() {
    // Both slots hold a firmware of ours and both crash at every start (a
    // fault in the hardware, say). The new one gives way to the old one once;
    // the old one then stays, in safe mode, however often it crashes.
    Flash flash(kImageA, kImageB, true, true, true, true);
    flash.running = 1;
    flash.run(200);
    TEST_ASSERT_EQUAL_MESSAGE(1, flash.switches, "exactly one switch in 200 boots");
    TEST_ASSERT_EQUAL(0, flash.running);
    TEST_ASSERT_TRUE(flash.safe_mode);
    TEST_ASSERT_TRUE(flash.record.arrived_by_switch);
    TEST_ASSERT_FALSE(flash.record.confirmed);
}

void test_switch_back_then_healthy_is_confirmed_and_settles() {
    // The new image crashes, the old one is fine: one switch, then normal
    // running, and the old image counts as proven again.
    Flash flash(kImageA, kImageB, false, true, true, true);
    flash.running = 1;
    flash.run();
    TEST_ASSERT_EQUAL(0, flash.running);
    TEST_ASSERT_EQUAL(1, flash.switches);
    TEST_ASSERT_FALSE(flash.safe_mode);
    TEST_ASSERT_TRUE(flash.record.confirmed);
    TEST_ASSERT_FALSE(flash.record.arrived_by_switch);

    // Later it starts crashing (a bad stored scene): safe mode, no switch to
    // the image it fled from.
    flash.crashes[0] = true;
    flash.run(20);
    TEST_ASSERT_EQUAL(0, flash.running);
    TEST_ASSERT_EQUAL(1, flash.switches);
    TEST_ASSERT_TRUE(flash.safe_mode);
}

void test_every_combination_is_bounded() {
    // Whatever the two images do and whoever wrote them: never more than one
    // automatic switch without somebody installing something.
    for (int mask = 0; mask < 64; ++mask) {
        Flash flash(kImageA, kImageB, (mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0);
        flash.usable[0] = (mask & 16) != 0;
        flash.usable[1] = (mask & 32) != 0;
        flash.running = 1;
        flash.run(300);
        char note[48];
        snprintf(note, sizeof note, "combination %d", mask);
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(1, flash.switches, note);
        if (!flash.usable[0]) TEST_ASSERT_EQUAL_MESSAGE(0, flash.switches, "never into a slot that is not usable");
    }
}

void test_the_handover_reset_is_not_a_crash() {
    // The other firmware is started by a reset of the whole chip, which looks
    // like a watchdog reset to whatever starts. If that is this firmware, the
    // record told it so: the reset is not counted, once.
    const BootRecord handed = recordForSwitch(kImageA.bytes);
    TEST_ASSERT_TRUE(handed.expect_reset);
    BootDecision d = decideBoot(&handed, facts(kImageA, true, true));
    TEST_ASSERT_TRUE(d.handed_over);
    TEST_ASSERT_EQUAL_MESSAGE(0, d.record.crashes, "not a crash");
    TEST_ASSERT_FALSE_MESSAGE(d.record.expect_reset, "good for one start");
    TEST_ASSERT_TRUE_MESSAGE(d.changed, "and that is written down");
    TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(d.action));
    TEST_ASSERT_TRUE(d.record.arrived_by_switch);

    // The next reset that looks like a crash is one.
    const BootRecord after = d.record;
    d = decideBoot(&after, facts(kImageA, true, true));
    TEST_ASSERT_FALSE(d.handed_over);
    TEST_ASSERT_EQUAL(1, d.record.crashes);

    // An image that crashed right up to the threshold is not excused by it:
    // the count it had stays, only the announced reset is left out.
    BootRecord worn = recordForSwitch(kImageA.bytes);
    worn.crashes = kThreshold - 1;
    d = decideBoot(&worn, facts(kImageA, true, true));
    TEST_ASSERT_EQUAL(kThreshold - 1, d.record.crashes);
    TEST_ASSERT_EQUAL(static_cast<int>(BootAction::Normal), static_cast<int>(d.action));

    // If another image starts than the one named (the original firmware
    // stayed a while, then someone installed a new one from it), the
    // announcement is spent on that start and the image begins clean.
    d = decideBoot(&handed, facts(kImageC, true, true));
    TEST_ASSERT_TRUE(d.new_image);
    TEST_ASSERT_EQUAL(0, d.record.crashes);
    TEST_ASSERT_FALSE(d.record.expect_reset);
    TEST_ASSERT_FALSE(d.record.arrived_by_switch);

    // In the two-slot run above the image that takes over is started that way
    // every time; had its first start counted, two crashing images would
    // reach safe mode one crash early. Count the boots: 3 crashes + the
    // switching boot for the new image, then the announced start + 3 crashes
    // before the old one is in safe mode.
    Flash flash(kImageA, kImageB, true, true, true, true);
    flash.running = 1;
    bool crashed = false;
    int boots_until_safe_mode = 0;
    for (int i = 0; i < 20 && !flash.safe_mode; ++i) {
        crashed = flash.boot(crashed);
        boots_until_safe_mode = flash.boots;
    }
    TEST_ASSERT_TRUE(flash.safe_mode);
    TEST_ASSERT_EQUAL(kThreshold + 1 + kThreshold + 1, boots_until_safe_mode);
}

void test_no_switch_when_the_record_cannot_be_stored() {
    // The device code switches only after the record was written and read
    // back. With a flash that takes no writes nothing is ever counted, so the
    // threshold is never reached either.
    Flash flash(kImageA, kImageB, true, true, true, true);
    flash.running = 1;
    flash.write_fails = true;
    flash.run(100);
    TEST_ASSERT_EQUAL(0, flash.switches);
    TEST_ASSERT_EQUAL(1, flash.running);
}

void test_record_for_switch() {
    const BootRecord record = recordForSwitch(kImageA.bytes);
    TEST_ASSERT_TRUE(sameImage(record, kImageA));
    TEST_ASSERT_EQUAL(0, record.crashes);
    TEST_ASSERT_FALSE(record.confirmed);
    TEST_ASSERT_TRUE(record.arrived_by_switch);

    // The image it names boots as a known image, and may not switch back.
    BootRecord stored = record;
    stored.expect_reset = false;  // (the announced reset is another test's business)
    for (uint8_t crash = 1; crash <= kThreshold + 1; ++crash) {
        const BootRecord before = stored;
        const BootDecision d = decideBoot(&before, facts(kImageA, true, true));
        TEST_ASSERT_FALSE(d.new_image);
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(BootAction::SwitchSlot), static_cast<int>(d.action));
        stored = d.record;
    }
    // Confirmed, the mark is gone, and so is any chance of a switch.
    TEST_ASSERT_TRUE(confirmImage(stored));
    TEST_ASSERT_FALSE(stored.arrived_by_switch);
}

// ---- Recovery button -------------------------------------------------------------------

using State = RecoveryHold::State;

constexpr uint32_t kWindow = 10000;
constexpr uint32_t kHold = 3000;
constexpr uint32_t kRelease = 10000;

// Samples every 50 ms from `from` to `to` (inclusive) and returns the last state.
State feed(RecoveryHold& hold, bool pressed, uint32_t from, uint32_t to) {
    State state = hold.state();
    for (uint32_t t = from; t <= to; t += 50) state = hold.sample(pressed, t);
    return state;
}

void test_button_hold_then_release_fires_once() {
    RecoveryHold hold(kWindow, kHold, kRelease);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Watching), static_cast<int>(feed(hold, false, 300, 2000)));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Holding), static_cast<int>(hold.sample(true, 2050)));
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(State::Holding), static_cast<int>(feed(hold, true, 2100, 5000)), "not yet 3 s");
    TEST_ASSERT_EQUAL(static_cast<int>(State::Armed), static_cast<int>(hold.sample(true, 5050)));
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(State::Armed), static_cast<int>(feed(hold, true, 5100, 7000)),
                              "nothing happens while it is still down");
    TEST_ASSERT_EQUAL(static_cast<int>(State::Fire), static_cast<int>(hold.sample(false, 7050)));
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(State::Done), static_cast<int>(hold.sample(false, 7100)), "exactly one sample fires");
    TEST_ASSERT_EQUAL(static_cast<int>(State::Done), static_cast<int>(feed(hold, true, 7150, 30000)));
}

void test_button_already_down_at_the_first_sample_counts() {
    // A button on a pin that does not decide the boot mode can be held from
    // before power-on.
    RecoveryHold hold(kWindow, kHold, kRelease);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Holding), static_cast<int>(hold.sample(true, 900)));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Armed), static_cast<int>(feed(hold, true, 950, 3900)));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Fire), static_cast<int>(hold.sample(false, 3950)));
}

void test_button_short_press_does_nothing_and_may_be_repeated() {
    RecoveryHold hold(kWindow, kHold, kRelease);
    feed(hold, true, 1000, 3900);  // 2.9 s
    TEST_ASSERT_EQUAL(static_cast<int>(State::Watching), static_cast<int>(hold.sample(false, 3950)));
    // A second try inside the window, held long enough, even past its end.
    TEST_ASSERT_EQUAL(static_cast<int>(State::Holding), static_cast<int>(hold.sample(true, 9000)));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Armed), static_cast<int>(feed(hold, true, 9050, 12000)));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Fire), static_cast<int>(hold.sample(false, 12050)));
}

void test_button_after_the_window_is_ignored() {
    RecoveryHold hold(kWindow, kHold, kRelease);
    feed(hold, false, 300, kWindow);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Watching), static_cast<int>(hold.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Done), static_cast<int>(hold.sample(true, kWindow + 50)));
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(State::Done), static_cast<int>(feed(hold, true, kWindow + 100, kWindow + 20000)),
                              "an hour into the evening the button does not swap firmware");

    // A short press that ends after the window closed is the end of it too.
    RecoveryHold late(kWindow, kHold, kRelease);
    feed(late, true, 9000, 10500);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Done), static_cast<int>(late.sample(false, 10550)));
}

void test_button_that_never_comes_up_is_not_a_finger() {
    // A stuck pin, a wrong pin number, a serial adapter holding GPIO0 low:
    // armed after 3 s, and given up 10 s later without firing.
    RecoveryHold hold(kWindow, kHold, kRelease);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Armed), static_cast<int>(feed(hold, true, 200, 3300)));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Armed), static_cast<int>(feed(hold, true, 3350, 13100)));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Done), static_cast<int>(feed(hold, true, 13150, 13300)));
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(State::Done), static_cast<int>(feed(hold, false, 13350, 20000)),
                              "letting go later does not fire either");
}

void test_button_never_fires_without_a_full_hold() {
    // Any pattern of presses shorter than the hold, sampled coarsely or finely.
    for (uint32_t step : {10u, 50u, 100u}) {
        RecoveryHold hold(kWindow, kHold, kRelease);
        for (uint32_t t = 0; t <= 30000; t += step) {
            const bool pressed = (t / 100) % 30 < 29;  // 2.9 s down, 0.1 s up, for ever
            TEST_ASSERT_NOT_EQUAL(static_cast<int>(State::Fire), static_cast<int>(hold.sample(pressed, t)));
        }
    }
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_record_round_trip);
    RUN_TEST(test_first_boot_of_an_image_starts_clean);
    RUN_TEST(test_a_new_image_does_not_inherit_the_old_ones_crashes);
    RUN_TEST(test_unconfirmed_image_gives_way_at_the_threshold);
    RUN_TEST(test_no_switch_without_a_usable_other_slot);
    RUN_TEST(test_confirmed_image_keeps_safe_mode);
    RUN_TEST(test_healthy_runs_between_crashes_never_reach_the_threshold);
    RUN_TEST(test_a_threshold_of_zero_never_acts);
    RUN_TEST(test_bad_first_install_returns_to_the_original_firmware);
    RUN_TEST(test_two_crashing_images_do_not_bounce);
    RUN_TEST(test_switch_back_then_healthy_is_confirmed_and_settles);
    RUN_TEST(test_every_combination_is_bounded);
    RUN_TEST(test_the_handover_reset_is_not_a_crash);
    RUN_TEST(test_no_switch_when_the_record_cannot_be_stored);
    RUN_TEST(test_record_for_switch);
    RUN_TEST(test_button_hold_then_release_fires_once);
    RUN_TEST(test_button_already_down_at_the_first_sample_counts);
    RUN_TEST(test_button_short_press_does_nothing_and_may_be_repeated);
    RUN_TEST(test_button_after_the_window_is_ignored);
    RUN_TEST(test_button_that_never_comes_up_is_not_a_finger);
    RUN_TEST(test_button_never_fires_without_a_full_hold);
    return UNITY_END();
}

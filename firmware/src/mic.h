// The board's own microphone: I2S capture, the analyser, and live frames for
// the engine. Compiled only for a board that has one (JACKET_HAS_MIC in
// config.h); on any other board every call here is an empty inline.
//
// A task of its own reads the microphone at 16 kHz, feeds jacket::MicLive
// (lib/core, jacket/audio) and about 30 times a second leaves a LiveInput in
// one slot, the newest wins, stamped with the time its last sample was
// captured; the loop takes it with take() and gives it to
// Engine::handleLocalLive(). Nothing here touches the engine: it is not
// thread-safe, and this is the same path Bluetooth live frames take (ble.h).
//
// The task
// --------
// Pinned to core 0, priority 2. The render loop is the Arduino loop task on
// core 1, and the LED signal is produced there, so nothing this task does can
// take time from a frame. On core 0 it runs below the Bluetooth and WiFi
// tasks, which preempt it whenever they need to, and above idle; it blocks in
// every read, so idle (and the task watchdog that checks it) still runs.
// One analysis frame per 10 ms is cheap, but four times a second the analyser
// also estimates the tempo, which takes several frames' worth of time in one
// call: the driver's buffers hold 120 ms of sound for that, and a count of
// what was lost anyway is in the heartbeat (ovf).
//
// Only while needed
// -----------------
// setWanted(false) stops the I2S clock (the microphone powers down without
// it), frees the driver and its buffers and lets the task sleep. The loop
// asks for the microphone only while the jacket is on and a segment plays an
// effect that reacts to sound, never in safe mode and never during a firmware
// upload.
//
// The samples
// -----------
// An I2S MEMS microphone answers in one half (slot) of the stereo frame, 24
// bits, left-justified in 32. Which half depends on how one of its pins is
// strapped, and nothing reports that, so both slots are read and the log
// shows the peak of each. If the slot in use (cfg::kMicRightSlot to begin
// with) stays dead silent for two seconds while the other one carries
// signal, the task changes over by itself and says so: a living microphone
// is never that silent, it has noise of its own. The slot in use is high-passed at
// 20 Hz (these microphones have a DC offset, and the analyser's window would
// smear it over the bass) and reduced to 16 bits (cfg::kMicDropBits), clipped
// rather than wrapped. The first 300 ms after the clock starts are thrown
// away: the microphone is still waking up.
#pragma once

#include <jacket.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"

class Mic {
public:
#if JACKET_HAS_MIC
    // Creates the task, asleep. False if that failed: the jacket runs without
    // a microphone. `wake_task` is notified whenever a frame is ready.
    bool begin(TaskHandle_t wake_task);

    // Any task. Capture starts or stops within a few tens of milliseconds.
    void setWanted(bool wanted);
    bool wanted() const { return wanted_; }
    bool running() const { return running_; }

    // Waits until capture has stopped (at most `timeout_ms`), after setWanted(false).
    void waitStopped(uint32_t timeout_ms);

    // The newest frame since the last call and the local time (millis) of its
    // last sample. Loop task.
    bool take(jacket::LiveInput& out, uint32_t& at_ms);

    // For the heartbeat, covering the time since the previous call:
    //   "mic=off"
    //   "mic=on load=7.4% worst=21300us ovf=0 stack=3900 slot=L pk=812/3 fr=150"
    // load: share of one core spent on conversion and analysis; worst: the
    // longest single processing call; ovf: driver buffers overwritten before
    // they were read (10 ms of sound each), since boot; stack: the task's free
    // stack at its lowest; slot: the half of the I2S frame in use, L or R;
    // pk: the largest sample in each slot after the high-pass, as 16-bit
    // values before clipping, the slot in use first; fr: frames made.
    void describe(char* out, size_t cap, uint32_t window_ms);

private:
    static void taskEntry(void* self);
    void run();
    bool startCapture();
    void stopCapture();

    volatile bool wanted_ = false;
    volatile bool running_ = false;
#else
    bool begin(TaskHandle_t) { return true; }
    void setWanted(bool) {}
    bool wanted() const { return false; }
    bool running() const { return false; }
    void waitStopped(uint32_t) {}
    bool take(jacket::LiveInput&, uint32_t&) { return false; }
    void describe(char* out, size_t cap, uint32_t) {
        if (cap != 0) out[0] = '\0';
    }
#endif
};

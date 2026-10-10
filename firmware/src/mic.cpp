#include "mic.h"

#if JACKET_HAS_MIC

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_timer.h>
#include <jacket/audio/mic_live.h>
#include <stdio.h>

#include "log.h"

namespace {

constexpr uint32_t kSampleRate = jacket::AudioAnalyzer::kSampleRate;
constexpr size_t kBlock = jacket::AudioAnalyzer::kHop;  // samples per read: one analysis frame, 10 ms
constexpr int64_t kBlockUs = 1000000LL * kBlock / kSampleRate;
constexpr uint32_t kDmaBuffers = 12;       // of one block each: 120 ms may pile up while the tempo is estimated
constexpr uint32_t kBlocksPerFrame = 3;    // a live frame every 30 ms
constexpr uint32_t kSettleBlocks = 30;     // 300 ms after the clock starts are not sound yet
constexpr uint32_t kReadTimeoutMs = 200;
constexpr uint8_t kReadFailures = 5;       // in a row: restart the driver
constexpr uint32_t kRetryMs = 5000;        // after the driver failed to start
constexpr uint8_t kHighPassShift = 7;      // 16000 / (2 pi 128) = 20 Hz
// The wrong slot: nothing at all in the one in use (a microphone's own noise
// is a few counts) and at least this much in the other, for this many blocks.
constexpr int32_t kDeadPeak = 1;
constexpr int32_t kAlivePeak = 8;
constexpr uint32_t kWrongSlotBlocks = 200;  // 2 s

constexpr uint32_t kTaskStack = 6144;
constexpr UBaseType_t kTaskPriority = 2;
constexpr BaseType_t kTaskCore = 0;

static_assert(cfg::kMicDropBits <= 8, "the samples have 24 bits and 16 are wanted");

jacket::MicLive g_live;      // 13.9 KB: static, as its header asks
int32_t g_raw[kBlock * 2];   // one block as the driver delivers it: left, right, left, ...
int16_t g_pcm[kBlock];

TaskHandle_t g_task = nullptr;
TaskHandle_t g_wake_task = nullptr;
i2s_chan_handle_t g_rx = nullptr;
bool g_start_failure_logged = false;

// The newest frame, written by the microphone task and taken by the loop;
// and what the heartbeat reports, written by the task and reset by the loop.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
jacket::LiveInput g_frame;
uint32_t g_frame_ms = 0;
bool g_frame_fresh = false;
struct Stats {
    uint32_t busy_us = 0;
    uint32_t worst_us = 0;
    uint32_t frames = 0;
    int32_t peak_used = 0;
    int32_t peak_other = 0;
} g_stats;
volatile uint32_t g_overruns = 0;
volatile bool g_right_slot = cfg::kMicRightSlot;  // the slot in use

// A buffer was overwritten before the task read it. Interrupt context.
bool IRAM_ATTR onOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    g_overruns = g_overruns + 1;
    return false;
}

// One-pole high-pass on 24-bit samples.
struct HighPass {
    int32_t x = 0;
    int32_t y = 0;
    int32_t step(int32_t in) {
        y = in - x + y - (y >> kHighPassShift);
        x = in;
        return y;
    }
};

int32_t magnitude(int32_t v) { return v < 0 ? -v : v; }

#if JACKET_TEST_MIC_SYNTH
// The drum machine of JACKET_TEST_MIC_SYNTH: every half second a burst of
// noise that dies away in a tenth of a second, a softer one in between, and a
// faint hiss throughout. Returns one sample as the microphone would, 24 bits.
int32_t synthSample() {
    static uint32_t n = 0;
    static uint32_t noise = 12345;
    static int32_t envelope = 0;  // Q8
    const uint32_t in_beat = n % (kSampleRate / 2);
    if (in_beat == 0) envelope = 12000 << 8;
    if (in_beat == kSampleRate / 4) envelope = 3000 << 8;
    ++n;
    noise = noise * 1664525u + 1013904223u;
    const int32_t white = static_cast<int32_t>(noise >> 16) - 32768;  // -32768..32767
    envelope -= envelope / 400;                                        // 25 ms time constant
    const int32_t sample16 = (white * ((envelope >> 8) + 60)) / 32768;
    return sample16 * (1 << cfg::kMicDropBits);
}
#endif

}  // namespace

bool Mic::begin(TaskHandle_t wake_task) {
    g_wake_task = wake_task;
    if (xTaskCreatePinnedToCore(taskEntry, "mic", kTaskStack, this, kTaskPriority, &g_task, kTaskCore) != pdPASS) {
        g_task = nullptr;
        logLine("[mic] task FAILED to start: no microphone");
        return false;
    }
    return true;
}

void Mic::setWanted(bool wanted) {
    if (g_task == nullptr || wanted == wanted_) return;
    wanted_ = wanted;
    xTaskNotifyGive(g_task);
}

void Mic::waitStopped(uint32_t timeout_ms) {
    const uint32_t start_ms = millis();
    while (running_ && millis() - start_ms < timeout_ms) delay(1);
}

bool Mic::take(jacket::LiveInput& out, uint32_t& at_ms) {
    portENTER_CRITICAL(&g_mux);
    const bool fresh = g_frame_fresh;
    if (fresh) {
        out = g_frame;
        at_ms = g_frame_ms;
    }
    g_frame_fresh = false;
    portEXIT_CRITICAL(&g_mux);
    return fresh;
}

void Mic::describe(char* out, size_t cap, uint32_t window_ms) {
    portENTER_CRITICAL(&g_mux);
    const Stats stats = g_stats;
    g_stats = Stats();
    portEXIT_CRITICAL(&g_mux);
    const uint32_t overruns = g_overruns;  // since boot: one lost buffer must not scroll away unseen
    if (!running_ && stats.frames == 0) {
        snprintf(out, cap, "mic=off ovf=%lu", static_cast<unsigned long>(overruns));
        return;
    }
    const uint32_t load_x10 = window_ms != 0 ? stats.busy_us / window_ms : 0;  // us per ms = tenths of a percent
    snprintf(out, cap, "mic=%s load=%lu.%lu%% worst=%luus ovf=%lu stack=%u slot=%c pk=%ld/%ld fr=%lu", running_ ? "on" : "off",
             static_cast<unsigned long>(load_x10 / 10u), static_cast<unsigned long>(load_x10 % 10u),
             static_cast<unsigned long>(stats.worst_us), static_cast<unsigned long>(overruns),
             g_task != nullptr ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_task)) : 0u, g_right_slot ? 'R' : 'L',
             static_cast<long>(stats.peak_used), static_cast<long>(stats.peak_other), static_cast<unsigned long>(stats.frames));
}

void Mic::taskEntry(void* self) { static_cast<Mic*>(self)->run(); }

bool Mic::startCapture() {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = kDmaBuffers;
    channel.dma_frame_num = kBlock;
    esp_err_t err = i2s_new_channel(&channel, nullptr, &g_rx);
    if (err == ESP_OK) {
        i2s_std_config_t config = {};
        config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
        config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
        config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        config.gpio_cfg.bclk = static_cast<gpio_num_t>(cfg::kPinMicSck);
        config.gpio_cfg.ws = static_cast<gpio_num_t>(cfg::kPinMicWs);
        config.gpio_cfg.dout = I2S_GPIO_UNUSED;
        config.gpio_cfg.din = static_cast<gpio_num_t>(cfg::kPinMicSd);
        err = i2s_channel_init_std_mode(g_rx, &config);
        if (err == ESP_OK) {
            i2s_event_callbacks_t callbacks = {};
            callbacks.on_recv_q_ovf = onOverflow;
            err = i2s_channel_register_event_callback(g_rx, &callbacks, nullptr);
        }
        if (err == ESP_OK) err = i2s_channel_enable(g_rx);
        if (err != ESP_OK) {
            i2s_del_channel(g_rx);
            g_rx = nullptr;
        }
    }
    if (err != ESP_OK) {
        if (!g_start_failure_logged) logLine("[mic] I2S FAILED to start (error 0x%x), retrying", static_cast<unsigned>(err));
        g_start_failure_logged = true;
        return false;
    }
    g_start_failure_logged = false;
    g_live.reset();
    running_ = true;
    logLine("[mic] listening: I2S %lu Hz, data=%d ws=%d clock=%d, %s slot", static_cast<unsigned long>(kSampleRate),
            static_cast<int>(cfg::kPinMicSd), static_cast<int>(cfg::kPinMicWs), static_cast<int>(cfg::kPinMicSck),
            g_right_slot ? "right" : "left");
    return true;
}

void Mic::stopCapture() {
    if (g_rx != nullptr) {
        i2s_channel_disable(g_rx);
        i2s_del_channel(g_rx);
        g_rx = nullptr;
    }
    running_ = false;
    logLine("[mic] stopped");
}

void Mic::run() {
    HighPass filter_used, filter_other;
    uint32_t blocks = 0;        // since capture started
    uint32_t wrong_blocks = 0;  // in a row in which the slot in use looked dead and the other alive
    uint8_t failures = 0;
    int64_t captured_us = 0;  // when the newest sample read so far was captured

    for (;;) {
        if (!wanted_) {
            if (running_) stopCapture();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!running_) {
            if (!startCapture()) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kRetryMs));
                continue;
            }
            filter_used = filter_other = HighPass();
            blocks = 0;
            wrong_blocks = 0;
            failures = 0;
        }
        const size_t used = g_right_slot ? 1 : 0;

        size_t got = 0;
        const int64_t asked_us = esp_timer_get_time();
        const esp_err_t err = i2s_channel_read(g_rx, g_raw, sizeof g_raw, &got, pdMS_TO_TICKS(kReadTimeoutMs));
        const int64_t now_us = esp_timer_get_time();
        if (err != ESP_OK || got != sizeof g_raw) {
            if (++failures >= kReadFailures) {
                logLine("[mic] no samples from the I2S driver (error 0x%x): restarting it", static_cast<unsigned>(err));
                stopCapture();
            }
            continue;
        }
        failures = 0;

        // When was the last of these samples captured? If the read had to
        // wait, just now. If the block was already waiting (the task is
        // catching up after a long analysis), one block after the previous
        // one, but never in the future and never absurdly long ago.
        if (blocks == 0 || now_us - asked_us >= 2000) {
            captured_us = now_us;
        } else {
            captured_us += kBlockUs;
            if (captured_us > now_us || now_us - captured_us > static_cast<int64_t>(kDmaBuffers) * kBlockUs) captured_us = now_us;
        }

        int32_t peak_used = 0;
        int32_t peak_other = 0;
        for (size_t i = 0; i < kBlock; ++i) {
#if JACKET_TEST_MIC_SYNTH
            g_raw[2 * i + used] = synthSample() * 256;
#endif
            const int32_t sample = filter_used.step(g_raw[2 * i + used] >> 8) >> cfg::kMicDropBits;
            const int32_t other = filter_other.step(g_raw[2 * i + (1 - used)] >> 8) >> cfg::kMicDropBits;
            if (magnitude(sample) > peak_used) peak_used = magnitude(sample);
            if (magnitude(other) > peak_other) peak_other = magnitude(other);
            g_pcm[i] = static_cast<int16_t>(sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample);
        }
        ++blocks;
        if (blocks <= kSettleBlocks) continue;  // the filters have settled on the microphone's offset meanwhile

        wrong_blocks = peak_used <= kDeadPeak && peak_other >= kAlivePeak ? wrong_blocks + 1 : 0;
        if (wrong_blocks >= kWrongSlotBlocks) {
            wrong_blocks = 0;
            g_right_slot = !g_right_slot;
            const HighPass swap = filter_used;
            filter_used = filter_other;
            filter_other = swap;
            g_live.reset();
            logLine("[mic] the %s slot is dead silent and the %s one is not: using the %s slot from now on (kMicRightSlot in config.h)",
                    g_right_slot ? "left" : "right", g_right_slot ? "right" : "left", g_right_slot ? "right" : "left");
            continue;
        }

        g_live.process(g_pcm, kBlock);
        jacket::LiveInput frame;
        const bool made = blocks % kBlocksPerFrame == 0 && g_live.snapshot(static_cast<uint32_t>(captured_us / 1000), frame);
        const uint32_t took_us = static_cast<uint32_t>(esp_timer_get_time() - now_us);

        portENTER_CRITICAL(&g_mux);
        if (made) {
            g_frame = frame;
            g_frame_ms = static_cast<uint32_t>(captured_us / 1000);
            g_frame_fresh = true;
            ++g_stats.frames;
        }
        g_stats.busy_us += took_us;
        if (took_us > g_stats.worst_us) g_stats.worst_us = took_us;
        if (peak_used > g_stats.peak_used) g_stats.peak_used = peak_used;
        if (peak_other > g_stats.peak_other) g_stats.peak_other = peak_other;
        portEXIT_CRITICAL(&g_mux);
        if (made && g_wake_task != nullptr) xTaskNotifyGive(g_wake_task);
    }
}

#endif  // JACKET_HAS_MIC

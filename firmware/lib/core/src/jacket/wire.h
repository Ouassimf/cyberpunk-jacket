// Internal: bounds-checked byte cursors shared by the protocol codec and the
// persistence code. Not part of the public API.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "jacket/scene.h"

namespace jacket {
namespace wire {

// Reads never go past `len`. After a failed read `ok()` is false and every
// further read returns zeros.
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return len_ - pos_; }

    uint8_t u8() {
        if (!need(1)) return 0;
        return data_[pos_++];
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = static_cast<uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    Rgb rgb() {
        if (!need(3)) return Rgb();
        const Rgb c(data_[pos_], data_[pos_ + 1], data_[pos_ + 2]);
        pos_ += 3;
        return c;
    }
    // Returns a pointer to `n` bytes inside the input, or nullptr.
    const uint8_t* bytes(size_t n) {
        if (!need(n)) return nullptr;
        const uint8_t* p = data_ + pos_;
        pos_ += n;
        return p;
    }
    EffectConfig config() {
        EffectConfig c;
        if (!need(10)) return c;
        c.effect = u8();
        c.palette = u8();
        c.speed = u8();
        c.intensity = u8();
        c.color1 = rgb();
        c.color2 = rgb();
        return c;
    }
    SegmentGeometry geometry() {
        SegmentGeometry g;
        if (!need(4)) return g;
        g.strip_mask = u8();
        g.start = u8();
        g.length = u8();
        g.flags = u8();
        return g;
    }

private:
    bool need(size_t n) {
        if (!ok_ || len_ - pos_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// Writes never go past `cap`. After an overflow `size()` returns 0.
class Writer {
public:
    Writer(uint8_t* out, size_t cap) : out_(out), cap_(cap) {}

    bool ok() const { return ok_; }
    size_t size() const { return ok_ ? pos_ : 0; }
    size_t position() const { return pos_; }
    bool fits(size_t n) const { return ok_ && cap_ - pos_ >= n; }

    void u8(uint8_t v) {
        if (!need(1)) return;
        out_[pos_++] = v;
    }
    void u16(uint16_t v) {
        if (!need(2)) return;
        out_[pos_++] = static_cast<uint8_t>(v & 0xFF);
        out_[pos_++] = static_cast<uint8_t>(v >> 8);
    }
    void rgb(Rgb c) {
        if (!need(3)) return;
        out_[pos_++] = c.r;
        out_[pos_++] = c.g;
        out_[pos_++] = c.b;
    }
    void bytes(const void* data, size_t n) {
        if (!need(n)) return;
        if (n) memcpy(out_ + pos_, data, n);
        pos_ += n;
    }
    void config(const EffectConfig& c) {
        if (!need(10)) return;
        u8(c.effect);
        u8(c.palette);
        u8(c.speed);
        u8(c.intensity);
        rgb(c.color1);
        rgb(c.color2);
    }
    void geometry(const SegmentGeometry& g) {
        if (!need(4)) return;
        u8(g.strip_mask);
        u8(g.start);
        u8(g.length);
        u8(g.flags);
    }
    // Overwrite one byte already written.
    void patch(size_t at, uint8_t v) {
        if (ok_ && at < pos_) out_[at] = v;
    }
    void fail() { ok_ = false; }

private:
    bool need(size_t n) {
        if (!ok_ || cap_ - pos_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    uint8_t* out_;
    size_t cap_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace wire
}  // namespace jacket

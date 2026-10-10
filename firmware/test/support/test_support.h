// Helpers shared by the host test suites (header only; each suite is its own
// program).
#pragma once

#include <jacket.h>
#include <unity.h>

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace support {

using Bytes = std::vector<uint8_t>;

inline Bytes fromHex(const std::string& hex) {
    Bytes out;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

inline std::string toHex(const uint8_t* data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}
inline std::string toHex(const Bytes& b) { return toHex(b.data(), b.size()); }

// Collects the frames an Engine sends in reply to commands.
class CaptureSink : public jacket::FrameSink {
public:
    void send(const uint8_t* frame, size_t len) override { frames.emplace_back(frame, frame + len); }
    std::vector<Bytes> frames;
};

// A copy of some bytes placed so that the byte after the last one is in a
// page that cannot be read: any read past the end crashes the test program
// instead of going unnoticed.
class GuardedBytes {
public:
    explicit GuardedBytes(const Bytes& bytes) : len_(bytes.size()) {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        page_ = info.dwPageSize;
        base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * page_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old;
        ok_ = base_ != nullptr && VirtualProtect(base_ + page_, page_, PAGE_NOACCESS, &old) != 0;
#else
        page_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        void* p = mmap(nullptr, 2 * page_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base_ = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
        ok_ = base_ != nullptr && mprotect(base_ + page_, page_, PROT_NONE) == 0;
#endif
        if (ok_ && len_ <= page_) {
            for (size_t i = 0; i < len_; ++i) data()[i] = bytes[i];
        } else {
            ok_ = false;
        }
    }
    ~GuardedBytes() {
        if (base_ == nullptr) return;
#ifdef _WIN32
        VirtualFree(base_, 0, MEM_RELEASE);
#else
        munmap(base_, 2 * page_);
#endif
    }
    GuardedBytes(const GuardedBytes&) = delete;
    GuardedBytes& operator=(const GuardedBytes&) = delete;

    bool ok() const { return ok_; }
    uint8_t* data() const { return base_ + page_ - len_; }
    size_t size() const { return len_; }

private:
    uint8_t* base_ = nullptr;
    size_t page_ = 0;
    size_t len_;
    bool ok_ = false;
};

// Two strips' worth of pixels.
struct Picture {
    jacket::Rgb left[jacket::kLedsPerStrip];
    jacket::Rgb right[jacket::kLedsPerStrip];

    bool operator==(const Picture& o) const {
        for (uint16_t i = 0; i < jacket::kLedsPerStrip; ++i) {
            if (left[i] != o.left[i] || right[i] != o.right[i]) return false;
        }
        return true;
    }
    bool operator!=(const Picture& o) const { return !(*this == o); }
    bool allBlack() const {
        for (uint16_t i = 0; i < jacket::kLedsPerStrip; ++i) {
            if (left[i] != jacket::kBlack || right[i] != jacket::kBlack) return false;
        }
        return true;
    }
};

inline int brightnessOf(jacket::Rgb c) { return c.r + c.g + c.b; }

#define TEST_ASSERT_RGB(expected, actual)                                         \
    do {                                                                          \
        const jacket::Rgb e_ = (expected);                                        \
        const jacket::Rgb a_ = (actual);                                          \
        char m_[80];                                                              \
        snprintf(m_, sizeof m_, "expected (%d,%d,%d) but was (%d,%d,%d)", e_.r, e_.g, e_.b, a_.r, a_.g, a_.b); \
        TEST_ASSERT_TRUE_MESSAGE(e_ == a_, m_);                                   \
    } while (0)

}  // namespace support

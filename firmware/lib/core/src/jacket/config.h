// Fixed properties of the jacket and of protocol v1 (protocol/PROTOCOL.md).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jacket {

constexpr uint8_t kProtoVersion = 1;

constexpr uint8_t kStripCount = 2;        // 0 = left, 1 = right
constexpr uint16_t kLedsPerStrip = 144;
constexpr uint8_t kMaxSegments = 8;
constexpr uint8_t kPresetSlots = 8;
constexpr uint8_t kMaxPresetName = 16;    // bytes of UTF-8

constexpr uint16_t kBudgetMinMa = 400;       // below about 370 mA the jacket could light nothing
constexpr uint16_t kBudgetMaxMa = 2000;
constexpr uint16_t kBudgetDefaultMa = 900;

constexpr size_t kMaxFrame = 180;         // no frame on any characteristic is longer
constexpr uint32_t kLiveTimeoutMs = 500;  // live input counts as gone after this silence

constexpr uint8_t kAllSegments = 0xFF;    // `segment` value meaning "every segment"
constexpr uint8_t kNoPreset = 0xFF;       // STATE.preset when the scene matches no slot

constexpr uint8_t kStripLeft = 0x01;      // strip_mask bits
constexpr uint8_t kStripRight = 0x02;
constexpr uint8_t kStripMaskAll = 0x03;
constexpr uint8_t kGeomReversed = 0x01;   // geometry flags

}  // namespace jacket

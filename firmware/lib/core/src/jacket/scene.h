// Scene model: what the jacket shows (segments and their effects) and the
// settings that are not part of a scene, with validation and the compact
// versioned binary form used for persistence.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jacket/color.h"
#include "jacket/config.h"

namespace jacket {

// What a segment plays. Wire form: 10 bytes (PROTOCOL.md "EffectConfig").
struct EffectConfig {
    uint8_t effect = 0;      // effect catalogue id
    uint8_t palette = 0;     // palette catalogue id
    uint8_t speed = 128;     // time multiplier 2^((speed - 128) / 32)
    uint8_t intensity = 128; // effect-specific amount
    Rgb color1;
    Rgb color2;
};

// Where a segment sits. Wire form: 4 bytes (PROTOCOL.md "Segment geometry").
struct SegmentGeometry {
    uint8_t strip_mask = kStripMaskAll;
    uint8_t start = 0;
    uint8_t length = static_cast<uint8_t>(kLedsPerStrip);
    uint8_t flags = 0;

    bool reversed() const { return (flags & kGeomReversed) != 0; }
};

struct Segment {
    SegmentGeometry geometry;
    EffectConfig config;
};

// Everything a preset stores. Power is deliberately not part of it.
struct Scene {
    uint8_t brightness = 128;
    uint8_t segment_count = 1;
    Segment segments[kMaxSegments];
};

// Device settings that live outside the scene.
struct Settings {
    uint16_t budget_ma = kBudgetDefaultMa;
    bool dev_mode = false;
};

bool operator==(const EffectConfig& a, const EffectConfig& b);
bool operator==(const SegmentGeometry& a, const SegmentGeometry& b);
bool operator==(const Segment& a, const Segment& b);
bool operator==(const Scene& a, const Scene& b);  // compares only the used segments
inline bool operator!=(const EffectConfig& a, const EffectConfig& b) { return !(a == b); }
inline bool operator!=(const SegmentGeometry& a, const SegmentGeometry& b) { return !(a == b); }
inline bool operator!=(const Segment& a, const Segment& b) { return !(a == b); }
inline bool operator!=(const Scene& a, const Scene& b) { return !(a == b); }

// The scene a jacket with empty storage starts with: one segment covering both
// strips, Cylon in cyan and magenta.
Scene defaultScene();

// Geometry is valid when strip_mask is 1..3, length is 1..144, the range fits
// on the strip and no unknown flag bit is set.
bool isValid(const SegmentGeometry& g);

// Structure only: 1..8 segments, all geometries valid. Effect and palette ids
// are checked by the engine against its catalogues.
bool isValid(const Scene& scene);

uint16_t clampBudget(uint16_t milliamps);

// SET_SEGMENTS: replaces the layout. Segment i keeps the EffectConfig it had,
// segments beyond the old count copy the old segment 0. Returns false and
// leaves the scene alone if the new layout is invalid.
bool applySegments(Scene& scene, const SegmentGeometry* geometry, uint8_t count);

// ---- Persistence ----------------------------------------------------------
// Scene blob v1:    [1][brightness][count] count x (geometry 4 bytes, config 10 bytes)
// Settings blob v1: [1][budget_ma u16 LE][flags: bit 0 dev mode]
// Readers reject an unknown version byte and ignore trailing bytes.

constexpr uint8_t kSceneBlobVersion = 1;
constexpr uint8_t kSettingsBlobVersion = 1;
constexpr size_t kMaxSceneBlob = 3 + static_cast<size_t>(kMaxSegments) * 14;
constexpr size_t kSettingsBlobSize = 4;

// Both return the number of bytes written, 0 if `cap` is too small or the
// scene is invalid.
size_t serializeScene(const Scene& scene, uint8_t* out, size_t cap);
size_t serializeSettings(const Settings& settings, uint8_t* out, size_t cap);

// Both return false (and leave `out` alone) on a bad version, short data or
// invalid content. A stored budget outside the allowed range is clamped.
bool deserializeScene(const uint8_t* data, size_t len, Scene& out);
bool deserializeSettings(const uint8_t* data, size_t len, Settings& out);

}  // namespace jacket

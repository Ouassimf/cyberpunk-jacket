#include "jacket/scene.h"

#include "jacket/wire.h"

namespace jacket {

bool operator==(const EffectConfig& a, const EffectConfig& b) {
    return a.effect == b.effect && a.palette == b.palette && a.speed == b.speed && a.intensity == b.intensity &&
           a.color1 == b.color1 && a.color2 == b.color2;
}

bool operator==(const SegmentGeometry& a, const SegmentGeometry& b) {
    return a.strip_mask == b.strip_mask && a.start == b.start && a.length == b.length && a.flags == b.flags;
}

bool operator==(const Segment& a, const Segment& b) { return a.geometry == b.geometry && a.config == b.config; }

bool operator==(const Scene& a, const Scene& b) {
    if (a.brightness != b.brightness || a.segment_count != b.segment_count) return false;
    const uint8_t n = a.segment_count <= kMaxSegments ? a.segment_count : kMaxSegments;
    for (uint8_t i = 0; i < n; ++i) {
        if (a.segments[i] != b.segments[i]) return false;
    }
    return true;
}

Scene defaultScene() {
    Scene s;
    s.brightness = 128;
    s.segment_count = 1;
    Segment& seg = s.segments[0];
    seg.geometry = SegmentGeometry();
    seg.config.effect = 6;  // Cylon
    seg.config.palette = 0;
    seg.config.speed = 128;
    seg.config.intensity = 128;
    seg.config.color1 = Rgb(0, 255, 204);
    seg.config.color2 = Rgb(255, 0, 255);
    return s;
}

bool isValid(const SegmentGeometry& g) {
    if (g.strip_mask == 0 || (g.strip_mask & ~kStripMaskAll) != 0) return false;
    if (g.length == 0 || g.length > kLedsPerStrip) return false;
    if (static_cast<uint16_t>(g.start) + g.length > kLedsPerStrip) return false;
    if ((g.flags & ~kGeomReversed) != 0) return false;
    return true;
}

bool isValid(const Scene& scene) {
    if (scene.segment_count == 0 || scene.segment_count > kMaxSegments) return false;
    for (uint8_t i = 0; i < scene.segment_count; ++i) {
        if (!isValid(scene.segments[i].geometry)) return false;
    }
    return true;
}

uint16_t clampBudget(uint16_t milliamps) {
    if (milliamps < kBudgetMinMa) return kBudgetMinMa;
    if (milliamps > kBudgetMaxMa) return kBudgetMaxMa;
    return milliamps;
}

bool applySegments(Scene& scene, const SegmentGeometry* geometry, uint8_t count) {
    if (geometry == nullptr || count == 0 || count > kMaxSegments) return false;
    for (uint8_t i = 0; i < count; ++i) {
        if (!isValid(geometry[i])) return false;
    }
    const uint8_t old_count = scene.segment_count <= kMaxSegments ? scene.segment_count : kMaxSegments;
    const EffectConfig first = scene.segments[0].config;
    for (uint8_t i = 0; i < count; ++i) {
        scene.segments[i].geometry = geometry[i];
        if (i >= old_count) scene.segments[i].config = first;
    }
    scene.segment_count = count;
    return true;
}

size_t serializeScene(const Scene& scene, uint8_t* out, size_t cap) {
    if (out == nullptr || !isValid(scene)) return 0;
    wire::Writer w(out, cap);
    w.u8(kSceneBlobVersion);
    w.u8(scene.brightness);
    w.u8(scene.segment_count);
    for (uint8_t i = 0; i < scene.segment_count; ++i) {
        w.geometry(scene.segments[i].geometry);
        w.config(scene.segments[i].config);
    }
    return w.size();
}

bool deserializeScene(const uint8_t* data, size_t len, Scene& out) {
    if (data == nullptr) return false;
    wire::Reader r(data, len);
    if (r.u8() != kSceneBlobVersion || !r.ok()) return false;
    Scene s;
    s.brightness = r.u8();
    s.segment_count = r.u8();
    if (!r.ok() || s.segment_count == 0 || s.segment_count > kMaxSegments) return false;
    for (uint8_t i = 0; i < s.segment_count; ++i) {
        s.segments[i].geometry = r.geometry();
        s.segments[i].config = r.config();
    }
    if (!r.ok() || !isValid(s)) return false;
    out = s;
    return true;
}

size_t serializeSettings(const Settings& settings, uint8_t* out, size_t cap) {
    if (out == nullptr) return 0;
    wire::Writer w(out, cap);
    w.u8(kSettingsBlobVersion);
    w.u16(clampBudget(settings.budget_ma));
    w.u8(settings.dev_mode ? 0x01 : 0x00);
    return w.size();
}

bool deserializeSettings(const uint8_t* data, size_t len, Settings& out) {
    if (data == nullptr) return false;
    wire::Reader r(data, len);
    if (r.u8() != kSettingsBlobVersion || !r.ok()) return false;
    const uint16_t budget = r.u16();
    const uint8_t flags = r.u8();
    if (!r.ok()) return false;
    out.budget_ma = clampBudget(budget);
    out.dev_mode = (flags & 0x01) != 0;
    return true;
}

}  // namespace jacket

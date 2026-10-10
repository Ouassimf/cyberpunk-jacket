// Portable core of the jacket firmware: protocol codec, scene model, effects
// and the engine that ties them together. No Arduino, ESP-IDF or FastLED: the
// same code runs on the ESP32 and in the host unit tests (pio test -e native).
//
// Device code needs only this header and, in practice, only jacket/engine.h
// (Engine, FrameSink, EngineConfig) and jacket/storage.h (Storage).
#pragma once

#include "jacket/beat_clock.h"
#include "jacket/boot_policy.h"
#include "jacket/color.h"
#include "jacket/config.h"
#include "jacket/effect.h"
#include "jacket/engine.h"
#include "jacket/live_signals.h"
#include "jacket/palette.h"
#include "jacket/power.h"
#include "jacket/protocol.h"
#include "jacket/scene.h"
#include "jacket/storage.h"

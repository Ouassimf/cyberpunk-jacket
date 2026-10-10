#include "log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

namespace {
volatile LogSink g_sink = nullptr;
}

void setLogSink(LogSink sink) { g_sink = sink; }

void logLine(const char* format, ...) {
    char line[448];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line - 1, format, args);
    va_end(args);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof line) - 2) n = sizeof line - 2;
    line[n++] = '\n';
    Serial.write(reinterpret_cast<const uint8_t*>(line), n);
    const LogSink sink = g_sink;
    if (sink != nullptr) sink(line, n);
}

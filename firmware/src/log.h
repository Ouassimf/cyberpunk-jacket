// One-line log messages: always to the serial port, and to the dev log stream
// when one is attached. Safe to call from any task; not from interrupts.
#pragma once

#include <stddef.h>

// printf-style; the newline is added. Lines longer than 446 characters are cut.
void logLine(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Where a copy of every line goes (the TCP log stream). The sink is called
// from whichever task logs, so it must only buffer. nullptr removes it.
using LogSink = void (*)(const char* line, size_t len);
void setLogSink(LogSink sink);

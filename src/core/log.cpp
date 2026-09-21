// SPDX-License-Identifier: GPL-2.0-or-later
#include "qcam/log.h"

#include <cstdarg>
#include <cstdio>

namespace qcam {
namespace {

void DefaultSink(LogLevel level, const char* msg) {
    static const char* kNames[] = {"ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"};
    int idx = static_cast<int>(level);
    if (idx < 0 || idx > 4) idx = 2;
    std::fprintf(stderr, "[qcam] %s %s\n", kNames[idx], msg);
}

LogSink  g_sink  = &DefaultSink;
LogLevel g_level = LogLevel::Info;

}  // namespace

void SetLogSink(LogSink sink) { g_sink = sink ? sink : &DefaultSink; }
void SetLogLevel(LogLevel level) { g_level = level; }
LogLevel GetLogLevel() { return g_level; }

void LogPrintf(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) > static_cast<int>(g_level)) return;
    if (!g_sink) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (static_cast<size_t>(n) >= sizeof(buf)) {
        buf[sizeof(buf) - 4] = '.';
        buf[sizeof(buf) - 3] = '.';
        buf[sizeof(buf) - 2] = '.';
        buf[sizeof(buf) - 1] = '\0';
    }
    g_sink(level, buf);
}

}  // namespace qcam

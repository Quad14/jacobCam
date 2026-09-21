// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal severity-filtered logging. The service writes to a file and to
// ETW/OutputDebugString; qcamctl writes to stderr; the unit tests capture.

#ifndef QCAM_LOG_H_
#define QCAM_LOG_H_

namespace qcam {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

using LogSink = void (*)(LogLevel level, const char* msg);

void SetLogSink(LogSink sink);
void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();

void LogPrintf(LogLevel level, const char* fmt, ...);

#define QCAM_LOGE(...) ::qcam::LogPrintf(::qcam::LogLevel::Error, __VA_ARGS__)
#define QCAM_LOGW(...) ::qcam::LogPrintf(::qcam::LogLevel::Warn,  __VA_ARGS__)
#define QCAM_LOGI(...) ::qcam::LogPrintf(::qcam::LogLevel::Info,  __VA_ARGS__)
#define QCAM_LOGD(...) ::qcam::LogPrintf(::qcam::LogLevel::Debug, __VA_ARGS__)
#define QCAM_LOGT(...) ::qcam::LogPrintf(::qcam::LogLevel::Trace, __VA_ARGS__)

}  // namespace qcam

#endif  // QCAM_LOG_H_

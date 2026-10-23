#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define ANYPORT_PRINTF_FORMAT(fmt_index, args_index) \
    __attribute__((format(printf, fmt_index, args_index)))
#else
#define ANYPORT_PRINTF_FORMAT(fmt_index, args_index)
#endif

namespace anyport::platform {

enum class LogLevel { Debug, Info, Warn, Error };

void set_log_level(LogLevel level);
void log(LogLevel level, const char* format, ...) ANYPORT_PRINTF_FORMAT(2, 3);

}  // namespace anyport::platform

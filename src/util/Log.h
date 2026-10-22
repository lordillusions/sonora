#pragma once
// Minimal logging to stderr, prefixed "[sonora]". Message thread / non-realtime only —
// NEVER call from the audio thread. Grep the app's stderr for "[sonora]" when debugging.

#include <cstdarg>
#include <cstdio>

namespace sonora
{

inline void logf(const char* fmt, ...)
{
    std::fputs("[sonora] ", stderr);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace sonora

#pragma once

#if defined(_WIN32) && defined(__MINGW64__)
#include <time.h>
#include <chrono>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
using clockid_t = int;

static inline int clock_gettime(clockid_t, struct timespec* ts) {
    if (!ts) return -1;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(now);
    ts->tv_sec = static_cast<time_t>(sec.count());
    ts->tv_nsec = static_cast<long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - sec).count());
    return 0;
}
#endif
#endif

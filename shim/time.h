/*
 * shim/time.h — Freestanding time.h for bare-metal UEFI
 */

#ifndef SHIM_TIME_H
#define SHIM_TIME_H

#include <stddef.h>
#include <stdint.h>

typedef long time_t;
typedef long clock_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#define CLOCKS_PER_SEC 1000000

static inline time_t time(time_t *t) {
    if (t) *t = 0;
    return 0;
}

static inline clock_t clock(void) { return 0; }

#endif /* SHIM_TIME_H */

/* Monotonic clock for the Linux platform. Unit tests provide their own
 * pf_now_ms() to inject a fake clock; this archive member then never gets
 * pulled in by the linker. */
#include "core/pf.h"

#include <time.h>

uint64_t pf_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

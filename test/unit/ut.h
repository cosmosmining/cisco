/* Tiny unit-test harness: assert-and-exit, no framework dependency. */
#ifndef UT_H
#define UT_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define UT_ASSERT(cond)                                                                            \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "\nFAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define UT_ASSERT_EQ(a, b)                                                                         \
    do {                                                                                           \
        long long ut_a = (long long)(a), ut_b = (long long)(b);                                    \
        if (ut_a != ut_b) {                                                                        \
            fprintf(stderr, "\nFAIL %s:%d: %s == %s  (%lld != %lld)\n", __FILE__, __LINE__, #a,    \
                    #b, ut_a, ut_b);                                                               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define UT_RUN(fn)                                                                                 \
    do {                                                                                           \
        printf("  %-48s", #fn);                                                                    \
        fflush(stdout);                                                                            \
        fn();                                                                                      \
        printf("ok\n");                                                                            \
    } while (0)

#endif /* UT_H */

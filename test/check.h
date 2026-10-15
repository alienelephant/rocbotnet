/* Minimal test helpers shared by the test programs. */
#ifndef ROCLINK_TEST_CHECK_H
#define ROCLINK_TEST_CHECK_H

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        long va_ = (long)(a), vb_ = (long)(b);                                        \
        ++g_checks;                                                                   \
        if (va_ != vb_) {                                                             \
            ++g_failures;                                                             \
            printf("  FAIL %s:%d: %s == %s (%ld vs %ld)\n", __FILE__, __LINE__, #a, #b, \
                   va_, vb_);                                                         \
        }                                                                             \
    } while (0)

#define CHECK_BYTES(buf, n, expected)                                                 \
    do {                                                                              \
        ++g_checks;                                                                   \
        if ((size_t)(n) != sizeof(expected) || memcmp((buf), (expected), sizeof(expected)) != 0) { \
            ++g_failures;                                                             \
            printf("  FAIL %s:%d: %s does not match %s\n", __FILE__, __LINE__, #buf,  \
                   #expected);                                                        \
        }                                                                             \
    } while (0)

#define RUN(fn)              \
    do {                     \
        printf("%s\n", #fn); \
        fn();                \
    } while (0)

#define REPORT()                                                       \
    (printf("\n%d checks, %d failures\n", g_checks, g_failures), g_failures ? 1 : 0)

#endif

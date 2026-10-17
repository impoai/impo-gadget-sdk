/* Tiny assertion helpers shared by the host tests. */
#pragma once

#include <stdio.h>

static int failures;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            printf("ok   %s\n", what);                                     \
        } else {                                                           \
            printf("FAIL %s  (%s:%d)\n", what, __FILE__, __LINE__);        \
            failures++;                                                    \
        }                                                                  \
    } while (0)

#define DONE()                                                             \
    do {                                                                   \
        printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures); \
        return failures != 0;                                              \
    } while (0)

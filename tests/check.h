/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * check.h - a tiny test harness: CHECK, CHECK_STR, TEST_MAIN_END.
 */
#ifndef TBOX_CHECK_H
#define TBOX_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);    \
            test_failures++;                                                             \
        }                                                                                \
    } while (0)

#define CHECK_STR(a, b)                                                                  \
    do {                                                                                 \
        const char *_a = (a), *_b = (b);                                                 \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {                           \
            fprintf(stderr, "%s:%d: CHECK_STR failed: \"%s\" != \"%s\"\n", __FILE__,    \
                    __LINE__, _a ? _a : "(null)", _b ? _b : "(null)");                   \
            test_failures++;                                                             \
        }                                                                                \
    } while (0)

#define TEST_MAIN_END()                                                                  \
    do {                                                                                 \
        if (test_failures == 0) {                                                        \
            printf("ok\n");                                                              \
            return 0;                                                                    \
        }                                                                                \
        fprintf(stderr, "%d check(s) failed\n", test_failures);                          \
        return 1;                                                                        \
    } while (0)

#endif

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_smoke.c — placeholder CTest case.
 *
 * Proves the unit-test pipeline (compile -> link -> test_*.exe -> ctest) is
 * wired up. Real suites land per ladder step: tests/test_cli.c (Step 1),
 * tests/test_paths.c (Step 2), etc.
 */

#include <stdio.h>

#ifndef TBOX_VERSION
#define TBOX_VERSION "(unset)"
#endif

int main(void)
{
    /* 1) build-time version is a non-empty literal */
    if (TBOX_VERSION[0] == '\0') {
        fprintf(stderr, "FAIL: TBOX_VERSION is empty\n");
        return 1;
    }
    /* 2) arithmetic/runtime sanity — compiler and CRT actually work */
    if (2 + 2 != 4) {
        fprintf(stderr, "FAIL: arithmetic sanity\n");
        return 1;
    }
    printf("test_smoke: ok (tbox %s)\n", TBOX_VERSION);
    return 0;
}
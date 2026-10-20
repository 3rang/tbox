/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_tbox_td.c — smoke test against the real linked tdjson runtime.
 * CMake stages the DLLs next to the exe, so no path arguments are needed.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td/tbox_td.h"

int main(void) {
    /* 1. a synchronous call round-trips real JSON */
    char *out = tbox_td_execute("{\"@type\":\"getLogStream\"}");
    assert(out != NULL);
    printf("getLogStream -> %s\n", out);
    assert(strstr(out, "logStream") != NULL);
    free(out);

    /* 2. async-only methods come back as TDLib error objects */
    out = tbox_td_execute("{\"@type\":\"getUser\",\"user_id\":1}");
    assert(out != NULL);
    assert(strstr(out, "\"@type\":\"error\"") != NULL);
    free(out);

    /* 3. verbosity wrapper is callable */
    tbox_td_init(1);

    printf("tbox_td smoke OK\n");
    return 0;
}

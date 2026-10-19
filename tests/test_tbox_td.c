/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_tbox_td.c — smoke test against the real vendored tdjson library.
 *
 * Usage: test_tbox_td <path-to-tdjson-library>
 * Exits 2 on wrong usage, 0 on pass/skip, assert-crashes on failure.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td/tbox_td.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tdjson-library-path>\n", argv[0]);
        return 2;
    }

    /* 1. a bad path must fail cleanly, not crash */
    assert(tbox_td_open("no-such-library-xyz") == NULL);
    assert(tbox_td_open("") == NULL);
    assert(tbox_td_execute(NULL, "{}") == NULL);

    /* 2. the real library loads */
    TBoxTd *td = tbox_td_open(argv[1]);
    if (td == NULL) {
        printf("skip: could not load %s\n", argv[1]);
        return 0;
    }

    /* 3. a synchronous call round-trips real JSON */
    char *out = tbox_td_execute(td, "{\"@type\":\"getLogStream\"}");
    assert(out != NULL);
    printf("getLogStream -> %s\n", out);
    assert(strstr(out, "logStream") != NULL);
    free(out);

    /* 4. async-only methods come back as TDLib error objects */
    out = tbox_td_execute(td, "{\"@type\":\"getUser\",\"user_id\":1}");
    assert(out != NULL);
    assert(strstr(out, "\"@type\":\"error\"") != NULL);
    free(out);

    tbox_td_close(td);
    printf("tbox_td smoke OK\n");
    return 0;
}

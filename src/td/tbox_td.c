/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td.c — thin wrappers over the td_json_client C API.
 *
 * The symbols come from tdjson.dll via the import library that
 * cmake/tdjson-vendored.cmake generates from a .def file, so there is no
 * dlopen/LoadLibrary machinery here. td_execute results are owned by TDLib
 * until the next call — this wrapper copies before returning.
 */
#include "td/tbox_td.h"

#include <stdlib.h>
#include <string.h>

void tbox_td_init(int verbosity) {
    td_set_log_verbosity_level(verbosity);
}

char *tbox_td_execute(const char *request_json) {
    if (request_json == NULL) {
        return NULL;
    }
    const char *out = td_execute(request_json);
    if (out == NULL) {
        return NULL;
    }
    size_t n = strlen(out);
    char *copy = malloc(n + 1);
    if (copy != NULL) {
        memcpy(copy, out, n + 1);
    }
    return copy;
}

void tbox_td_close(void) {
    /* tdjson has no global teardown; nothing to release. Kept for ABI
     * symmetry with the old handle-based API and future cleanup hooks. */
}

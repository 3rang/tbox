/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * serve.c — `tbox-engine serve <profile-dir>`: the long-running engine
 * (spec 6, spec 8.1).
 *
 * This is the mode the desktop front uses: one process per profile, spoken
 * to in JSON lines on stdin/stdout. The body here is deliberately thin -
 * it wires the real TDLib api and the stdio transport to the engine loop
 * and maps startup failures to the spec's exit codes. Everything else
 * lives in src/engine/.
 */
#include "cmd/cmd.h"

#include <stdio.h>

#include "cJSON.h"
#include "engine/engine.h"
#include "engine/protocol.h"
#include "engine/transport_stdio.h"
#include "td/tbox_td_api.h"

/* Spec 8.1 step 3: busy -> one JSON error line, then exit 2. The line is
 * an event, not a response, because no request was read yet. */
static void report_busy(void) {
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return;
    }
    cJSON_AddStringToObject(d, "code", "PROFILE_BUSY");
    cJSON_AddStringToObject(d, "message",
                            "another engine already owns this profile");
    char *line = tbox_proto_event("engine.error", d);
    cJSON_Delete(d);
    if (line != NULL) {
        printf("%s\n", line);
        fflush(stdout);
        free(line);
    }
}

int tbox_cmd_serve(const char *profile_dir) {
    if (profile_dir == NULL || profile_dir[0] == '\0') {
        fprintf(stderr, "usage: tbox-engine serve <profile-dir>\n");
        return TBOX_SERVE_BAD_ARGS;
    }

    TBoxStdioTransport *t = tbox_transport_stdio_new();
    if (t == NULL) {
        fprintf(stderr, "error: out of memory\n");
        return TBOX_ERROR;
    }

    TBoxEngineConfig cfg;
    cfg.profile_dir = profile_dir;
    cfg.api_hash = NULL; /* secrets arrive over the protocol, never argv */
    cfg.db_key = NULL;

    int err = 0;
    TBoxEngine *e = tbox_engine_new(tbox_td_api(), &cfg, tbox_transport_stdio_emit,
                                    t, &err);
    if (e == NULL) {
        if (err == TBOX_SERVE_PROFILE_BUSY) {
            report_busy();
        } else {
            fprintf(stderr, "error: could not start the engine\n");
        }
        tbox_transport_stdio_free(t);
        return err == 0 ? TBOX_ERROR : err;
    }

    if (tbox_transport_stdio_start_reader(t, e) != 0) {
        fprintf(stderr, "error: could not start the stdin reader\n");
        tbox_engine_free(e);
        tbox_transport_stdio_free(t);
        return TBOX_ERROR;
    }

    int rc = tbox_engine_run(e); /* 0 clean shutdown, 1 other (spec 6.1) */
    tbox_transport_stdio_free(t);
    tbox_engine_free(e);
    return rc;
}

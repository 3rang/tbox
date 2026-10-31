/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox logout - end the session on Telegram's side and wipe the local
 * TDLib database (TDLib does that itself on logOut).
 */
#include "cmd/commands.h"
#include "core/session.h"

int cmd_logout(int argc, char **argv) {
    (void)argv;
    if (argc != 0) {
        fprintf(stderr, "usage: tbox logout\n");
        return EXIT_USAGE;
    }
    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        return rc;
    }

    cJSON *r = td_request(&s.td, jreq("logOut"), TD_TIMEOUT);
    if (td_is_error(r)) {
        fprintf(stderr, "error: logOut: %s\n", td_error_message(r));
        rc = EXIT_ERROR;
    } else {
        printf("Logged out.\n");
    }
    cJSON_Delete(r);
    /* logOut ends in authorizationStateClosed on its own; this waits for it. */
    session_close(&s);
    return rc;
}

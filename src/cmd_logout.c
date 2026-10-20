/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox logout - end the session on Telegram's side and wipe the local
 * TDLib database (TDLib does that itself on logOut).
 */
#include <stdio.h>

#include "commands.h"
#include "session.h"

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
        cJSON_Delete(r);
        session_close(&s);
        return EXIT_ERROR;
    }
    cJSON_Delete(r);

    /* logOut ends in authorizationStateClosed; wait for it. */
    for (int idle = 0; idle < 60 && !s.td.closed;) {
        if (!td_pump(&s.td, 0.5)) {
            idle++;
        }
    }
    session_close(&s); /* no-op once closed */
    printf("Logged out.\n");
    return EXIT_OK;
}

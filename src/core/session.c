/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "core/session.h"

#include <stdio.h>
#include <string.h>

#include "tg/auth.h"

int session_open(Session *s, int interactive) {
    char err[4096];
    memset(s, 0, sizeof *s);

    if (config_load(&s->cfg, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return EXIT_ERROR;
    }
    if (!config_has_credentials(&s->cfg)) {
        fprintf(stderr, "%s\n", CONFIG_CREDENTIALS_HELP);
        return EXIT_NOT_LOGGED_IN;
    }

    td_init(&s->td, td_real_api());

    switch (auth_run(&s->td, &s->cfg, interactive, err, sizeof err)) {
    case AUTH_OK:
        return EXIT_OK;
    case AUTH_NOT_LOGGED_IN:
        fprintf(stderr, "not logged in; run `tbox login`\n");
        td_close(&s->td);
        return EXIT_NOT_LOGGED_IN;
    case AUTH_FAILED:
    default:
        fprintf(stderr, "error: %s\n", err);
        td_close(&s->td);
        return EXIT_ERROR;
    }
}

void session_close(Session *s) {
    td_close(&s->td);
}

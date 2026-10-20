/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * session.h - what every command does first and last: load the config,
 * load TDLib, get authorized; then close TDLib cleanly so its database
 * is flushed.
 */
#ifndef TBOX_SESSION_H
#define TBOX_SESSION_H

#include "config.h"
#include "td.h"

typedef struct Session {
    Config cfg;
    Td td;
} Session;

/* Exit codes shared by all commands. */
enum {
    EXIT_OK = 0,
    EXIT_ERROR = 1,
    EXIT_USAGE = 2,
    EXIT_NOT_LOGGED_IN = 3,
};

/* Open and authorize. interactive=1 only for `tbox login`. Prints the
 * problem and returns an exit code on failure, EXIT_OK on success. */
int session_open(Session *s, int interactive);

void session_close(Session *s);

#endif

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * cmd.h — command implementations (the cli/ layer only parses + dispatches).
 */
#ifndef TBOX_CMD_H
#define TBOX_CMD_H

#define TBOX_VERSION "0.1.0-dev"

/* Exit codes (deliberately small):
 *   0 = ok, 1 = runtime error, 2 = unknown command / bad usage */
#define TBOX_EXIT_OK         0
#define TBOX_ERROR           1
#define TBOX_UNKNOWN_COMMAND 2

/* Exit codes of `serve` (spec 6.1): superset of the above because a front
 * needs to tell "profile busy" apart from a bad invocation. */
#define TBOX_SERVE_PROFILE_BUSY 2
#define TBOX_SERVE_BAD_ARGS     3
#define TBOX_SERVE_DB_ERROR     4

int tbox_cmd_version(void);
int tbox_cmd_auth_state(const char *td_state); /* debug: one FSM step */
int tbox_cmd_td_smoke(void);
/* Dev login over the legacy one-shot protocol: QR + optional 2FA, one
 * human-ish JSON line per event. Credentials are NOT argv (P1): the caller
 * writes one JSON object to stdin first:
 *   {"api_id":"123","api_hash":"...","db_key":"<base64>"}   (db_key optional)
 * The GUI/CLI use the long-running engine (serve) instead; this stays as a
 * developer shortcut and for the engine's scripted tests. */
int tbox_cmd_login(void);

/* The long-running engine: protocol v1 over stdin/stdout (spec 6, 8.1).
 * Exit codes: 0 clean shutdown, 2 profile busy, 3 bad args, 4 db error. */
int tbox_cmd_serve(const char *profile_dir);

#endif /* TBOX_CMD_H */

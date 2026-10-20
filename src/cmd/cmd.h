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

int tbox_cmd_version(void);
int tbox_cmd_auth_state(const char *td_state); /* debug: one FSM step */
int tbox_cmd_td_smoke(void);
/* Login with QR + optional 2FA. Credentials arrive as parameters — the
 * C core never reads config; the Python front will pass them
 * programmatically. argv here is the developer shortcut. */
int tbox_cmd_login(const char *session_dir, const char *api_id, const char *api_hash);

#endif /* TBOX_CMD_H */

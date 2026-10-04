/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TDSERVE_H
#define TDSERVE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * tbox_td_serve_run - the `tbox serve` runner that main.c injects into the
 * command layer (see cmd/cmd.h).
 *
 * Everything `serve` is, in order:
 *
 *   1. parameters + credentials -> the TDLib session in <root>/tdlib
 *   2. authorizationStateReady   -> an existing session, fast path; if it is
 *                                   not authorized, say so and point at
 *                                   `tbox auth` instead of pretending
 *   3. getMe                     -> the Saved Messages chat id
 *   4. scan                      -> the archive, seeded from index.json so
 *                                   the tree is servable before page 1 lands
 *   5. FTP up                    -> 127.0.0.1, USER tbox + the session token
 *   6. watch                     -> updateNewMessage sets dirty, and a
 *                                   debounced rescan swaps the index under
 *                                   the FTP server's lock
 *
 * It publishes status.json at every state change, and it owns the session for
 * its whole life: two `serve` processes cannot both hold it (the session lock
 * in cmd/serve.c is what makes that true).
 */
int tbox_td_serve_run(const char *root, volatile bool *should_stop);

#ifdef __cplusplus
}
#endif

#endif /* TDSERVE_H */

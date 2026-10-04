/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TDHANDLER_H
#define TDHANDLER_H

#include <stdio.h>
#include <stdbool.h>

/* Official TDLib C API: td_create_client_id / td_send / td_receive / td_execute.
 * Headers vendored in third_party/tdlib_headers. */
#include <td/telegram/td_json_client.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * tbox auth flow (td layer). Runs the QR-login conversation with TDLib:
 *
 *   setTdlibParameters -> (restore fast-path) -> requestQrCodeAuthentication
 *   -> authorizationStateWaitOtherDeviceConfirmation -> QR in terminal
 *   -> [authorizationStateWaitPassword -> masked 2FA prompt]
 *   -> authorizationStateReady -> getMe -> "Logged in as ..." -> close
 *
 * The session persists in the TDLib database under the data dir
 * (Tier 1: plain, no setDatabaseEncryptionKey yet).
 *
 * Designed to run on a worker thread (util/tbox_thread). The receive
 * loop polls td_receive(1.0); every tick it checks *should_stop, so the
 * signal listener can cancel it cooperatively:
 *
 *   should_stop == NULL  -> run to completion (e.g. the demo path)
 *   *should_stop != 0    -> send close, wait for authorizationStateClosed
 *                           (bounded), return TBOX_TD_CANCELLED
 *
 * Returns (see TBOX_TD_* below).
 */
int tbox_td_auth_run(const char *data_dir, volatile bool *should_stop);

/* tbox_td_auth_run result codes */
#define TBOX_TD_OK         0   /* authorizationStateReady reached */
#define TBOX_TD_CANCELLED  1   /* should_stop fired mid-flow */
#define TBOX_TD_ERROR      2   /* credentials / TDLib error */

#ifdef __cplusplus
}
#endif

#endif /* TDHANDLER_H */

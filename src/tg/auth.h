/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * auth.h - the Telegram login flow.
 *
 * TDLib drives authorization as a state machine and tells us the current
 * state through updateAuthorizationState. Each state wants one answer:
 *
 *   WaitTdlibParameters  -> setTdlibParameters (api_id, api_hash, dirs)
 *   WaitPhoneNumber      -> setAuthenticationPhoneNumber
 *   WaitCode             -> checkAuthenticationCode  (sent to the Telegram app or SMS)
 *   WaitPassword         -> checkAuthenticationPassword  (two-step verification)
 *   Ready                -> we are logged in
 *
 * Once Ready has been reached the session lives in TDLib's database, so
 * the next run goes WaitTdlibParameters -> Ready without asking anything.
 * auth_run() walks this; interactive=0 (every command but login) stops
 * with "not logged in" as soon as user input would be needed.
 */
#ifndef TBOX_AUTH_H
#define TBOX_AUTH_H

#include <stddef.h>

#include "cJSON.h"
#include "core/config.h"
#include "core/td.h"

typedef enum AuthState {
    AUTH_UNKNOWN = 0,
    AUTH_WAIT_PARAMS,
    AUTH_WAIT_PHONE,
    AUTH_WAIT_CODE,
    AUTH_WAIT_PASSWORD,
    AUTH_WAIT_REGISTRATION,
    AUTH_WAIT_OTHER,       /* email, QR, ... : states tbox does not drive */
    AUTH_READY,
    AUTH_LOGGING_OUT,
    AUTH_CLOSING,
    AUTH_CLOSED,
} AuthState;

/* Map TDLib's authorizationState* type name to our enum. Pure. */
AuthState auth_state_from_type(const char *type);
const char *auth_state_name(AuthState s);

/* Build the setTdlibParameters request from the config. Pure. */
cJSON *auth_tdlib_parameters(const Config *cfg);

/* Result of auth_run. */
typedef enum AuthResult {
    AUTH_OK = 0,
    AUTH_NOT_LOGGED_IN,    /* needs `tbox login` */
    AUTH_FAILED,           /* err has the reason */
} AuthResult;

/* Drive the flow until Ready. Prompts on the terminal when interactive.
 * Installs its own update handler on td and restores the previous one. */
AuthResult auth_run(Td *td, const Config *cfg, int interactive, char *err, size_t errlen);

#endif

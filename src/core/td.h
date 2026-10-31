/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * td.h - the bridge to TDLib's JSON interface (libtdjson).
 *
 * TDLib's C API is four functions, linked in at build time (see
 * cmake/tdlib.cmake). They sit in a TdApi table so the tests can plug in
 * a fake instead; td_real_api() is the table with the real ones.
 *
 * Talking to TDLib is: send a JSON request, receive JSON events. Events
 * are either replies (they carry back the "@extra" we attached) or
 * updates (anything TDLib wants to tell us). td_request() does the
 * bookkeeping: it posts, then polls events until the matching reply
 * arrives, handing every update met on the way to the update handler.
 */
#ifndef TBOX_TD_H
#define TBOX_TD_H

#include <stddef.h>

#include "cJSON.h"
#include "core/json.h"

typedef struct TdApi {
    int (*create_client_id)(void);
    void (*send)(int client_id, const char *request);
    const char *(*receive)(double timeout);
    const char *(*execute)(const char *request);
} TdApi;

typedef struct Td Td;
typedef void (*TdUpdateFn)(Td *td, const cJSON *update, void *user);

struct Td {
    TdApi api;
    int client_id;
    long seq;              /* @extra counter */
    int closed;            /* authorizationStateClosed was seen */
    int debug;             /* TBOX_DEBUG: echo JSON in/out (secrets blanked), TDLib warnings */
    TdUpdateFn on_update;
    void *on_update_user;
};

/* The linked TDLib. */
const TdApi *td_real_api(void);

/* Create a client. Verbosity 1 keeps TDLib quiet except for fatal errors. */
void td_init(Td *td, const TdApi *api);

void td_set_update_handler(Td *td, TdUpdateFn fn, void *user);

/* Send without waiting for a reply. Takes ownership of req. */
void td_post(Td *td, cJSON *req);

/* Wait up to timeout seconds for one event. Caller frees. NULL on timeout.
 * Does not dispatch to the update handler. */
cJSON *td_poll(Td *td, double timeout);

/* Receive one event and dispatch it to the update handler.
 * Returns 1 if an event was handled, 0 on timeout. */
int td_pump(Td *td, double timeout);

/* Send req and return its reply (may be an "error" object). Takes
 * ownership of req. Updates seen meanwhile go to the handler. NULL after
 * timeout seconds of silence or once TDLib has closed. Caller frees. */
cJSON *td_request(Td *td, cJSON *req, double timeout);

/* Ask TDLib to close and wait until it has. Flushes its database. */
void td_close(Td *td);

/* Reply inspection; both accept NULL (a timeout). */
int td_is_error(const cJSON *reply);
const char *td_error_message(const cJSON *reply);

/* Default wait for a reply, in seconds of silence. */
#define TD_TIMEOUT 30.0

#endif

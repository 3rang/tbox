/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * td.h - the bridge to TDLib's JSON interface (libtdjson).
 *
 * libtdjson exposes four C functions. tbox loads the library at run time
 * so the same executable works with the libtdjson staged next to it, one
 * named by $TBOX_TDJSON, or one found on the system's library path. The
 * functions sit in a TdApi table so tests can plug in a fake.
 *
 * Talking to TDLib is: send a JSON request, receive JSON events. Events
 * are either replies (they carry back the "@extra" we attached) or
 * updates (anything TDLib wants to tell us). td_request() does the
 * bookkeeping: it sends, then pumps events until the matching reply
 * arrives, handing every update met on the way to the update handler.
 */
#ifndef TBOX_TD_H
#define TBOX_TD_H

#include <stddef.h>

#include "cJSON.h"
#include "json.h"

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
    int debug;             /* TBOX_DEBUG: echo every JSON in/out to stderr */
    TdUpdateFn on_update;
    void *on_update_user;
};

/* Locate and load libtdjson. Returns 0 and fills api, or -1 with err. */
int td_load(TdApi *api, char *err, size_t errlen);

/* Create a client. Verbosity 1 keeps TDLib quiet except for fatal errors. */
void td_init(Td *td, const TdApi *api);

void td_set_update_handler(Td *td, TdUpdateFn fn, void *user);

/* Send without waiting. Takes ownership of req. */
void td_send(Td *td, cJSON *req);

/* Wait up to timeout seconds for one event. Caller frees. NULL on timeout.
 * Does not dispatch to the update handler. */
cJSON *td_receive(Td *td, double timeout);

/* Receive one event and dispatch it to the update handler.
 * Returns 1 if an event was handled, 0 on timeout. */
int td_pump(Td *td, double timeout);

/* Send req and return its reply (may be an "error" object). Takes
 * ownership of req. Updates seen meanwhile go to the handler. NULL if no
 * reply arrived within timeout seconds. Caller frees. */
cJSON *td_request(Td *td, cJSON *req, double timeout);

/* Ask TDLib to close and wait until it has. Flushes its database. */
void td_close(Td *td);

/* Reply inspection. */
int td_is_error(const cJSON *reply);
int td_error_code(const cJSON *reply);
const char *td_error_message(const cJSON *reply);

/* Default wait for a network round trip, in seconds. */
#define TD_TIMEOUT 30.0

#endif

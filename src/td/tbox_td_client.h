/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td_client.h — one TDLib client id plus its receive thread.
 *
 * Routing principle (same as the reference design):
 *   - an event WITH "@extra"  is a reply to a request -> matched later
 *   - an event WITHOUT        is an update            -> update handlers
 *
 * The tdjson entry points are injected as data (TBoxTdApi) so tests can
 * replace the real library with a scripted fake. Update callbacks receive
 * the parsed cJSON tree; it is freed when the callback returns — copy
 * anything you keep.
 */
#ifndef TBOX_TD_CLIENT_H
#define TBOX_TD_CLIENT_H

#include "tbox_td_api.h"

#include "cJSON.h"

typedef struct TBoxTdClient TBoxTdClient;

typedef void (*TBoxTdUpdateFn)(void *user_data, const cJSON *update);

/* Creates a client id and starts the receive thread. */
TBoxTdClient *tbox_td_client_open(const TBoxTdApi *api, int verbosity);

void tbox_td_client_on_update(TBoxTdClient *c, TBoxTdUpdateFn handler, void *user_data);

/* Fire-and-forget: td_send without waiting for a reply. */
void tbox_td_client_send(TBoxTdClient *c, const char *request_json);

/* Stops the receive thread (wakes within its 1 s poll) and frees the client. */
void tbox_td_client_close(TBoxTdClient *c);

#endif /* TBOX_TD_CLIENT_H */

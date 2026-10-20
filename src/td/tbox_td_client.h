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

/* Lifecycle: open -> on_update (optional) -> start -> ... -> close.
 *
 * No receive thread exists before start, so no update can arrive before a
 * handler is registered (tdjson queues events until receive() pulls them).
 * Starting the thread inside open() caused a real hang: tdlib's first
 * cue arrived into the window before the handler existed and was lost. */
TBoxTdClient *tbox_td_client_open(const TBoxTdApi *api, int verbosity);

void tbox_td_client_on_update(TBoxTdClient *c, TBoxTdUpdateFn handler, void *user_data);

/* Begins receiving; call after on_update. 0 = started, -1 = thread failed. */
int tbox_td_client_start(TBoxTdClient *c);

/* Fire-and-forget: td_send without waiting for a reply. */
void tbox_td_client_send(TBoxTdClient *c, const char *request_json);

/* Returns the OLDEST unread reply (any event carrying @extra) as a
 * malloc'd JSON string, or NULL when none are pending; caller frees.
 * Replies queue FIFO (4 deep); if a caller truly never reads, the fifth
 * reply drops the oldest rather than blocking the receive thread. */
char *tbox_td_client_take_reply(TBoxTdClient *c);

/* Stops the receive thread (wakes within its 1 s poll) and frees the client. */
void tbox_td_client_close(TBoxTdClient *c);

#endif /* TBOX_TD_CLIENT_H */

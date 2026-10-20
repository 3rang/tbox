/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_session.h — the UI-free engine: commands in, state events out.
 *
 * This layer owns everything that must happen for a session to work
 * (directories, tdlib parameters, the auth FSM, request/reply plumbing)
 * and owns NOTHING that a user sees: no printing, no prompting, no QR
 * drawing. A front end (terminal UI, GUI, JSON pipe) reads events and
 * decides what to show and when to give up.
 *
 * Calling model: the caller polls tbox_session_take_event(), which blocks
 * the CALLER only, with a timeout. So a front can call it straight from a
 * worker thread ("threaded calling") or in a loop ("blocking calling").
 *
 * Threading: tdlib's receive thread pushes events; the caller thread pops
 * them. Both go through the session lock, so no event is ever lost to a
 * race, and replies are drained on the caller side.
 */
#ifndef TBOX_SESSION_H
#define TBOX_SESSION_H

#include "td/tbox_td_api.h"

typedef enum TBoxSessionEvent {
    TBOX_SESSION_EV_AUTH_STATE = 0, /* data = FSM state name, e.g. "WAIT_QR" */
    TBOX_SESSION_EV_QR_LINK,        /* data = tg:// link; a new one per rotation */
    TBOX_SESSION_EV_PASSWORD_NEEDED,/* data = password hint (may be empty) */
    TBOX_SESSION_EV_READY,          /* data = account display name */
    TBOX_SESSION_EV_CONNECTION,     /* data = connection state name */
    TBOX_SESSION_EV_CLOSED,         /* tdlib confirmed the session is closed */
    TBOX_SESSION_EV_ERROR,          /* code + data = tdlib message */
} TBoxSessionEvent;

typedef struct TBoxSessionEventOut {
    TBoxSessionEvent type;
    int code;          /* tdlib error code, 0 otherwise */
    char data[1024];   /* meaning depends on type; always NUL-terminated */
} TBoxSessionEventOut;

typedef struct TBoxSession TBoxSession;

/* Creates <session_dir>/tdlib and <session_dir>/files, opens the td client,
 * registers the handler and starts the receive thread, then sends
 * setTdlibParameters eagerly (this tdlib build does not emit the opening
 * cue; the FSM row still catches a rejection).
 *
 * api is injected so tests run without the real dll. Returns NULL and
 * leaves nothing running if any step fails. */
TBoxSession *tbox_session_start(const TBoxTdApi *api, const char *session_dir,
                                long api_id, const char *api_hash);

/* Waits up to timeout_ms for the next event. Returns 1 and fills out, or 0
 * on timeout. Events come out in the order they happened; a burst longer
 * than the internal ring drops the oldest, never the newest. */
int tbox_session_take_event(TBoxSession *s, TBoxSessionEventOut *out,
                            unsigned timeout_ms);

/* Answer to PASSWORD_NEEDED. Callable any time after start. */
void tbox_session_submit_password(TBoxSession *s, const char *password);

/* Asks tdlib to close the session (valid once authorized); the CLOSED
 * event follows. Safe to call more than once. */
void tbox_session_request_close(TBoxSession *s);

/* Stops the receive thread and frees the session. Does not ask tdlib to
 * close - call tbox_session_request_close first for a clean exit. */
void tbox_session_close(TBoxSession *s);

#endif /* TBOX_SESSION_H */
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
 *
 * Event delivery (spec 6.6, fixes P4/P5): a payload is heap text with no
 * fixed size, the queue grows as far as memory allows (high-water cap
 * TBOX_SESSION_EVENTS_MAX, default 10,000), and a queue that still cannot
 * take an event never drops it silently - it sets the sticky
 * tbox_session_resync_needed() flag and emits one CATALOG_CHANGED event
 * with folder "*", which tells the front to reload what it shows and
 * re-run an incremental reconcile. Only PROGRESS events may ever be
 * dropped, and only as coalescing: a newer progress for the same op_id
 * replaces the queued older one.
 */
#ifndef TBOX_SESSION_H
#define TBOX_SESSION_H

#include "td/tbox_td_api.h"

/* Queue cap for non-progress events before the resync path (spec 6.6:
 * "default 10,000 queued items"). */
#define TBOX_SESSION_EVENTS_MAX 10000

typedef enum TBoxSessionEvent {
    TBOX_SESSION_EV_AUTH_STATE = 0, /* data = FSM state name, e.g. "WAIT_QR" */
    TBOX_SESSION_EV_QR_LINK,        /* data = tg:// link; a new one per rotation */
    TBOX_SESSION_EV_PASSWORD_NEEDED,/* data = password hint (may be empty) */
    TBOX_SESSION_EV_READY,          /* data = account display name */
    TBOX_SESSION_EV_CONNECTION,     /* data = connection state name */
    TBOX_SESSION_EV_CLOSED,         /* tdlib confirmed the session is closed */
    TBOX_SESSION_EV_ERROR,          /* code + data = tdlib message */
    TBOX_SESSION_EV_CATALOG_CHANGED,/* data = comma-separated folder ids, "*" = all */
    TBOX_SESSION_EV_TRANSFER_PROGRESS, /* data = op_id, code = done, total via total() */
    TBOX_SESSION_EV_TRANSFER_DONE,  /* data = op_id, total() = 1 if object_id is set */
    TBOX_SESSION_EV_TRANSFER_FAILED,/* data = op_id, total() = error code */
    TBOX_SESSION_EV_SYNC_PROGRESS,  /* data = op_id, code = messages_scanned */
    TBOX_SESSION_EV_SYNC_DONE,      /* data = op_id, code = objects, total() = inbox_items */
} TBoxSessionEvent;

typedef struct TBoxSessionEventOut {
    TBoxSessionEvent type;
    int code;      /* tdlib code, or the first counter of the event */
    long long total; /* second counter: progress total, done's object_id, ... */
    char *data;    /* heap text, NUL-terminated; owned by the caller, free()
                    * exactly like a malloc pointer. NULL if the event had
                    * none. */
} TBoxSessionEventOut;

typedef struct TBoxSession TBoxSession;

/* Creates <session_dir>/tdlib and <session_dir>/files, opens the td client,
 * registers the handler and starts the receive thread, then sends
 * setTdlibParameters eagerly (this tdlib build does not emit the opening
 * cue; the FSM row still catches a rejection).
 *
 * db_key is the TDLib database encryption key (S5), base64, or NULL/empty
 * for an unencrypted session; it is a secret and is never logged.
 *
 * api is injected so tests run without the real dll. Returns NULL and
 * leaves nothing running if any step fails. */
TBoxSession *tbox_session_start(const TBoxTdApi *api, const char *session_dir,
                                long api_id, const char *api_hash,
                                const char *db_key);

/* Waits up to timeout_ms for the next event. Returns 1 and fills out, or 0
 * on timeout. Events come out in the order they happened. out->data is
 * owned by the caller and must be free()d (NULL when the event carries no
 * text). Under memory pressure events are never quietly lost: the resync
 * flags are set instead - see the header comment and spec 6.6. */
int tbox_session_take_event(TBoxSession *s, TBoxSessionEventOut *out,
                            unsigned timeout_ms);

/* Set when the queue overflowed and the engine must re-run an incremental
 * reconcile (spec 6.6); sticky for the life of the session. */
int tbox_session_resync_needed(TBoxSession *s);

/* Set when a progress event was coalesced (a newer one replaced it).
 * Progress is advisory, so the front only needs to know it fell behind. */
int tbox_session_progress_dropped(TBoxSession *s);

/* Answer to PASSWORD_NEEDED. Callable any time after start. */
void tbox_session_submit_password(TBoxSession *s, const char *password);

/* S10: asks tdlib to log out (TDLib logOut). Nothing is deleted on
 * Telegram; auth.closed follows once tdlib finishes. Safe to call more
 * than once. */
void tbox_session_logout(TBoxSession *s);

/* Asks tdlib to close the session (valid once authorized); the CLOSED
 * event follows. Safe to call more than once. */
void tbox_session_request_close(TBoxSession *s);

/* Stops the receive thread and frees the session. Does not ask tdlib to
 * close - call tbox_session_request_close first for a clean exit. */
void tbox_session_close(TBoxSession *s);

#endif /* TBOX_SESSION_H */

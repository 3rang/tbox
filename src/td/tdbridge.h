/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TDBRIDGE_H
#define TDBRIDGE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * tdbridge - the one place TDLib is talked to.
 *
 * TDLib's JSON interface has one hard rule: only one thread may call
 * td_receive at a time (td_send is free from any thread). `tbox serve` needs
 * TDLib from several places at once - the scan loop, the FTP session that is
 * downloading a body right now, the rescan after a new message - so this is
 * the thread that owns td_receive and hands everything else a request/reply
 * API:
 *
 *   caller                          bridge thread
 *   ------                          -------------
 *   tbox_tdbridge_call()            td_receive()
 *     stamps @extra = "tbox:<n>"      reads the JSON
 *     td_send()                       looks the token up in the slot table
 *     polls its slot                  stores the reply there
 *     takes the reply                 wakes up
 *
 * The poll is ~2 ms per waiter and the bridge polls TDLib at 25 ms, so the
 * latency is invisible and no condition variable is needed on either OS.
 * 32 slots are enough because only a handful of requests are ever in flight
 * (at most one per FTP session, which is capped at 8).
 *
 * Anything TDLib pushes that is *not* a reply to one of our requests - an
 * updateNewMessage, updateFile, an option change - goes to the update handler
 * on the bridge thread. `json` is valid only for the duration of the call.
 */
typedef struct tbox_tdbridge tbox_tdbridge_t;

/* Called on the bridge thread for every update that is not one of our replies. */
typedef void (*tbox_tdb_update_fn)(void *user, const char *json);

/* tbox_tdbridge_call() results. A TDLib "error" object is NOT one of these:
 * the reply is handed back and the caller reads @type itself. */
#define TBOX_TDB_OK       0
#define TBOX_TDB_ERROR   (-1)   /* bad arguments, or no free slot */
#define TBOX_TDB_TIMEOUT (-2)   /* no reply within the deadline */
#define TBOX_TDB_STOPPED (-3)   /* the bridge is shutting down */

/*
 * Create the TDLib client, start the receive thread and silence TDLib's own
 * log chatter. The caller then sends setTdlibParameters (td/tdparams.c) and
 * waits for authorizationStateReady. Returns NULL on failure.
 */
tbox_tdbridge_t *tbox_tdbridge_start(void);

/*
 * Stop the bridge and release it. The caller must have finished every
 * outstanding call: this frees the whole object, so a caller still polling a
 * slot would be reading freed memory.
 *
 * The caller is responsible for sending `close` and draining first (tdserve
 * does both) - TDLib wants a closed client, not a dropped one.
 */
void tbox_tdbridge_stop(tbox_tdbridge_t *bridge);

/* Install the update handler. Pass NULL to remove it. Not safe to call while a
 * scan is running; set it once, right after start. */
void tbox_tdbridge_on_update(tbox_tdbridge_t *bridge, tbox_tdb_update_fn fn,
                             void *user);

/*
 * Send a request without waiting for a reply (close, setOption, and other
 * fire-and-forget calls). Returns 0, or -1 for a NULL bridge/request.
 */
int tbox_tdbridge_send(tbox_tdbridge_t *bridge, const char *request);

/*
 * Send a request and wait for its reply.
 *
 * The bridge adds "@extra" itself, so the request must not carry one. On
 * TBOX_TDB_OK, *reply holds a malloc'd copy of the raw reply JSON that the
 * caller frees; on any error *reply is NULL. `timeout_ms` <= 0 means "wait
 * as long as it takes", which is right for the first authorization round trip.
 */
int tbox_tdbridge_call(tbox_tdbridge_t *bridge, const char *request,
                       long timeout_ms, char **reply);

/* 1 when the bridge is shutting down, so callers can give up early. */
int tbox_tdbridge_stopping(const tbox_tdbridge_t *bridge);

/*
 * Copy a TDLib error object's message into `out` ("" when there is none).
 * Returns -1 when `reply` IS an error, 0 when it is not - so the caller can
 * report TDLib's own words without another parse:
 *
 *   if (tbox_td_reply_error(reply, buf, sizeof buf) != 0)
 *       fprintf(stderr, "TDLib: %s\n", buf);
 */
int tbox_td_reply_error(const char *reply, char *out, size_t size);

/*
 * Download progress, as TDLib reports it in updateFile.
 *
 * The cache asks for one body per FTP session, so the bridge keeps a small
 * fixed table: the fetcher sends downloadFile, then polls its file_id until
 * `completed` or `failed` turns true, then copies TDLib's local file into the
 * cache. Nothing here owns a TDLib path - it is only valid while TDLib still
 * has the file.
 */
typedef struct
{
    int downloading;      /* local.is_downloading_active */
    int completed;        /* local.is_downloading_completed */
    /* Started and then went quiet without finishing. TDLib sends no error for
     * a download it drops, so this is read off the state rather than a reply. */
    int failed;
    char path[600];       /* TDLib's local path, valid only while completed */

} tbox_td_file_t;

/*
 * Fill `out` with what TDLib last said about `file_id`. Returns 0 when
 * something is known, -1 when TDLib has not mentioned it (yet) - which is
 * not an error: a download that was just requested is still silent.
 *
 * `out` is zeroed first, so an unknown file reads as "nothing happening".
 */
int tbox_tdbridge_file(tbox_tdbridge_t *bridge, long long file_id,
                       tbox_td_file_t *out);

/*
 * Reserve a place in the progress table for `file_id`. Call this *before* the
 * downloadFile request, so an updateFile that arrives before the reply is
 * still recorded against the download it belongs to.
 *
 * A second caller asking for the same file joins the download already in
 * flight instead of resetting it, so two sessions can fetch one body at the
 * same time. Returns -1 when the progress table is full (16 slots against a
 * cap of 8 sessions), which the caller should report rather than retry.
 */
int tbox_tdbridge_file_want(tbox_tdbridge_t *bridge, long long file_id);

/* Forget `file_id`, so a later download of the same file starts clean. */
void tbox_tdbridge_file_forget(tbox_tdbridge_t *bridge, long long file_id);

#ifdef __cplusplus
}
#endif

#endif /* TDBRIDGE_H */

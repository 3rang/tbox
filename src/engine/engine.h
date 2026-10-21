/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * engine.h — the engine loop: commands in, responses and events out.
 *
 * One thread owns all state (spec 3.1 rule 2). Other threads (the TDLib
 * receive thread, the stdin reader) only push items into queues:
 *
 *   stdin reader  --engine_push_command()-->  command queue
 *   TDLib thread  --tbox_session_take_event()--> session queue
 *                                   |
 *                                   v
 *                          engine_run() (this thread)
 *                                   |
 *                         emit callback (transport)
 *
 * The transport is pluggable (spec 3.1 rule 3): the engine NEVER calls
 * printf or fgets. It is handed an emit callback at creation and the
 * stdio transport is a thin wrapper around it.
 */
#ifndef TBOX_ENGINE_H
#define TBOX_ENGINE_H

#include <stddef.h>

#include "td/tbox_td_api.h"

/* What the engine needs to start. api_hash and db_key are secrets: they
 * arrive here only from a protocol message (S2) and are never logged. */
typedef struct TBoxEngineConfig {
    const char *profile_dir; /* session directory (spec 4) */
    const char *api_hash;    /* borrowed; only used for auth.start */
    const char *db_key;      /* base64 TDLib DB key, or NULL (S5) */
} TBoxEngineConfig;

typedef struct TBoxEngine TBoxEngine;

/* Called by the engine loop for every response/event line. The line is
 * heap text owned by the caller for the duration of the call; the transport
 * must copy what it keeps. */
typedef void (*TBoxEngineEmitFn)(const char *json_line, void *ctx);

/* Create the engine. Does the spec 8.1 startup steps up to "start the
 * loop": creates logs/, opens the log, acquires engine.lock, and opens the
 * db. Returns NULL with *err set when a step fails so the caller can exit
 * with the right code:
 *   2 profile busy, 4 db error, 1 other.
 * The caller owns the lock and log through the returned engine.
 *
 * api is injected so tests run against the fake TDLib (spec 0.6). */
TBoxEngine *tbox_engine_new(const TBoxTdApi *api, const TBoxEngineConfig *cfg,
                            TBoxEngineEmitFn emit, void *emit_ctx,
                            int *err);

/* Thread-safe: enqueue one request line (already NUL-terminated). The line
 * is copied. Returns 1 if queued, 0 if the engine is shutting down. */
int tbox_engine_push_command(TBoxEngine *e, const char *json);

/* Tell the loop no more commands will arrive (stdin EOF). The loop drains
 * what is queued, then shuts down cleanly. */
void tbox_engine_signal_input_closed(TBoxEngine *e);

/* Run the loop until shutdown. Returns the process exit code (spec 6.1:
 * 0 clean shutdown, 1 other). */
int tbox_engine_run(TBoxEngine *e);

/* Release everything: session, db, log, lock. Safe after run(). */
void tbox_engine_free(TBoxEngine *e);

#endif /* TBOX_ENGINE_H */

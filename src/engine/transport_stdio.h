/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * transport_stdio.h — the desktop transport: stdin lines in, stdout
 * lines out (spec 6.1).
 *
 * A thin wrapper around the pluggable engine seam (spec 3.1 rule 3). It
 * owns the stdin reader thread and the stdout writer lock; the engine loop
 * never touches a FILE*. stdin = requests, stdout = responses and events
 * only, stderr = fatal startup errors only.
 */
#ifndef TBOX_TRANSPORT_STDIO_H
#define TBOX_TRANSPORT_STDIO_H

#include "engine/engine.h"

typedef struct TBoxStdioTransport TBoxStdioTransport;

/* Allocate the writer state. Pass the emit callback and the returned
 * pointer to tbox_engine_new(). */
TBoxStdioTransport *tbox_transport_stdio_new(void);

/* The engine's emit callback (json_line, transport). */
void tbox_transport_stdio_emit(const char *json_line, void *ctx);

/* Start the stdin reader thread: reads lines, pushes them to the engine,
 * and signals input-closed on EOF. Returns 0 on success, -1 if the thread
 * could not be created. */
int tbox_transport_stdio_start_reader(TBoxStdioTransport *t, TBoxEngine *e);

/* Stop and free. The reader thread is detached and dies with the process. */
void tbox_transport_stdio_free(TBoxStdioTransport *t);

#endif /* TBOX_TRANSPORT_STDIO_H */

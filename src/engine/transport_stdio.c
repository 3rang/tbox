/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * transport_stdio.c — the desktop transport (spec 6.1, spec 3.1 rule 3).
 *
 * The ONLY place in the engine that calls fgets/printf. The reader runs on
 * its own thread so a blocked stdin never stops the engine loop from
 * delivering events; the writer takes a lock so two threads can never
 * interleave half a line.
 *
 * Line length cap (spec 6.1): a line longer than TBOX_PROTOCOL_MAX_LINE is
 * refused with PROTOCOL_ERROR and discarded. Because the engine answers
 * malformed lines by id, the transport hands over an over-long line as a
 * synthetic PROTOCOL_ERROR request only when it can still see the id; lines
 * with no parseable id are dropped (they cannot be answered).
 */
#include "engine/transport_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/protocol.h"
#include "util/log.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

struct TBoxStdioTransport {
    TBoxEngine *engine; /* borrowed; set before the reader starts */
#ifdef _WIN32
    CRITICAL_SECTION lock_out;
    HANDLE reader;
#else
    pthread_mutex_t lock_out;
    pthread_t reader;
#endif
};

TBoxStdioTransport *tbox_transport_stdio_new(void) {
    TBoxStdioTransport *t = calloc(1, sizeof *t);
    if (t == NULL) {
        return NULL;
    }
#ifdef _WIN32
    InitializeCriticalSection(&t->lock_out);
#else
    pthread_mutex_init(&t->lock_out, NULL);
#endif
    return t;
}

void tbox_transport_stdio_emit(const char *json_line, void *ctx) {
    TBoxStdioTransport *t = ctx;
    if (t == NULL || json_line == NULL) {
        return;
    }
#ifdef _WIN32
    EnterCriticalSection(&t->lock_out);
#else
    pthread_mutex_lock(&t->lock_out);
#endif
    fputs(json_line, stdout);
    fputc('\n', stdout);
    fflush(stdout); /* one complete line, immediately: events are live */
#ifdef _WIN32
    LeaveCriticalSection(&t->lock_out);
#else
    pthread_mutex_unlock(&t->lock_out);
#endif
}

/* Reads one line of any length from stdin into a growable buffer. Returns
 * the length, or -1 on EOF with nothing read. The buffer is reused; *cap
 * tracks its size. */
static long read_line(char **buf, size_t *cap) {
    size_t len = 0;
    int c;
    for (;;) {
        c = fgetc(stdin);
        if (c == EOF) {
            return (len == 0) ? -1 : (long)len;
        }
        if (len + 1 >= *cap) {
            size_t ncap = *cap ? *cap * 2 : 1024;
            char *n = realloc(*buf, ncap);
            if (n == NULL) {
                return -1;
            }
            *buf = n;
            *cap = ncap;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue; /* accept CRLF from Windows pipes */
        }
        (*buf)[len++] = (char)c;
    }
    (*buf)[len] = '\0';
    return (long)len;
}

static void reader_main(void *arg) {
    TBoxStdioTransport *t = arg;
    TBoxEngine *e = t->engine;
    char *buf = NULL;
    size_t cap = 0;
    for (;;) {
        long n = read_line(&buf, &cap);
        if (n < 0) {
            break; /* EOF */
        }
        if ((size_t)n > TBOX_PROTOCOL_MAX_LINE) {
            /* spec 6.1: too long -> refuse and discard. We cannot always
             * see the id, so a plain broken-pipe-safe refusal is enough:
             * emit a PROTOCOL_ERROR with id 0 only if an id is present. */
            const char *idp = strstr(buf, "\"id\"");
            if (idp != NULL) {
                char *end = NULL;
                long long id = strtoll(idp + 4, &end, 10);
                char *line = tbox_proto_response_err(id, TBOX_PROTO_ERR_PROTOCOL,
                                                     "request line too long");
                tbox_transport_stdio_emit(line, t);
                free(line);
            }
            continue;
        }
        if (!tbox_engine_push_command(e, buf)) {
            break; /* engine shutting down */
        }
    }
    free(buf);
    tbox_engine_signal_input_closed(e);
}

int tbox_transport_stdio_start_reader(TBoxStdioTransport *t, TBoxEngine *e) {
    if (t == NULL || e == NULL) {
        return -1;
    }
    t->engine = e;
#ifdef _WIN32
    uintptr_t h = _beginthread(reader_main, 0, t);
    if (h == (uintptr_t)-1) {
        return -1;
    }
    t->reader = (HANDLE)h;
#else
    if (pthread_create(&t->reader, NULL, (void *(*)(void *))reader_main, t) != 0) {
        return -1;
    }
    pthread_detach(t->reader);
#endif
    return 0;
}

void tbox_transport_stdio_free(TBoxStdioTransport *t) {
    if (t == NULL) {
        return;
    }
#ifdef _WIN32
    DeleteCriticalSection(&t->lock_out);
#else
    pthread_mutex_destroy(&t->lock_out);
#endif
    free(t);
}

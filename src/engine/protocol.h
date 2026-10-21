/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * protocol.h — engine protocol v1 as data: parse one request line, build
 * one response or event line (spec 6).
 *
 * Everything here is pure text-in/text-out so it can be unit-tested with
 * no engine, no TDLib and no sockets. The engine loop owns the state; this
 * module only knows the wire shapes:
 *
 *   request   {"id":7,"cmd":"catalog.list","args":{...}}
 *   response  {"id":7,"ok":true,"result":{...}}
 *             {"id":7,"ok":false,"error":{"code":"...","message":"...",
 *              "retryable":false}}
 *   event     {"event":"auth.state","data":{...}}
 */
#ifndef TBOX_ENGINE_PROTOCOL_H
#define TBOX_ENGINE_PROTOCOL_H

#include <stddef.h>

#include "cJSON.h"

/* Spec 6.1: one JSON object per line, max 1 MiB. Longer lines are refused
 * with PROTOCOL_ERROR and discarded. */
#define TBOX_PROTOCOL_MAX_LINE (1024 * 1024)

/* Spec 6.3 error codes, as wire strings. Keep this list the single source
 * of truth: tbox_proto_error_code() maps the enum to it. */
typedef enum TBoxProtoError {
    TBOX_PROTO_OK = 0,
    TBOX_PROTO_ERR_PROTOCOL,       /* malformed JSON / missing id or cmd */
    TBOX_PROTO_ERR_UNKNOWN_CMD,    /* cmd not in the dispatch table */
    TBOX_PROTO_ERR_BAD_ARGS,       /* cmd present, args wrong */
    TBOX_PROTO_ERR_NOT_AUTHORIZED,
    TBOX_PROTO_ERR_PROFILE_BUSY,
    TBOX_PROTO_ERR_NOT_FOUND,
    TBOX_PROTO_ERR_NAME_EXISTS,
    TBOX_PROTO_ERR_NAME_INVALID,
    TBOX_PROTO_ERR_CONFLICT,
    TBOX_PROTO_ERR_NOT_EMPTY,
    TBOX_PROTO_ERR_FILE_TOO_LARGE,
    TBOX_PROTO_ERR_IO,
    TBOX_PROTO_ERR_DB,
    TBOX_PROTO_ERR_TD,
    TBOX_PROTO_ERR_FLOOD_WAIT,
    TBOX_PROTO_ERR_CANCELLED,
    TBOX_PROTO_ERR_CONFIRM_REQUIRED,
    TBOX_PROTO_ERR_INTERNAL
} TBoxProtoError;

/* The wire string for an error code ("PROTOCOL_ERROR", ...). */
const char *tbox_proto_error_code(TBoxProtoError code);

/* True when a client should reconnect/retry, mirroring the "retryable"
 * flag in the spec 6.2 error shape. */
int tbox_proto_error_retryable(TBoxProtoError code);

/* One parsed request. The cJSON tree owns cmd/args; it is freed with
 * tbox_proto_request_clear(). id is 0 and cmd NULL for a malformed line. */
typedef struct TBoxProtoRequest {
    cJSON *root;  /* the whole request object, or NULL if parse failed */
    long long id; /* required integer id, unique per engine run */
    int has_id;
    const char *cmd;  /* borrowed from root */
    const cJSON *args; /* borrowed from root; NULL when absent */
    TBoxProtoError err; /* non-OK when the line could not be used */
} TBoxProtoRequest;

/* Parses one line. Never fails hard: a bad line yields err != OK (usually
 * PROTOCOL_ERROR) and a request whose root is NULL. The line need not be
 * NUL-terminated; len is authoritative. Returns 0 always (kept as a status
 * for callers that prefer it). */
int tbox_proto_parse(const char *line, size_t len, TBoxProtoRequest *out);
void tbox_proto_request_clear(TBoxProtoRequest *req);

/* Reads an args field by name. Helpers return "present and the right type"
 * so a command body can validate args without writing cJSON by hand. */
const char *tbox_proto_arg_str(const cJSON *args, const char *name);
int tbox_proto_arg_long(const cJSON *args, const char *name, long long *out);
int tbox_proto_arg_bool(const cJSON *args, const char *name, int *out);

/* Build a success/error response line for request id. Returns a heap
 * string the caller frees; result may be NULL (treated as {}). */
char *tbox_proto_response_ok(long long id, const cJSON *result);
char *tbox_proto_response_err(long long id, TBoxProtoError code,
                              const char *message);

/* Build an event line. data may be NULL (treated as {}). */
char *tbox_proto_event(const char *event, const cJSON *data);

#endif /* TBOX_ENGINE_PROTOCOL_H */

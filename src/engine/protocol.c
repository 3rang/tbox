/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * protocol.c — request parsing, response/event building (spec 6).
 *
 * The engine trusts nothing on stdin: a line is bounded, parsed with
 * cJSON's own length limits, and every field is type-checked before a
 * command body sees it. Bad input becomes a typed error, never a crash.
 */
#include "engine/protocol.h"

#include <stdlib.h>
#include <string.h>

/* Wire strings, indexed by TBoxProtoError. Order MUST match the enum. */
static const char *const ERROR_CODES[] = {
    "OK",
    "PROTOCOL_ERROR",
    "UNKNOWN_COMMAND",
    "BAD_ARGS",
    "NOT_AUTHORIZED",
    "PROFILE_BUSY",
    "NOT_FOUND",
    "NAME_EXISTS",
    "NAME_INVALID",
    "CONFLICT",
    "NOT_EMPTY",
    "FILE_TOO_LARGE",
    "IO_ERROR",
    "DB_ERROR",
    "TD_ERROR",
    "FLOOD_WAIT",
    "CANCELLED",
    "CONFIRM_REQUIRED",
    "INTERNAL",
};

const char *tbox_proto_error_code(TBoxProtoError code) {
    size_t n = sizeof(ERROR_CODES) / sizeof(ERROR_CODES[0]);
    return ((size_t)code < n) ? ERROR_CODES[code] : "INTERNAL";
}

int tbox_proto_error_retryable(TBoxProtoError code) {
    switch (code) {
    case TBOX_PROTO_ERR_FLOOD_WAIT:
    case TBOX_PROTO_ERR_TD:
    case TBOX_PROTO_ERR_IO:
    case TBOX_PROTO_ERR_INTERNAL:
        return 1;
    default:
        return 0;
    }
}

int tbox_proto_parse(const char *line, size_t len, TBoxProtoRequest *out) {
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->err = TBOX_PROTO_ERR_PROTOCOL;

    if (line == NULL || len == 0) {
        return 0;
    }
    if (len > TBOX_PROTOCOL_MAX_LINE) {
        return 0; /* spec 6.1: too long -> refuse and discard */
    }

    /* cJSON wants a NUL-terminated string; copy exactly the bytes we were
     * given so an embedded NUL cannot smuggle a second document. */
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        out->err = TBOX_PROTO_ERR_INTERNAL;
        return 0;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(copy, len);
    free(copy);
    if (root == NULL) {
        return 0; /* malformed JSON -> PROTOCOL_ERROR */
    }
    out->root = root;

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id)) {
        out->id = (long long)id->valuedouble;
        out->has_id = 1;
    }

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        if (!out->has_id) {
            return 0; /* no usable request at all */
        }
        out->err = TBOX_PROTO_ERR_BAD_ARGS;
        return 0;
    }
    out->cmd = cJSON_GetStringValue(cmd);

    if (!out->has_id) {
        /* valid JSON with a cmd but no id: cannot answer it */
        out->err = TBOX_PROTO_ERR_PROTOCOL;
        return 0;
    }

    const cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
    out->args = cJSON_IsObject(args) ? args : NULL;
    out->err = TBOX_PROTO_OK;
    return 0;
}

void tbox_proto_request_clear(TBoxProtoRequest *req) {
    if (req == NULL) {
        return;
    }
    cJSON_Delete(req->root);
    memset(req, 0, sizeof *req);
}

const char *tbox_proto_arg_str(const cJSON *args, const char *name) {
    if (args == NULL || name == NULL) {
        return NULL;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    return cJSON_IsString(v) ? cJSON_GetStringValue(v) : NULL;
}

int tbox_proto_arg_long(const cJSON *args, const char *name, long long *out) {
    if (args == NULL || name == NULL || out == NULL) {
        return 0;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    if (cJSON_IsNumber(v)) {
        *out = (long long)v->valuedouble;
        return 1;
    }
    if (cJSON_IsString(v)) {
        /* ids travel as strings (spec 6.1); accept a decimal string here */
        char *end = NULL;
        long long n = strtoll(cJSON_GetStringValue(v), &end, 10);
        if (end != NULL && *end == '\0') {
            *out = n;
            return 1;
        }
    }
    return 0;
}

int tbox_proto_arg_bool(const cJSON *args, const char *name, int *out) {
    if (args == NULL || name == NULL || out == NULL) {
        return 0;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v) ? 1 : 0;
        return 1;
    }
    return 0;
}

char *tbox_proto_response_ok(long long id, const cJSON *result) {
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddBoolToObject(o, "ok", 1);
    if (result != NULL) {
        cJSON_AddItemToObject(o, "result", cJSON_Duplicate(result, 1));
    } else {
        cJSON_AddItemToObject(o, "result", cJSON_CreateObject());
    }
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return line;
}

char *tbox_proto_response_err(long long id, TBoxProtoError code,
                              const char *message) {
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "code", tbox_proto_error_code(code));
    cJSON_AddStringToObject(e, "message", message != NULL ? message : "");
    cJSON_AddBoolToObject(e, "retryable", tbox_proto_error_retryable(code));
    cJSON_AddItemToObject(o, "error", e);
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return line;
}

char *tbox_proto_response_err_wait(long long id, const char *message,
                                   long long retry_after) {
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "code", tbox_proto_error_code(TBOX_PROTO_ERR_FLOOD_WAIT));
    cJSON_AddStringToObject(e, "message", message != NULL ? message : "");
    cJSON_AddBoolToObject(e, "retryable", 1);
    cJSON_AddNumberToObject(e, "retry_after", (double)retry_after);
    cJSON_AddItemToObject(o, "error", e);
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return line;
}

char *tbox_proto_event(const char *event, const cJSON *data) {
    if (event == NULL) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "event", event);
    if (data != NULL) {
        cJSON_AddItemToObject(o, "data", cJSON_Duplicate(data, 1));
    } else {
        cJSON_AddItemToObject(o, "data", cJSON_CreateObject());
    }
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return line;
}

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_protocol.c — the engine protocol as pure text->text (spec 6).
 * No engine, no TDLib: malformed input, error codes, response and event
 * shapes.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "engine/protocol.h"

static void test_error_codes(void) {
    /* spec 6.3 list is fixed; the enum maps to it exactly */
    assert(strcmp(tbox_proto_error_code(TBOX_PROTO_ERR_PROTOCOL),
                  "PROTOCOL_ERROR") == 0);
    assert(strcmp(tbox_proto_error_code(TBOX_PROTO_ERR_UNKNOWN_CMD),
                  "UNKNOWN_COMMAND") == 0);
    assert(strcmp(tbox_proto_error_code(TBOX_PROTO_ERR_PROFILE_BUSY),
                  "PROFILE_BUSY") == 0);
    assert(strcmp(tbox_proto_error_code(TBOX_PROTO_ERR_FLOOD_WAIT),
                  "FLOOD_WAIT") == 0);
    assert(strcmp(tbox_proto_error_code(TBOX_PROTO_ERR_CONFIRM_REQUIRED),
                  "CONFIRM_REQUIRED") == 0);
    /* retryable mirrors the spec 6.2 flag */
    assert(tbox_proto_error_retryable(TBOX_PROTO_ERR_FLOOD_WAIT) == 1);
    assert(tbox_proto_error_retryable(TBOX_PROTO_ERR_NOT_FOUND) == 0);
}

static void test_parse_good(void) {
    const char *line = "{\"id\":7,\"cmd\":\"catalog.list\","
                       "\"args\":{\"folder_id\":\"root\"}}";
    TBoxProtoRequest req;
    tbox_proto_parse(line, strlen(line), &req);
    assert(req.err == TBOX_PROTO_OK);
    assert(req.has_id && req.id == 7);
    assert(strcmp(req.cmd, "catalog.list") == 0);
    assert(strcmp(tbox_proto_arg_str(req.args, "folder_id"), "root") == 0);
    assert(tbox_proto_arg_str(req.args, "nope") == NULL);
    tbox_proto_request_clear(&req);

    /* args optional */
    const char *plain = "{\"id\":1,\"cmd\":\"engine.status\"}";
    tbox_proto_parse(plain, strlen(plain), &req);
    assert(req.err == TBOX_PROTO_OK);
    assert(req.id == 1 && req.args == NULL);
    tbox_proto_request_clear(&req);

    /* embedded NUL in the line must not hide a second document */
    const char nul[] = "{\"id\":2,\"cmd\":\"hello\"}\0{\"id\":3}";
    tbox_proto_parse(nul, sizeof nul - 1, &req);
    assert(req.err == TBOX_PROTO_OK);
    assert(req.id == 2 && strcmp(req.cmd, "hello") == 0);
    tbox_proto_request_clear(&req);
}

static void test_parse_bad(void) {
    TBoxProtoRequest req;

    tbox_proto_parse("not json", 8, &req);
    assert(req.err == TBOX_PROTO_ERR_PROTOCOL && req.root == NULL);

    tbox_proto_parse("{}", 2, &req);
    assert(req.err == TBOX_PROTO_ERR_PROTOCOL);

    /* cmd without id: cannot be answered */
    const char *noid = "{\"cmd\":\"hello\"}";
    tbox_proto_parse(noid, strlen(noid), &req);
    assert(req.err == TBOX_PROTO_ERR_PROTOCOL);

    /* id with no cmd */
    const char *nocmd = "{\"id\":9}";
    tbox_proto_parse(nocmd, strlen(nocmd), &req);
    assert(req.err == TBOX_PROTO_ERR_BAD_ARGS && req.id == 9);
    tbox_proto_request_clear(&req);

    /* empty line */
    tbox_proto_parse("", 0, &req);
    assert(req.err == TBOX_PROTO_ERR_PROTOCOL);
}

static void test_arg_types(void) {
    cJSON *args = cJSON_Parse("{\"n\":5,\"s\":\"42\",\"b\":true,\"t\":\"x\"}");
    long long n = 0;
    assert(tbox_proto_arg_long(args, "n", &n) && n == 5);
    assert(tbox_proto_arg_long(args, "s", &n) && n == 42); /* ids as strings */
    int b = 0;
    assert(tbox_proto_arg_bool(args, "b", &b) && b == 1);
    assert(!tbox_proto_arg_long(args, "t", &n)); /* not numeric */
    assert(!tbox_proto_arg_bool(args, "n", &b)); /* not bool */
    cJSON_Delete(args);
}

static void test_response_shapes(void) {
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "engine_version", "0.1.0-dev");
    char *ok = tbox_proto_response_ok(7, res);
    assert(ok != NULL);
    cJSON *o = cJSON_Parse(ok);
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(o, "id")));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "ok")));
    assert(cJSON_GetObjectItemCaseSensitive(o, "result") != NULL);
    cJSON_Delete(o);
    free(ok);
    cJSON_Delete(res);

    char *err = tbox_proto_response_err(8, TBOX_PROTO_ERR_NOT_FOUND, "gone");
    cJSON *eo = cJSON_Parse(err);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(eo, "ok")));
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(eo, "error");
    assert(strcmp(cJSON_GetStringValue(
                      cJSON_GetObjectItemCaseSensitive(e, "code")),
                  "NOT_FOUND") == 0);
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(e, "retryable")));
    cJSON_Delete(eo);
    free(err);

    /* NULL result -> {} */
    char *empty = tbox_proto_response_ok(1, NULL);
    cJSON *eo2 = cJSON_Parse(empty);
    assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(eo2, "result")));
    cJSON_Delete(eo2);
    free(empty);
}

static void test_event_shape(void) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "state", "WAIT_QR");
    char *line = tbox_proto_event("auth.state", d);
    cJSON *o = cJSON_Parse(line);
    assert(strcmp(cJSON_GetStringValue(
                      cJSON_GetObjectItemCaseSensitive(o, "event")),
                  "auth.state") == 0);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(o, "data");
    assert(strcmp(cJSON_GetStringValue(
                      cJSON_GetObjectItemCaseSensitive(data, "state")),
                  "WAIT_QR") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(o, "id") == NULL); /* events have no id */
    cJSON_Delete(o);
    free(line);
    cJSON_Delete(d);
}

int main(void) {
    test_error_codes();
    test_parse_good();
    test_parse_bad();
    test_arg_types();
    test_response_shapes();
    test_event_shape();
    printf("protocol OK\n");
    return 0;
}

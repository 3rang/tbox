/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "core/td.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TDLib's C API (td/telegram/td_json_client.h), resolved by the linker. */
int td_create_client_id(void);
void td_send(int client_id, const char *request);
const char *td_receive(double timeout);
const char *td_execute(const char *request);

const TdApi *td_real_api(void) {
    static const TdApi api = { td_create_client_id, td_send, td_receive, td_execute };
    return &api;
}

void td_init(Td *td, const TdApi *api) {
    memset(td, 0, sizeof *td);
    td->api = *api;
    td->debug = getenv("TBOX_DEBUG") != NULL;

    /* TDLib's own log: fatal errors only, or also warnings when debugging. */
    td->api.execute(td->debug ? "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":2}"
                              : "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":1}");
    td->client_id = td->api.create_client_id();
}

/* Echo a request to stderr with the secrets blanked out. */
static void debug_request(const cJSON *req) {
    cJSON *copy = cJSON_Duplicate(req, 1);
    const char *secrets[] = { "password", "api_hash" };
    for (size_t i = 0; i < sizeof secrets / sizeof secrets[0]; i++) {
        if (jobj(copy, secrets[i]) != NULL) {
            cJSON_ReplaceItemInObject(copy, secrets[i], cJSON_CreateString("***"));
        }
    }
    char *text = cJSON_PrintUnformatted(copy);
    fprintf(stderr, ">> %s\n", text);
    free(text);
    cJSON_Delete(copy);
}

void td_set_update_handler(Td *td, TdUpdateFn fn, void *user) {
    td->on_update = fn;
    td->on_update_user = user;
}

void td_post(Td *td, cJSON *req) {
    if (td->debug) {
        debug_request(req);
    }
    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (text != NULL) {
        td->api.send(td->client_id, text);
        free(text);
    }
}

cJSON *td_poll(Td *td, double timeout) {
    const char *text = td->api.receive(timeout);
    if (text == NULL) {
        return NULL;
    }
    if (td->debug) {
        fprintf(stderr, "<< %s\n", text);
    }
    cJSON *ev = cJSON_Parse(text);
    if (ev == NULL) {
        return NULL;
    }
    if (jis(ev, "updateAuthorizationState") &&
        jis(jobj(ev, "authorization_state"), "authorizationStateClosed")) {
        td->closed = 1;
    }
    return ev;
}

static void dispatch(Td *td, const cJSON *ev) {
    if (td->on_update != NULL) {
        td->on_update(td, ev, td->on_update_user);
    }
}

int td_pump(Td *td, double timeout) {
    cJSON *ev = td_poll(td, timeout);
    if (ev == NULL) {
        return 0;
    }
    dispatch(td, ev);
    cJSON_Delete(ev);
    return 1;
}

cJSON *td_request(Td *td, cJSON *req, double timeout) {
    char extra[32];
    snprintf(extra, sizeof extra, "tbox-%ld", ++td->seq);
    cJSON_AddStringToObject(req, "@extra", extra);
    td_post(td, req);

    /* Like every wait in tbox, the timeout is seconds of *silence*: a
     * stream of updates TDLib is busy delivering does not count. */
    for (double left = timeout; left > 0 && !td->closed;) {
        double slice = left < 1.0 ? left : 1.0;
        cJSON *ev = td_poll(td, slice);
        if (ev == NULL) {
            left -= slice;
            continue;
        }
        if (strcmp(jstr(ev, "@extra"), extra) == 0) {
            return ev;
        }
        dispatch(td, ev);
        cJSON_Delete(ev);
    }
    return NULL;
}

void td_close(Td *td) {
    if (td->closed) {
        return;
    }
    td_post(td, jreq("close"));
    /* Give up after ~10 s of silence; a busy stream does not count. */
    for (int idle = 0; idle < 50 && !td->closed;) {
        if (!td_pump(td, 0.2)) {
            idle++;
        }
    }
}

int td_is_error(const cJSON *reply) {
    return reply == NULL || jis(reply, "error");
}

const char *td_error_message(const cJSON *reply) {
    if (reply == NULL) {
        return "no reply from TDLib (timeout)";
    }
    return jstr(reply, "message");
}

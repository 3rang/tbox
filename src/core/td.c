/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "core/td.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/platform.h"

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

    const char *verbosity = getenv("TBOX_TD_VERBOSITY");
    char req[128];
    snprintf(req, sizeof req, "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":%d}",
             verbosity != NULL ? atoi(verbosity) : 1);
    td->api.execute(req);

    td->client_id = td->api.create_client_id();
}

void td_set_update_handler(Td *td, TdUpdateFn fn, void *user) {
    td->on_update = fn;
    td->on_update_user = user;
}

void td_post(Td *td, cJSON *req) {
    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (text == NULL) {
        return;
    }
    if (td->debug) {
        fprintf(stderr, ">> %s\n", text);
    }
    td->api.send(td->client_id, text);
    free(text);
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

    /* Poll in short slices until the deadline, but stop as soon as
     * the reply shows up. */
    double deadline = platform_now() + timeout;
    for (;;) {
        double left = deadline - platform_now();
        if (left <= 0) {
            break;
        }
        cJSON *ev = td_poll(td, left < 1.0 ? left : 1.0);
        if (ev == NULL) {
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

int td_error_code(const cJSON *reply) {
    return reply == NULL ? 0 : (int)jint(reply, "code");
}

const char *td_error_message(const cJSON *reply) {
    if (reply == NULL) {
        return "no reply from TDLib (timeout)";
    }
    return jstr(reply, "message");
}

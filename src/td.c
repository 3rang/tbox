/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "td.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

static int try_load(const char *path, TdApi *api, char *err, size_t errlen) {
    void *h = platform_dlopen(path, err, errlen);
    if (h == NULL) {
        return -1;
    }
    *(void **)&api->create_client_id = platform_dlsym(h, "td_create_client_id");
    *(void **)&api->send = platform_dlsym(h, "td_send");
    *(void **)&api->receive = platform_dlsym(h, "td_receive");
    *(void **)&api->execute = platform_dlsym(h, "td_execute");
    if (api->create_client_id == NULL || api->send == NULL ||
        api->receive == NULL || api->execute == NULL) {
        snprintf(err, errlen, "%s does not export the td_json_client API", path);
        return -1;
    }
    return 0;
}

int td_load(TdApi *api, char *err, size_t errlen) {
    char path[1024];
    char tried[2048] = "";

    const char *env = getenv("TBOX_TDJSON");
    if (env != NULL && env[0] != '\0') {
        if (try_load(env, api, err, errlen) == 0) {
            return 0;
        }
        snprintf(tried, sizeof tried, "\n  %s: %s", env, err);
    }

    if (platform_exe_dir(path, sizeof path) == 0) {
        size_t len = strlen(path);
        snprintf(path + len, sizeof path - len, "/%s", platform_tdjson_name());
        if (try_load(path, api, err, errlen) == 0) {
            return 0;
        }
        size_t tl = strlen(tried);
        snprintf(tried + tl, sizeof tried - tl, "\n  %s: %s", path, err);
    }

    if (try_load(platform_tdjson_name(), api, err, errlen) == 0) {
        return 0;
    }
    size_t tl = strlen(tried);
    snprintf(tried + tl, sizeof tried - tl, "\n  %s: %s", platform_tdjson_name(), err);

    snprintf(err, errlen,
             "cannot load TDLib (%s). Put it next to the tbox executable or set "
             "TBOX_TDJSON to its path. Tried:%s",
             platform_tdjson_name(), tried);
    return -1;
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

void td_send(Td *td, cJSON *req) {
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

cJSON *td_receive(Td *td, double timeout) {
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
    cJSON *ev = td_receive(td, timeout);
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
    td_send(td, req);

    /* Receive in short slices until the deadline, but stop as soon as
     * the reply shows up. */
    double deadline = platform_now() + timeout;
    for (;;) {
        double left = deadline - platform_now();
        if (left <= 0) {
            break;
        }
        cJSON *ev = td_receive(td, left < 1.0 ? left : 1.0);
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
    td_send(td, jreq("close"));
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

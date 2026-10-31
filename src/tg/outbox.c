/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "tg/outbox.h"

#include <stdio.h>
#include <string.h>

typedef struct Outbox {
    long long old_message_id;
    long long file_id;
    long long final_id;      /* set on success */
    int failed;
    char err[256];
    long long last_shown;    /* bytes reported in the last progress line */
} Outbox;

static void show_progress(Outbox *o, const cJSON *file) {
    long long total = jint(file, "expected_size");
    long long done = jint(jobj(file, "remote"), "uploaded_size");
    if (total <= 0 || done == o->last_shown) {
        return;
    }
    o->last_shown = done;
    fprintf(stderr, "\ruploading %lld / %lld bytes (%d%%)", done, total,
            (int)(done * 100 / total));
    if (done >= total) {
        fputc('\n', stderr);
    }
    fflush(stderr);
}

static void on_update(Td *td, const cJSON *ev, void *user) {
    (void)td;
    Outbox *o = (Outbox *)user;

    if (jis(ev, "updateMessageSendSucceeded")) {
        if (jint(ev, "old_message_id") == o->old_message_id) {
            o->final_id = jint(jobj(ev, "message"), "id");
        }
    } else if (jis(ev, "updateMessageSendFailed")) {
        if (jint(ev, "old_message_id") == o->old_message_id) {
            o->failed = 1;
            const char *msg = jstr(jobj(ev, "error"), "message");
            snprintf(o->err, sizeof o->err, "%s", msg[0] != '\0' ? msg : "send failed");
        }
    } else if (jis(ev, "updateFile") && o->file_id > 0) {
        const cJSON *file = jobj(ev, "file");
        if (jint(file, "id") == o->file_id) {
            show_progress(o, file);
        }
    }
}

long long outbox_send(Td *td, long long chat_id, cJSON *content, char *err, size_t errlen) {
    cJSON *req = jreq("sendMessage");
    cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
    cJSON_AddItemToObject(req, "input_message_content", content);

    cJSON *r = td_request(td, req, TD_TIMEOUT);
    if (td_is_error(r)) {
        snprintf(err, errlen, "sendMessage: %s", td_error_message(r));
        cJSON_Delete(r);
        return 0;
    }

    Outbox o;
    memset(&o, 0, sizeof o);
    o.old_message_id = jint(r, "id");
    /* Present for a document; 0 for text, which has nothing to upload. */
    o.file_id = jint(jobj(jobj(jobj(r, "content"), "document"), "document"), "id");
    cJSON_Delete(r);

    TdUpdateFn old_fn = td->on_update;
    void *old_user = td->on_update_user;
    td_set_update_handler(td, on_update, &o);

    /* Only silence counts against the timeout; progress updates do not. */
    for (double left = 120.0; left > 0 && o.final_id == 0 && !o.failed && !td->closed;) {
        if (!td_pump(td, 0.5)) {
            left -= 0.5;
        }
    }

    td_set_update_handler(td, old_fn, old_user);

    if (o.failed) {
        snprintf(err, errlen, "%s", o.err);
        return 0;
    }
    if (o.final_id == 0) {
        snprintf(err, errlen, "timed out waiting for Telegram to accept the message");
        return 0;
    }
    return o.final_id;
}

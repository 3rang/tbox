/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "tg/chat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int all_digits(const char *s) {
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
    }
    return 1;
}

static int ieq(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
    }
    return *a == *b;
}

int chat_ref_parse(const char *s, ChatRef *out) {
    memset(out, 0, sizeof *out);
    if (s == NULL || s[0] == '\0') {
        return -1;
    }
    if (ieq(s, "me") || ieq(s, "saved")) {
        out->kind = CHAT_REF_ME;
        return 0;
    }
    if (all_digits(s[0] == '-' ? s + 1 : s)) {
        out->kind = CHAT_REF_ID;
        out->id = strtoll(s, NULL, 10);
        return 0;
    }
    const char *prefixes[] = { "@", "https://t.me/", "http://t.me/", "t.me/" };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(s, prefixes[i], n) == 0) {
            out->kind = CHAT_REF_USERNAME;
            snprintf(out->text, sizeof out->text, "%s", s + n);
            size_t len = strlen(out->text);
            if (len > 0 && out->text[len - 1] == '/') {
                out->text[len - 1] = '\0';
            }
            return out->text[0] != '\0' ? 0 : -1;
        }
    }
    return -1;
}

const char *chat_username(const cJSON *obj) {
    const cJSON *active = jobj(jobj(obj, "usernames"), "active_usernames");
    const cJSON *first = cJSON_IsArray(active) ? cJSON_GetArrayItem(active, 0) : NULL;
    if (cJSON_IsString(first)) {
        return first->valuestring;
    }
    return jstr(obj, "username");
}

void chat_user_name(const cJSON *user, char *buf, size_t n) {
    const char *first = jstr(user, "first_name");
    const char *last = jstr(user, "last_name");
    if (last[0] != '\0') {
        snprintf(buf, n, "%s %s", first, last);
    } else {
        snprintf(buf, n, "%s", first);
    }
}

/* --- resolution ----------------------------------------------------------- */

/* {"@type": type, key: id} -> reply. Most of TDLib's lookups are this. */
static cJSON *request_by_id(Td *td, const char *type, const char *key, long long id) {
    cJSON *req = jreq(type);
    cJSON_AddNumberToObject(req, key, (double)id);
    return td_request(td, req, TD_TIMEOUT);
}

static long long reply_chat_id(cJSON *r, char *err, size_t errlen, const char *what) {
    long long id = 0;
    if (td_is_error(r)) {
        snprintf(err, errlen, "%s: %s", what, td_error_message(r));
    } else {
        id = jint(r, "id");
    }
    cJSON_Delete(r);
    return id;
}

static long long resolve_me(Td *td, char *err, size_t errlen) {
    cJSON *me = td_request(td, jreq("getMe"), TD_TIMEOUT);
    if (td_is_error(me)) {
        snprintf(err, errlen, "getMe: %s", td_error_message(me));
        cJSON_Delete(me);
        return 0;
    }
    long long my_id = jint(me, "id");
    cJSON_Delete(me);
    return reply_chat_id(request_by_id(td, "createPrivateChat", "user_id", my_id), err, errlen,
                         "Saved Messages");
}

static long long resolve_username(Td *td, const char *name, char *err, size_t errlen) {
    cJSON *req = jreq("searchPublicChat");
    cJSON_AddStringToObject(req, "username", name);
    return reply_chat_id(td_request(td, req, TD_TIMEOUT), err, errlen, name);
}

static long long resolve_id(Td *td, long long id, char *err, size_t errlen) {
    cJSON *r = request_by_id(td, "getChat", "chat_id", id);
    if (td_is_error(r)) {
        /* A fresh session knows no chats yet: load the list and retry once. */
        cJSON_Delete(r);
        cJSON_Delete(request_by_id(td, "loadChats", "limit", 200));
        r = request_by_id(td, "getChat", "chat_id", id);
    }
    int found = !td_is_error(r);
    cJSON_Delete(r);
    if (!found) {
        snprintf(err, errlen, "chat %lld not found (is it in your chat list?)", id);
    }
    return found ? id : 0;
}

long long chat_resolve(Td *td, const char *ref, char *err, size_t errlen) {
    ChatRef cr;
    if (chat_ref_parse(ref, &cr) != 0) {
        snprintf(err, errlen, "\"%s\" is not a chat: use me, @username or a chat id", ref);
        return 0;
    }
    switch (cr.kind) {
    case CHAT_REF_ME:
        return resolve_me(td, err, errlen);
    case CHAT_REF_USERNAME:
        return resolve_username(td, cr.text, err, errlen);
    case CHAT_REF_ID:
    default:
        return resolve_id(td, cr.id, err, errlen);
    }
}

/* --- names ---------------------------------------------------------------- */

void chat_title(Td *td, long long chat_id, char *buf, size_t n) {
    cJSON *r = request_by_id(td, "getChat", "chat_id", chat_id);
    snprintf(buf, n, "%s", td_is_error(r) ? "?" : jstr(r, "title"));
    cJSON_Delete(r);
}

void chat_user_display(Td *td, long long user_id, char *buf, size_t n) {
    cJSON *r = request_by_id(td, "getUser", "user_id", user_id);
    if (td_is_error(r)) {
        snprintf(buf, n, "?");
    } else {
        chat_user_name(r, buf, n);
    }
    cJSON_Delete(r);
}

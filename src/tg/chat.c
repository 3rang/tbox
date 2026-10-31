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

static const char *strip_prefix(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0 ? s + n : NULL;
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
    const char *name = NULL;
    if (s[0] == '@') {
        name = s + 1;
    } else if ((name = strip_prefix(s, "https://t.me/")) == NULL &&
               (name = strip_prefix(s, "http://t.me/")) == NULL &&
               (name = strip_prefix(s, "t.me/")) == NULL) {
        name = NULL;
    }
    if (name != NULL) {
        out->kind = CHAT_REF_USERNAME;
        snprintf(out->text, sizeof out->text, "%s", name);
        size_t len = strlen(out->text);
        if (len > 0 && out->text[len - 1] == '/') {
            out->text[len - 1] = '\0';
        }
        return out->text[0] != '\0' ? 0 : -1;
    }
    out->kind = CHAT_REF_NAME;
    snprintf(out->text, sizeof out->text, "%s", s);
    return 0;
}

const char *chat_type_name(const cJSON *chat) {
    const cJSON *type = jobj(chat, "type");
    const char *t = jtype(type);
    if (strcmp(t, "chatTypePrivate") == 0) {
        return "private";
    }
    if (strcmp(t, "chatTypeBasicGroup") == 0) {
        return "group";
    }
    if (strcmp(t, "chatTypeSupergroup") == 0) {
        return jbool(type, "is_channel") ? "channel" : "supergroup";
    }
    if (strcmp(t, "chatTypeSecret") == 0) {
        return "secret";
    }
    return "?";
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
    cJSON *req = jreq("createPrivateChat");
    cJSON_AddNumberToObject(req, "user_id", (double)jint(me, "id"));
    cJSON_AddBoolToObject(req, "force", 0);
    cJSON_Delete(me);
    return reply_chat_id(td_request(td, req, TD_TIMEOUT), err, errlen, "Saved Messages");
}

static long long resolve_username(Td *td, const char *name, char *err, size_t errlen) {
    cJSON *req = jreq("searchPublicChat");
    cJSON_AddStringToObject(req, "username", name);
    return reply_chat_id(td_request(td, req, TD_TIMEOUT), err, errlen, name);
}

static long long resolve_id(Td *td, long long id, char *err, size_t errlen) {
    for (int attempt = 0; attempt < 2; attempt++) {
        cJSON *req = jreq("getChat");
        cJSON_AddNumberToObject(req, "chat_id", (double)id);
        cJSON *r = td_request(td, req, TD_TIMEOUT);
        if (!td_is_error(r)) {
            cJSON_Delete(r);
            return id;
        }
        cJSON_Delete(r);
        /* A fresh session knows no chats yet: load the main list and retry. */
        cJSON *load = jreq("loadChats");
        cJSON_AddNumberToObject(load, "limit", 200);
        cJSON_Delete(td_request(td, load, TD_TIMEOUT));
    }
    snprintf(err, errlen, "chat %lld not found (is it in your chat list?)", id);
    return 0;
}

static long long resolve_name(Td *td, const char *name, char *err, size_t errlen) {
    cJSON *req = jreq("searchChats");
    cJSON_AddStringToObject(req, "query", name);
    cJSON_AddNumberToObject(req, "limit", 10);
    cJSON *r = td_request(td, req, TD_TIMEOUT);
    if (td_is_error(r)) {
        snprintf(err, errlen, "searchChats: %s", td_error_message(r));
        cJSON_Delete(r);
        return 0;
    }
    cJSON *ids = jobj(r, "chat_ids");
    int n = cJSON_GetArraySize(ids);
    if (n == 0) {
        cJSON_Delete(r);
        snprintf(err, errlen, "no chat named \"%s\"; try `tbox search %s`", name, name);
        return 0;
    }
    if (n > 1) {
        size_t used = (size_t)snprintf(err, errlen, "\"%s\" matches %d chats, use the id:", name, n);
        for (int i = 0; i < n && used < errlen; i++) {
            long long id = (long long)cJSON_GetArrayItem(ids, i)->valuedouble;
            cJSON *g = jreq("getChat");
            cJSON_AddNumberToObject(g, "chat_id", (double)id);
            cJSON *chat = td_request(td, g, TD_TIMEOUT);
            used += (size_t)snprintf(err + used, errlen - used, "\n  %lld  %s", id, jstr(chat, "title"));
            cJSON_Delete(chat);
        }
        cJSON_Delete(r);
        return 0;
    }
    long long id = (long long)cJSON_GetArrayItem(ids, 0)->valuedouble;
    cJSON_Delete(r);
    return id;
}

long long chat_resolve(Td *td, const char *ref, char *err, size_t errlen) {
    ChatRef cr;
    if (chat_ref_parse(ref, &cr) != 0) {
        snprintf(err, errlen, "empty chat reference");
        return 0;
    }
    switch (cr.kind) {
    case CHAT_REF_ME:
        return resolve_me(td, err, errlen);
    case CHAT_REF_USERNAME:
        return resolve_username(td, cr.text, err, errlen);
    case CHAT_REF_ID:
        return resolve_id(td, cr.id, err, errlen);
    case CHAT_REF_NAME:
    default:
        return resolve_name(td, cr.text, err, errlen);
    }
}

/* --- description ---------------------------------------------------------- */

void chat_describe(Td *td, const cJSON *chat, char *buf, size_t n) {
    const cJSON *type = jobj(chat, "type");
    const char *t = jtype(type);
    char username[128] = "";

    cJSON *req = NULL;
    if (strcmp(t, "chatTypePrivate") == 0) {
        req = jreq("getUser");
        cJSON_AddNumberToObject(req, "user_id", (double)jint(type, "user_id"));
    } else if (strcmp(t, "chatTypeSupergroup") == 0) {
        req = jreq("getSupergroup");
        cJSON_AddNumberToObject(req, "supergroup_id", (double)jint(type, "supergroup_id"));
    }
    if (req != NULL) {
        cJSON *r = td_request(td, req, TD_TIMEOUT);
        if (!td_is_error(r) && chat_username(r)[0] != '\0') {
            snprintf(username, sizeof username, "  @%s", chat_username(r));
        }
        cJSON_Delete(r);
    }
    snprintf(buf, n, "%-15lld %-10s %s%s", jint(chat, "id"), chat_type_name(chat),
             jstr(chat, "title"), username);
}

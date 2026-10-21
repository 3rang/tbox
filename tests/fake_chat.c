/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * fake_chat.c — the in-memory Saved Messages chat (see fake_chat.h).
 *
 * The message JSON is a faithful subset of what TDLib sends: enough of
 * messageDocument/messagePhoto/messageVideo/messageAudio/messageVoiceNote and
 * messageText for the scan and record code to read, with the caption or text
 * carried in a formattedText object the way TDLib carries it. Nothing here
 * talks to the network or the filesystem.
 */
#include "fake_chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

struct FakeChat {
    FakeMsg *msgs;      /* kept sorted by id DESCENDING (newest first) */
    size_t   count;
    size_t   cap;
    long long next_id;  /* ids count up from 1 */
    int       page_size;/* 0 = honour limit */
    char    **updates;  /* update JSON queued by edits/deletes */
    size_t    upd_count;
    size_t    upd_cap;
    size_t    upd_head;
};

static char *dup_str(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

FakeChat *fake_chat_new(void) {
    FakeChat *c = (FakeChat *)calloc(1, sizeof(FakeChat));
    if (c != NULL) {
        c->next_id = 1;
    }
    return c;
}

static void free_msg(FakeMsg *m) {
    free(m->caption);
    free(m->text);
    free(m->file.name);
}

void fake_chat_free(FakeChat *c) {
    if (c == NULL) {
        return;
    }
    for (size_t i = 0; i < c->count; i++) {
        free_msg(&c->msgs[i]);
    }
    free(c->msgs);
    for (size_t i = 0; i < c->upd_count; i++) {
        free(c->updates[i]);
    }
    free(c->updates);
    free(c);
}

/* Insert keeping the array sorted by id descending (newest first). */
static FakeMsg *insert_slot(FakeChat *c, long long id) {
    if (c->count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 8;
        FakeMsg *grown = (FakeMsg *)realloc(c->msgs, cap * sizeof(FakeMsg));
        if (grown == NULL) {
            return NULL;
        }
        c->msgs = grown;
        c->cap = cap;
    }
    size_t pos = 0;
    while (pos < c->count && c->msgs[pos].id > id) {
        pos++;
    }
    memmove(&c->msgs[pos + 1], &c->msgs[pos],
            (c->count - pos) * sizeof(FakeMsg));
    c->count++;
    memset(&c->msgs[pos], 0, sizeof(FakeMsg));
    c->msgs[pos].id = id;
    return &c->msgs[pos];
}

static size_t find_index(const FakeChat *c, long long id) {
    for (size_t i = 0; i < c->count; i++) {
        if (c->msgs[i].id == id) {
            return i;
        }
    }
    return (size_t)-1;
}

long long fake_chat_add_media(FakeChat *c, const char *kind, const char *name,
                              long long size, long long remote_id,
                              const char *caption) {
    long long id = c->next_id++;
    FakeMsg *m = insert_slot(c, id);
    if (m == NULL) {
        return 0;
    }
    m->kind = kind;
    m->caption = dup_str(caption);
    m->date = 1760000000 + id;
    m->has_file = 1;
    m->file.id = id * 10;
    m->file.remote_id = remote_id;
    m->file.name = dup_str(name);
    m->file.size = size;
    snprintf(m->file.mime, sizeof m->file.mime, "%s", "application/octet-stream");
    return id;
}

long long fake_chat_add_text(FakeChat *c, const char *text) {
    long long id = c->next_id++;
    FakeMsg *m = insert_slot(c, id);
    if (m == NULL) {
        return 0;
    }
    m->kind = "text";
    m->text = dup_str(text);
    m->date = 1760000000 + id;
    return id;
}

/* ---- update queue ---------------------------------------------------- */

static void queue_update(FakeChat *c, char *json) {
    if (json == NULL) {
        return;
    }
    if (c->upd_count == c->upd_cap) {
        size_t cap = c->upd_cap ? c->upd_cap * 2 : 4;
        char **grown = (char **)realloc(c->updates, cap * sizeof(char *));
        if (grown == NULL) {
            free(json);
            return;
        }
        c->updates = grown;
        c->upd_cap = cap;
    }
    c->updates[c->upd_count++] = json;
}

char *fake_chat_take_update(FakeChat *c) {
    if (c->upd_head >= c->upd_count) {
        return NULL;
    }
    /* Ownership passes to the caller; clear the slot so fake_chat_free does
     * not free the same string a second time. */
    char *json = c->updates[c->upd_head];
    c->updates[c->upd_head] = NULL;
    c->upd_head++;
    return json;
}

/* ---- edits and deletes ----------------------------------------------- */

/* Builds updateMessageContent / updateMessageEdited for the message at index
 * `i`. TDLib sends MessageContent for a media caption edit and MessageEdited
 * for a text edit; this mirrors that split. */
static void queue_edit_update(FakeChat *c, size_t i) {
    const FakeMsg *m = &c->msgs[i];
    cJSON *u = cJSON_CreateObject();
    if (u == NULL) {
        return;
    }
    if (m->kind != NULL && strcmp(m->kind, "text") == 0) {
        cJSON_AddStringToObject(u, "@type", "updateMessageEdited");
        cJSON_AddNumberToObject(u, "chat_id", 1);
        cJSON_AddNumberToObject(u, "message_id", (double)m->id);
        cJSON_AddNumberToObject(u, "edit_date", (double)m->date);
    } else {
        cJSON_AddStringToObject(u, "@type", "updateMessageContent");
        cJSON_AddNumberToObject(u, "chat_id", 1);
        cJSON_AddNumberToObject(u, "message_id", (double)m->id);
    }
    queue_update(c, cJSON_PrintUnformatted(u));
    cJSON_Delete(u);
}

void fake_chat_edit_caption(FakeChat *c, long long id, const char *caption) {
    size_t i = find_index(c, id);
    if (i == (size_t)-1) {
        return;
    }
    free(c->msgs[i].caption);
    c->msgs[i].caption = dup_str(caption);
    queue_edit_update(c, i);
}

void fake_chat_delete(FakeChat *c, long long id, int permanent) {
    size_t i = find_index(c, id);
    if (i == (size_t)-1) {
        return;
    }
    cJSON *u = cJSON_CreateObject();
    if (u != NULL) {
        cJSON_AddStringToObject(u, "@type", "updateDeleteMessages");
        cJSON_AddNumberToObject(u, "chat_id", 1);
        cJSON *ids = cJSON_AddArrayToObject(u, "message_ids");
        cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)id));
        cJSON_AddBoolToObject(u, "is_permanent", permanent ? 1 : 0);
        cJSON_AddBoolToObject(u, "from_cache", permanent ? 0 : 1);
        queue_update(c, cJSON_PrintUnformatted(u));
        cJSON_Delete(u);
    }
    if (permanent) {
        free_msg(&c->msgs[i]);
        memmove(&c->msgs[i], &c->msgs[i + 1],
                (c->count - i - 1) * sizeof(FakeMsg));
        c->count--;
    }
}

/* ---- reading --------------------------------------------------------- */

const FakeMsg *fake_chat_get(const FakeChat *c, long long id) {
    size_t i = find_index(c, id);
    return i == (size_t)-1 ? NULL : &c->msgs[i];
}

size_t fake_chat_count(const FakeChat *c) {
    return c->count;
}

void fake_chat_set_page_size(FakeChat *c, int page_size) {
    c->page_size = page_size;
}

size_t fake_chat_ids(const FakeChat *c, long long *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < c->count && n < cap; i++) {
        out[n++] = c->msgs[i].id;
    }
    return n;
}

/* ---- JSON ------------------------------------------------------------ */

/* The `file` object of a document/photo/... : id, size and remote id. */
static cJSON *file_json(const FakeFile *f) {
    cJSON *j = cJSON_CreateObject();
    if (j == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(j, "@type", "file");
    cJSON_AddNumberToObject(j, "id", (double)f->id);
    cJSON_AddNumberToObject(j, "size", (double)f->size);
    cJSON *r = cJSON_CreateObject();
    if (r != NULL) {
        cJSON_AddStringToObject(r, "@type", "remoteFile");
        cJSON_AddNumberToObject(r, "id", (double)f->remote_id);
        cJSON_AddItemToObject(j, "remote", r);
    }
    return j;
}

static cJSON *formatted_text(const char *text) {
    cJSON *t = cJSON_CreateObject();
    if (t == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(t, "@type", "formattedText");
    cJSON_AddStringToObject(t, "text", text != NULL ? text : "");
    return t;
}

/* The `content` object for one message. */
static cJSON *content_json(const FakeMsg *m) {
    cJSON *content = cJSON_CreateObject();
    if (content == NULL) {
        return NULL;
    }
    if (strcmp(m->kind, "text") == 0) {
        cJSON_AddStringToObject(content, "@type", "messageText");
        cJSON *t = formatted_text(m->text);
        if (t != NULL) {
            cJSON_AddItemToObject(content, "text", t);
        }
        return content;
    }
    const char *ctype = "messageDocument";
    if (strcmp(m->kind, "photo") == 0) {
        ctype = "messagePhoto";
    } else if (strcmp(m->kind, "video") == 0) {
        ctype = "messageVideo";
    } else if (strcmp(m->kind, "audio") == 0) {
        ctype = "messageAudio";
    } else if (strcmp(m->kind, "voice") == 0) {
        ctype = "messageVoiceNote";
    }
    cJSON_AddStringToObject(content, "@type", ctype);

    const char *field = "document";
    if (strcmp(m->kind, "photo") == 0) {
        field = "photo";
    } else if (strcmp(m->kind, "video") == 0) {
        field = "video";
    } else if (strcmp(m->kind, "audio") == 0) {
        field = "audio";
    } else if (strcmp(m->kind, "voice") == 0) {
        field = "voice_note";
    }

    cJSON *item = cJSON_CreateObject();
    if (item != NULL) {
        /* photos wrap their file; the others carry it directly */
        if (strcmp(m->kind, "photo") == 0) {
            cJSON_AddStringToObject(item, "@type", "photo");
            cJSON *f = file_json(&m->file);
            if (f != NULL) {
                cJSON_AddItemToObject(item, "sizes", cJSON_CreateArray());
                cJSON_AddItemToObject(item, "photo", f);
            }
        } else {
            cJSON_AddStringToObject(item, "@type", m->kind);
            if (m->file.name != NULL) {
                cJSON_AddStringToObject(item, "file_name", m->file.name);
            }
            cJSON_AddStringToObject(item, "mime_type", m->file.mime);
            cJSON *f = file_json(&m->file);
            if (f != NULL) {
                cJSON_AddItemToObject(item, field, f);
            }
        }
        cJSON_AddItemToObject(content, field, item);
    }
    cJSON *cap = formatted_text(m->caption);
    if (cap != NULL) {
        cJSON_AddItemToObject(content, "caption", cap);
    }
    return content;
}

static cJSON *message_json(const FakeMsg *m) {
    cJSON *j = cJSON_CreateObject();
    if (j == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(j, "@type", "message");
    cJSON_AddNumberToObject(j, "id", (double)m->id);
    cJSON_AddNumberToObject(j, "chat_id", 1);
    cJSON_AddNumberToObject(j, "date", (double)m->date);
    cJSON *content = content_json(m);
    if (content != NULL) {
        cJSON_AddItemToObject(j, "content", content);
    }
    return j;
}

char *fake_chat_message_json(const FakeChat *c, long long id) {
    const FakeMsg *m = fake_chat_get(c, id);
    if (m == NULL) {
        return NULL;
    }
    cJSON *j = message_json(m);
    if (j == NULL) {
        return NULL;
    }
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

char *fake_chat_history_json(const FakeChat *c, long long from_message_id,
                             int offset, int limit) {
    if (limit <= 0) {
        limit = 100;
    }
    /* TDLib may return fewer than `limit`; the page-size cap models that. */
    int window = limit;
    if (c->page_size > 0 && c->page_size < window) {
        window = c->page_size;
    }

    cJSON *messages = cJSON_CreateArray();
    if (messages == NULL) {
        return NULL;
    }
    int produced = 0;
    int skipped = 0;
    for (size_t i = 0; i < c->count && produced < window; i++) {
        const FakeMsg *m = &c->msgs[i]; /* newest first */
        if (from_message_id != 0 && m->id >= from_message_id) {
            continue; /* history is strictly below from_message_id */
        }
        if (skipped < offset) {
            skipped++;
            continue;
        }
        cJSON *mj = message_json(m);
        if (mj != NULL) {
            cJSON_AddItemToArray(messages, mj);
        }
        produced++;
    }

    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) {
        cJSON_Delete(messages);
        return NULL;
    }
    cJSON_AddStringToObject(reply, "@type", "messages");
    cJSON_AddNumberToObject(reply, "total_count", (double)c->count);
    cJSON_AddItemToObject(reply, "messages", messages);
    char *out = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    return out;
}

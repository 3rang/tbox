/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * message.c â€” classify a TDLib message and fill a `messages` row (spec 7.1,
 * 7.2). Pure: it reads a cJSON tree and writes a plain struct.
 *
 * The only subtlety is the file shape. TDLib nests the file differently per
 * media type: a photo carries its file under `photo`, while a document,
 * video, audio and voice note carry theirs under the same-named field. One
 * small lookup handles them all, so adding a type later is a one-line change.
 */
#include "catalog/message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#ifdef _WIN32
#define TBOX_STRDUP _strdup
#else
#define TBOX_STRDUP strdup
#endif

/* ---- small readers --------------------------------------------------- */

static const cJSON *field(const cJSON *o, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}

static const char *str_field(const cJSON *o, const char *key) {
    const cJSON *v = field(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int has_num(const cJSON *o, const char *key, long long *out) {
    const cJSON *v = field(o, key);
    if (!cJSON_IsNumber(v)) {
        return 0;
    }
    *out = (long long)v->valuedouble;
    return 1;
}

/* The text of a formattedText object (caption or text), or NULL. */
static const char *formatted_text(const cJSON *content, const char *key) {
    const cJSON *ft = field(content, key);
    if (!cJSON_IsObject(ft)) {
        return NULL;
    }
    return str_field(ft, "text");
}

/* ---- media classification -------------------------------------------- */

/* The TDLib content @types the catalog treats as a file. */
typedef struct MediaType {
    const char *content_type; /* content.@type */
    const char *item;         /* the object holding the file */
    const char *file_field;   /* the field under `item` that is the file */
} MediaType;

static const MediaType MEDIA[] = {
    { "messageDocument",  "document",   "document" },
    { "messagePhoto",     "photo",      "photo" },
    { "messageVideo",     "video",      "video" },
    { "messageAudio",     "audio",      "audio" },
    { "messageVoiceNote", "voice_note", "voice_note" },
};

static const MediaType *find_media(const char *content_type) {
    for (size_t i = 0; i < sizeof MEDIA / sizeof MEDIA[0]; i++) {
        if (strcmp(MEDIA[i].content_type, content_type) == 0) {
            return &MEDIA[i];
        }
    }
    return NULL;
}

/* ---- parsing --------------------------------------------------------- */

/* Copies src into dst (cap bytes) if it fits, reporting success. */
static int copy_into(char *dst, size_t cap, const char *src) {
    if (src == NULL) {
        return 0;
    }
    size_t n = strlen(src);
    if (n + 1 > cap) {
        return 0;
    }
    memcpy(dst, src, n + 1);
    return 1;
}

/* Fills the file fields (id, remote id, size, name, mime) from a media item. */
static void fill_file(const cJSON *item, const MediaType *mt,
                      TBoxMessageRow *row) {
    const char *name = str_field(item, "file_name");
    if (name != NULL) {
        copy_into(row->orig_name, sizeof row->orig_name, name);
    }
    const char *mime = str_field(item, "mime_type");
    if (mime != NULL) {
        copy_into(row->mime, sizeof row->mime, mime);
    }

    const cJSON *f = field(item, mt->file_field);
    if (cJSON_IsObject(f)) {
        long long v;
        if (has_num(f, "id", &v)) {
            row->has_file_id = 1;
            row->file_id = v;
        }
        if (has_num(f, "size", &v)) {
            row->has_size = 1;
            row->size = v;
        }
        const cJSON *remote = field(f, "remote");
        if (cJSON_IsObject(remote) && has_num(remote, "id", &v)) {
            row->has_remote_id = 1;
            row->remote_id = v;
        }
    }
}

/* Parses a caption/text into a record, setting the record and bad_record
 * flags. Returns 1 when the text is a valid record. */
static int parse_record_field(const char *text, int is_media, TBoxMessageRow *row) {
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    TBoxRecord rec;
    TBoxRecordStatus st = tbox_record_parse(text, &rec);
    if (st == TBOX_REC_OK) {
        row->has_record = 1;
        row->rec = rec;
        return 1;
    }
    /* a managed prefix that did not parse is flagged for doctor.run, whether
     * it sits on a file (unmanaged) or on a note */
    if (st != TBOX_REC_E_NOPREFIX) {
        row->bad_record = 1;
    }
    (void)is_media;
    return 0;
}

int tbox_message_parse(const cJSON *message, TBoxMessageRow *out) {
    if (message == NULL || out == NULL || !cJSON_IsObject(message)) {
        return -1;
    }
    memset(out, 0, sizeof *out);

    long long id;
    if (!has_num(message, "id", &id)) {
        return -1;
    }
    snprintf(out->message_id, sizeof out->message_id, "%lld", id);
    long long date = 0;
    (void)has_num(message, "date", &date);
    out->date = date;

    const cJSON *content = field(message, "content");
    if (!cJSON_IsObject(content)) {
        return 0; /* no content: not a catalog message */
    }
    const char *ctype = str_field(content, "@type");
    if (ctype == NULL) {
        return 0;
    }

    const MediaType *mt = find_media(ctype);
    if (mt != NULL) {
        const cJSON *item = field(content, mt->item);
        if (cJSON_IsObject(item)) {
            fill_file(item, mt, out);
        }
        const char *cap = formatted_text(content, "caption");
        if (cap != NULL) {
            out->text = TBOX_STRDUP(cap);
        }
        if (parse_record_field(cap, 1, out) && out->rec.kind == 'f') {
            snprintf(out->kind, sizeof out->kind, "%s", TBOX_MSG_KIND_FILE);
        } else {
            snprintf(out->kind, sizeof out->kind, "%s", TBOX_MSG_KIND_UNMANAGED);
        }
        return 1;
    }

    if (strcmp(ctype, "messageText") == 0) {
        const char *text = formatted_text(content, "text");
        if (text != NULL) {
            out->text = TBOX_STRDUP(text);
        }
        if (parse_record_field(text, 0, out) && out->rec.kind == 'd') {
            snprintf(out->kind, sizeof out->kind, "%s", TBOX_MSG_KIND_FOLDER);
        } else {
            snprintf(out->kind, sizeof out->kind, "%s", TBOX_MSG_KIND_NOTE);
        }
        return 1;
    }

    /* stickers, polls, service messages, ...: ignored by the catalog */
    return 0;
}

void tbox_message_row_free(TBoxMessageRow *row) {
    if (row == NULL) {
        return;
    }
    free(row->text);
    row->text = NULL;
}

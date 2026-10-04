/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * tdscan.c - Saved Messages -> archive entries.
 *
 *   whoami        getMe for the Saved Messages chat id
 *   history_page  getChatHistory, one page, newest to oldest
 *   entry_of      one message JSON -> one archive entry (core/scanmap.c does
 *                 the naming; this only reads fields out of TDLib's JSON)
 *   tbox_td_scan  the loop
 */

#include "tdscan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "core/scanmap.h"
#include "util/tbox_thread.h"

/* ---- small JSON readers ----------------------------------------------------- */

static long long td_int(const cJSON *obj, const char *key, long long fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);

    if (!cJSON_IsNumber(item))
        return fallback;
    return (long long)item->valuedouble;
}

static const char *td_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const cJSON *td_obj(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);

    return cJSON_IsObject(item) ? item : NULL;
}

static const cJSON *td_arr(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);

    return cJSON_IsArray(item) ? item : NULL;
}

/* 1 when `obj` carries that @type. */
static int is_type(const cJSON *obj, const char *want)
{
    const char *type = td_str(obj, "@type");

    return type != NULL && strcmp(type, want) == 0;
}

/* TDLib's own words for a refusal, or "" when this is not an error object. */
static const char *error_text(const cJSON *obj)
{
    const char *text;

    if (!is_type(obj, "error"))
        return "";

    text = td_str(obj, "message");
    return text != NULL ? text : "unspecified error";
}

static void copy_field(char *dst, size_t size, const char *src)
{
    size_t n;

    if (dst == NULL || size == 0)
        return;

    dst[0] = '\0';
    if (src == NULL)
        return;

    n = strlen(src);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- getMe: the Saved Messages chat id --------------------------------------- */

int tbox_td_scan_whoami(tbox_tdbridge_t *bridge, long long *chat_id,
                        char *user_name, size_t user_name_size,
                        char *phone, size_t phone_size)
{
    char *reply = NULL;
    cJSON *msg;
    long long id;

    if (bridge == NULL || chat_id == NULL)
        return -1;

    if (tbox_tdbridge_call(bridge, "{\"@type\":\"getMe\"}",
                           TBOX_SCAN_PAGE_TIMEOUT, &reply) != TBOX_TDB_OK) {
        fprintf(stderr, "scan: getMe did not answer.\n");
        return -1;
    }

    msg = cJSON_Parse(reply);
    free(reply);
    if (msg == NULL) {
        fprintf(stderr, "scan: getMe answered with unreadable JSON.\n");
        return -1;
    }

    if (!is_type(msg, "user")) {
        fprintf(stderr, "scan: getMe failed: %s\n", error_text(msg));
        cJSON_Delete(msg);
        return -1;
    }

    /* Saved Messages is the account's own chat, and its id is the user id. */
    id = td_int(msg, "id", 0);
    if (id == 0) {
        fprintf(stderr, "scan: getMe returned no user id.\n");
        cJSON_Delete(msg);
        return -1;
    }

    *chat_id = id;
    copy_field(user_name, user_name_size, td_str(msg, "first_name"));
    copy_field(phone, phone_size, td_str(msg, "phone_number"));

    cJSON_Delete(msg);
    return 0;
}

/* ---- getChatHistory --------------------------------------------------------- */

/* One page, starting at `from_id` (0 = the newest message). Caller frees. */
static char *history_request(long long chat_id, long long from_id, long long limit)
{
    cJSON *req = cJSON_CreateObject();
    char *text;

    if (req == NULL)
        return NULL;

    cJSON_AddStringToObject(req, "@type", "getChatHistory");
    cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
    cJSON_AddNumberToObject(req, "from_message_id", (double)from_id);
    cJSON_AddNumberToObject(req, "offset", 0);
    cJSON_AddNumberToObject(req, "limit", (double)limit);
    /* only_local = false: this has to read the server, not whatever TDLib
     * happens to have cached, or a cold session would scan nothing. It is
     * spelled as a JSON boolean on purpose - this generation of TDLib has a
     * strict parser and does not accept 0/1 for a bool field. */
    cJSON_AddBoolToObject(req, "only_local", 0);

    text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    return text;
}

/*
 * One page of history, as the parsed "messages" object (caller deletes).
 *
 * The first page is allowed a few retries because "Chat not found" on a
 * freshly opened session is a race with the database load, not a refusal -
 * see TBOX_SCAN_WARM_TRIES. Every later page gets exactly one attempt: by then
 * the chat has long since existed, so an error is real and retrying it would
 * only turn a failure into a hang.
 *
 * Returns TBOX_SCAN_*. On an error the error object is handed back as usual
 * and the caller reports it, so exhausting the retries still says what
 * Telegram actually replied.
 */
static int history_page(tbox_tdbridge_t *bridge, long long chat_id,
                        long long from_id, int first,
                        tbox_scan_stats_t *stats, cJSON **out)
{
    char *request;
    int tries;

    *out = NULL;

    request = history_request(chat_id, from_id, TBOX_SCAN_PAGE_LIMIT);
    if (request == NULL) {
        fprintf(stderr, "scan: could not build the history request.\n");
        return TBOX_SCAN_ERROR;
    }

    for (tries = 1; ; tries++) {
        char *reply = NULL;
        cJSON *msg;
        int rc;

        stats->pages++;

        rc = tbox_tdbridge_call(bridge, request, TBOX_SCAN_PAGE_TIMEOUT, &reply);
        if (rc != TBOX_TDB_OK) {
            fprintf(stderr, "scan: history page %lld %s.\n", stats->pages,
                    rc == TBOX_TDB_STOPPED ? "cancelled" : "did not answer");
            free(reply);
            free(request);
            return rc == TBOX_TDB_STOPPED ? TBOX_SCAN_CANCELLED
                                          : TBOX_SCAN_ERROR;
        }

        msg = cJSON_Parse(reply);
        free(reply);

        if (msg == NULL) {
            fprintf(stderr, "scan: page %lld was unreadable.\n", stats->pages);
            free(request);
            return TBOX_SCAN_ERROR;
        }

        if (is_type(msg, "messages") || !first
            || tries >= TBOX_SCAN_WARM_TRIES) {
            free(request);
            *out = msg;
            return TBOX_SCAN_OK;
        }

        fprintf(stderr, "scan: Saved Messages is not there yet (%s) - "
                        "waiting.\n", error_text(msg));
        cJSON_Delete(msg);
        tbox_sleep_ms(TBOX_SCAN_WARM_WAIT_MS);
    }
}

/* ---- one message -> one entry ----------------------------------------------- */

/*
 * The caption, as plain text. TDLib has moved this more than once:
 *   content.caption.text     a formattedText object (current)
 *   document.caption         on the Document itself, older
 *   message.text             before 1.8, when the caption sat on the message
 * All three are worth reading, because the caption is where tbox put the
 * name of the file and the tree it belongs in.
 */
static const char *caption_of(const cJSON *obj)
{
    const cJSON *caption = cJSON_GetObjectItem(obj, "caption");

    if (cJSON_IsObject(caption))
        return cJSON_GetStringValue(cJSON_GetObjectItem(caption, "text"));

    return cJSON_IsString(caption) ? caption->valuestring : NULL;
}

static const char *caption_text(const cJSON *message, const cJSON *content)
{
    const char *text = caption_of(content);

    if (text == NULL)
        text = caption_of(td_obj(content, "document"));
    if (text == NULL)
        text = td_str(message, "text");

    return text;
}

/*
 * Size in bytes. The current Document object carries no size of its own at
 * all - it hangs off the nested File, where expected_size is the whole file
 * and size is just this one variant. Older builds put size / file_size on
 * Document directly, so those are still accepted as a fallback.
 */
static long long document_size(const cJSON *document)
{
    const cJSON *file = td_obj(document, "document");
    long long size;

    if (file != NULL) {
        size = td_int(file, "expected_size", -1);
        if (size < 0)
            size = td_int(file, "size", -1);
        if (size >= 0)
            return size;
    }

    size = td_int(document, "file_size", -1);
    return size >= 0 ? size : td_int(document, "size", 0);
}

/*
 * Fill `out` from one TDLib message. Returns 1 when the message carries a
 * document (so it belongs in the archive), 0 when it does not - a text-only
 * message, a sticker, a poll - and -1 when the message is unusable.
 */
static int entry_of(const cJSON *message, tbox_archive_entry_t *out)
{
    const cJSON *content;
    const cJSON *document;
    const cJSON *file;
    long long message_id = td_int(message, "id", 0);

    if (message_id <= 0)
        return -1;

    content = td_obj(message, "content");
    if (content == NULL || !is_type(content, "messageDocument"))
        return 0;                       /* not a file: invisible, by design */

    document = td_obj(content, "document");
    if (document == NULL)
        return 0;

    /* The File object: downloadFile wants its numeric id, not remote.id,
     * which is a base64 string in this generation of TDLib. That id is the
     * local database's handle for the file, stable for one account on one
     * database, and it is rebuilt by every scan, so a RETR always uses the
     * handle this session knows. */
    file = td_obj(document, "document");

    if (tbox_scanmap_entry(message_id,
                           caption_text(message, content),
                           td_str(document, "file_name"),
                           document_size(document),
                           td_int(message, "date", 0),
                           file != NULL ? td_int(file, "id", 0) : 0,
                           out) != 0)
        return -1;

    return 1;
}

/* ---- the walk --------------------------------------------------------------- */

int tbox_td_scan(tbox_tdbridge_t *bridge, long long chat_id,
                 volatile bool *should_stop, long long max_pages,
                 tbox_archive_t *ar, tbox_scan_stats_t *stats)
{
    tbox_scan_stats_t local;
    long long from_id = 0;
    int have_from = 0;

    if (bridge == NULL || ar == NULL || chat_id == 0)
        return TBOX_SCAN_ERROR;

    memset(&local, 0, sizeof local);

    for (;;) {
        cJSON *msg = NULL;
        const cJSON *list;
        const cJSON *item;
        long long oldest = 0;
        int rc;

        if (should_stop != NULL && *should_stop) {
            if (stats != NULL)
                *stats = local;
            return TBOX_SCAN_CANCELLED;
        }
        if (max_pages > 0 && local.pages >= max_pages) {
            local.truncated = 1;
            if (stats != NULL)
                *stats = local;
            return TBOX_SCAN_OK;
        }

        rc = history_page(bridge, chat_id, from_id, have_from ? 0 : 1,
                          &local, &msg);
        if (rc != TBOX_SCAN_OK) {
            if (stats != NULL)
                *stats = local;
            return rc;
        }

        if (!is_type(msg, "messages")) {
            fprintf(stderr, "scan: history page %lld failed: %s\n", local.pages,
                    error_text(msg));
            cJSON_Delete(msg);
            if (stats != NULL)
                *stats = local;
            return TBOX_SCAN_ERROR;
        }

        /* Older TDLib answered with the array itself; newer wraps it. */
        list = td_arr(msg, "messages");
        if (list == NULL)
            list = msg;

        cJSON_ArrayForEach(item, list) {
            tbox_archive_entry_t entry;
            long long id;

            if (!cJSON_IsObject(item))
                continue;

            id = td_int(item, "id", 0);
            if (id <= 0)
                continue;
            /* from_message_id is inclusive, so this page repeats the message
             * the last one ended on. Skipping what we have already seen makes
             * the walk correct whether TDLib treats it as inclusive or not. */
            if (have_from && id >= from_id)
                continue;

            local.scanned++;

            if (!have_from || id < oldest)
                oldest = id;

            /* Only documents are entries; everything else is skipped. */
            if (entry_of(item, &entry) != 1)
                continue;

            /* Newest first, and whoever claims a name first keeps it: adding
             * the older duplicate on top would resurrect it. */
            if (tbox_archive_find(ar, entry.name) != NULL)
                local.superseded++;
            else if (tbox_archive_add(ar, &entry) == 0)
                local.indexed++;
        }

        cJSON_Delete(msg);

        /* An empty page, or one that did not move further back, is the end of
         * the chat. Without that a server that keeps re-answering the same
         * from_message_id would loop forever. */
        if (oldest == 0 || (have_from && oldest >= from_id))
            break;

        from_id = oldest;
        have_from = 1;
    }

    tbox_archive_sort(ar);

    if (stats != NULL)
        *stats = local;

    return TBOX_SCAN_OK;
}

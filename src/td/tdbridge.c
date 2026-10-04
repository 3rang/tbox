/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * tdbridge.c - the single owner of TDLib's receive side.
 *
 * Three tables, all behind one mutex:
 *   slots[]  request -> reply, matched by the @extra we stamp on the way out
 *   files[]  download progress, matched by the remote file id
 *   on_update  where anything else goes
 *
 * The receive thread never blocks on a caller: it copies a reply into the
 * slot and moves on, so a slow or cancelled waiter cannot stall TDLib.
 */

#include "tdbridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

/* the official C API: td_create_client_id / td_send / td_receive / td_execute */
#include <td/telegram/td_json_client.h>

#include "util/tbox_thread.h"

/* TDLib's own log chatter, off, before anything else. The vendored
 * td_json_client.h no longer declares td_set_log_verbosity_level(), so this is
 * the supported equivalent - the same trick tdhandler.c uses. */
#define TDB_QUIET "{\"@type\":\"setLogVerbosityLevel\",\"new_level\":0}"

/* td_receive() wait, in seconds. Small, so a stop is noticed promptly. */
#define TDB_POLL_SECONDS 0.025

/* How often a waiting caller looks at its slot. */
#define TDB_WAIT_TICK_MS 2

/* Concurrent requests. One per FTP session at most (8), plus the scan loop. */
#define TDB_MAX_SLOTS 32

/* Downloads tracked at once: one per FTP session. */
#define TDB_MAX_FILES 16

/* Our @extra marker. The token follows, so a reply is unmistakably ours. */
#define TDB_EXTRA_PREFIX "tbox:"

/* ---- one in-flight request -------------------------------------------------- */

typedef struct
{
    long long token;   /* 0 = free; otherwise the owning request */
    int ready;         /* 1 once reply holds the answer */
    char *reply;       /* owned until the waiter takes it */

} tdb_slot_t;

/* ---- one download in flight -------------------------------------------------- */

typedef struct
{
    long long file_id;  /* 0 = free */
    int downloading;
    int completed;
    int failed;
    char path[600];

} tdb_file_t;

struct tbox_tdbridge
{
    int client_id;
    tbox_thread_t thread;
    tbox_mutex_t lock;
    volatile int stop;
    long long next_token;
    tdb_slot_t slots[TDB_MAX_SLOTS];
    tdb_file_t files[TDB_MAX_FILES];
    tbox_tdb_update_fn on_update;
    void *on_update_user;
};

/* ---- small helpers ---------------------------------------------------------- */

static char *dup_text(const char *text)
{
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);

    if (copy != NULL)
        memcpy(copy, text, n);
    return copy;
}

/* ---- slots ------------------------------------------------------------------ */

/* Take a free slot and stamp it with a fresh token. 0 when the table is full. */
static long long slot_claim(tbox_tdbridge_t *b)
{
    size_t i;
    long long token = 0;

    tbox_mutex_lock(&b->lock);
    for (i = 0; i < TDB_MAX_SLOTS; i++) {
        if (b->slots[i].token == 0 && !b->slots[i].ready) {
            token = ++b->next_token;
            b->slots[i].token = token;
            b->slots[i].ready = 0;
            b->slots[i].reply = NULL;
            break;
        }
    }
    tbox_mutex_unlock(&b->lock);

    return token;
}

/* Give a slot back, throwing away a reply nobody is going to read. */
static void slot_release(tbox_tdbridge_t *b, long long token)
{
    size_t i;

    tbox_mutex_lock(&b->lock);
    for (i = 0; i < TDB_MAX_SLOTS; i++) {
        if (b->slots[i].token == token) {
            free(b->slots[i].reply);
            b->slots[i].reply = NULL;
            b->slots[i].ready = 0;
            b->slots[i].token = 0;
            break;
        }
    }
    tbox_mutex_unlock(&b->lock);
}

/* Hand the reply to the waiter that owns this token. 1 when it landed. */
static int slot_deliver(tbox_tdbridge_t *b, long long token, const char *reply)
{
    size_t i;
    int landed = 0;

    tbox_mutex_lock(&b->lock);
    for (i = 0; i < TDB_MAX_SLOTS; i++) {
        if (b->slots[i].token == token && !b->slots[i].ready) {
            b->slots[i].reply = dup_text(reply);
            if (b->slots[i].reply != NULL)
                b->slots[i].ready = 1;
            landed = 1;
            break;
        }
    }
    tbox_mutex_unlock(&b->lock);

    return landed;
}

/* Poll one slot. Returns 1 with *reply taken, 0 while still waiting. */
static int slot_take(tbox_tdbridge_t *b, long long token, char **reply)
{
    size_t i;
    int got = 0;

    *reply = NULL;

    tbox_mutex_lock(&b->lock);
    for (i = 0; i < TDB_MAX_SLOTS; i++) {
        if (b->slots[i].token == token) {
            if (b->slots[i].ready) {
                *reply = b->slots[i].reply;
                b->slots[i].reply = NULL;
                b->slots[i].ready = 0;
                b->slots[i].token = 0;
                got = 1;
            }
            break;
        }
    }
    tbox_mutex_unlock(&b->lock);

    return got;
}

/* ---- download progress ------------------------------------------------------ */

/*
 * Read one of TDLib's flags.
 *
 * They arrive as real JSON booleans, and cJSON does not count a boolean as a
 * number: cJSON_IsNumber() is false for both `true` and `false`. Reading one
 * with cJSON_IsNumber therefore ignores every update TDLib ever sends, which
 * looks exactly like a server that never reports progress.
 */
static int bool_field(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);

    if (cJSON_IsTrue(item))
        return 1;
    if (cJSON_IsFalse(item))
        return 0;

    return fallback;
}

/*
 * The slot for `file_id`, or TDB_MAX_FILES when there is none and none is
 * free. `*is_new` says whether the caller would be creating it, so an update
 * about an unknown file can be dropped instead of evicting somebody else.
 * Lock held.
 */
static size_t file_slot(tbox_tdbridge_t *b, long long file_id, int *is_new)
{
    size_t i;
    size_t fresh = TDB_MAX_FILES;

    for (i = 0; i < TDB_MAX_FILES; i++) {
        if (b->files[i].file_id == file_id) {
            *is_new = 0;
            return i;
        }
        if (b->files[i].file_id == 0 && fresh == TDB_MAX_FILES)
            fresh = i;
    }

    *is_new = 1;
    return fresh;
}

static void file_note(tbox_tdbridge_t *b, const cJSON *file)
{
    const cJSON *id_item;
    const cJSON *local;
    const char *path;
    long long id;
    size_t slot;
    int is_new;

    id_item = cJSON_GetObjectItem(file, "id");
    if (!cJSON_IsNumber(id_item))
        return;
    id = (long long)id_item->valuedouble;
    if (id == 0)
        return;

    tbox_mutex_lock(&b->lock);

    slot = file_slot(b, id, &is_new);
    if (slot == TDB_MAX_FILES) {
        /* Table full and this file is not ours to watch (8 FTP sessions cannot
         * fill 16 slots, but never evict a download that is in flight). */
        tbox_mutex_unlock(&b->lock);
        return;
    }

    if (is_new) {
        memset(&b->files[slot], 0, sizeof b->files[slot]);
        b->files[slot].file_id = id;
    }

    local = cJSON_GetObjectItem(file, "local");
    if (cJSON_IsObject(local)) {
        int was_downloading = b->files[slot].downloading;

        b->files[slot].downloading =
                bool_field(local, "is_downloading_active",
                           b->files[slot].downloading);
        b->files[slot].completed =
                bool_field(local, "is_downloading_completed",
                           b->files[slot].completed);

        /* Started, then gone quiet without finishing: TDLib dropped it. A
         * download that was accepted but has not begun yet looks identical, and
         * calling that a failure makes every real download look broken. */
        if (was_downloading && !b->files[slot].downloading
            && !b->files[slot].completed)
            b->files[slot].failed = 1;

        path = cJSON_GetStringValue(cJSON_GetObjectItem(local, "path"));
        if (path != NULL) {
            size_t n = strlen(path);

            if (n >= sizeof b->files[slot].path)
                n = sizeof b->files[slot].path - 1;
            memcpy(b->files[slot].path, path, n);
            b->files[slot].path[n] = '\0';
        }
    }

    tbox_mutex_unlock(&b->lock);
}

/* ---- the receive thread ----------------------------------------------------- */

static void *bridge_main(void *arg)
{
    tbox_tdbridge_t *b = (tbox_tdbridge_t *)arg;

    while (!b->stop) {
        const char *raw = td_receive(TDB_POLL_SECONDS);
        cJSON *msg;
        const char *extra;
        tbox_tdb_update_fn on_update;
        void *on_update_user;
        long long token = 0;

        if (raw == NULL)
            continue;

        msg = cJSON_Parse(raw);
        if (msg == NULL)
            continue;

        /* Is this the answer to one of our requests? */
        extra = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "@extra"));
        if (extra != NULL && strncmp(extra, TDB_EXTRA_PREFIX,
                                     sizeof TDB_EXTRA_PREFIX - 1) == 0)
            token = strtoll(extra + sizeof TDB_EXTRA_PREFIX - 1, NULL, 10);

        if (token != 0) {
            if (slot_deliver(b, token, raw)) {
                cJSON_Delete(msg);
                continue;
            }
            /* our token, but nobody is waiting for it any more: drop it
             * quietly rather than handing a stale reply to the update path */
            cJSON_Delete(msg);
            continue;
        }

        /* An update. Download progress first: the cache polls for it, so it
         * must be recorded even when nobody is listening for updates. */
        {
            const char *type = cJSON_GetStringValue(
                cJSON_GetObjectItem(msg, "@type"));
            const cJSON *file;

            if (type != NULL && strcmp(type, "updateFile") == 0) {
                file = cJSON_GetObjectItem(msg, "file");
                if (cJSON_IsObject(file))
                    file_note(b, file);
            }
        }

        tbox_mutex_lock(&b->lock);
        on_update = b->on_update;
        on_update_user = b->on_update_user;
        tbox_mutex_unlock(&b->lock);

        if (on_update != NULL)
            on_update(on_update_user, raw);

        cJSON_Delete(msg);
    }

    return NULL;
}

/* ---- public ----------------------------------------------------------------- */

tbox_tdbridge_t *tbox_tdbridge_start(void)
{
    tbox_tdbridge_t *b = calloc(1, sizeof *b);

    if (b == NULL)
        return NULL;

    if (tbox_mutex_init(&b->lock) != 0) {
        free(b);
        return NULL;
    }

    (void)td_execute(TDB_QUIET);
    b->client_id = td_create_client_id();
    b->stop = 0;

    if (tbox_thread_start(&b->thread, bridge_main, b) != 0) {
        tbox_mutex_free(&b->lock);
        free(b);
        return NULL;
    }

    return b;
}

void tbox_tdbridge_stop(tbox_tdbridge_t *b)
{
    size_t i;

    if (b == NULL)
        return;

    b->stop = 1;
    if (b->thread.started)
        (void)tbox_thread_join(&b->thread);

    /* the receive thread is gone, so nothing can touch these any more */
    tbox_mutex_lock(&b->lock);
    for (i = 0; i < TDB_MAX_SLOTS; i++) {
        free(b->slots[i].reply);
        b->slots[i].reply = NULL;
        b->slots[i].ready = 0;
        b->slots[i].token = 0;
    }
    tbox_mutex_unlock(&b->lock);

    tbox_mutex_free(&b->lock);
    free(b);
}

void tbox_tdbridge_on_update(tbox_tdbridge_t *b, tbox_tdb_update_fn fn,
                             void *user)
{
    if (b == NULL)
        return;

    tbox_mutex_lock(&b->lock);
    b->on_update = fn;
    b->on_update_user = user;
    tbox_mutex_unlock(&b->lock);
}

int tbox_tdbridge_send(tbox_tdbridge_t *b, const char *request)
{
    if (b == NULL || request == NULL)
        return -1;

    td_send(b->client_id, request);
    return 0;
}

int tbox_tdbridge_call(tbox_tdbridge_t *b, const char *request,
                       long timeout_ms, char **reply)
{
    cJSON *req;
    char *tagged;
    long long token;
    long waited;

    if (reply != NULL)
        *reply = NULL;
    if (b == NULL || request == NULL || reply == NULL)
        return TBOX_TDB_ERROR;
    if (b->stop)
        return TBOX_TDB_STOPPED;

    /* Stamp our own @extra. Parsing and re-printing is cheap next to a
     * Telegram round trip, and it means callers never have to think about
     * tokens. */
    req = cJSON_Parse(request);
    if (req == NULL || !cJSON_IsObject(req)) {
        cJSON_Delete(req);
        return TBOX_TDB_ERROR;
    }

    token = slot_claim(b);
    if (token == 0) {
        cJSON_Delete(req);
        return TBOX_TDB_ERROR;          /* every slot is in use */
    }

    {
        char stamp[32];

        snprintf(stamp, sizeof stamp, "%s%lld", TDB_EXTRA_PREFIX, token);
        /* replace, do not append: a caller-supplied @extra would otherwise
         * produce a duplicate key and TDLib would echo back the wrong one */
        cJSON_DeleteItemFromObjectCaseSensitive(req, "@extra");
        cJSON_AddStringToObject(req, "@extra", stamp);
    }

    tagged = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (tagged == NULL) {
        slot_release(b, token);
        return TBOX_TDB_ERROR;
    }

    td_send(b->client_id, tagged);
    free(tagged);

    for (waited = 0; timeout_ms <= 0 || waited <= timeout_ms;
         waited += TDB_WAIT_TICK_MS) {
        char *got = NULL;

        if (slot_take(b, token, &got)) {
            *reply = got;
            return TBOX_TDB_OK;
        }
        if (b->stop) {
            slot_release(b, token);
            return TBOX_TDB_STOPPED;
        }
        tbox_sleep_ms(TDB_WAIT_TICK_MS);
    }

    slot_release(b, token);
    return TBOX_TDB_TIMEOUT;
}

int tbox_tdbridge_stopping(const tbox_tdbridge_t *b)
{
    return b != NULL && b->stop;
}

int tbox_td_reply_error(const char *reply, char *out, size_t size)
{
    cJSON *msg;
    const char *type;
    const char *text;

    if (out == NULL || size == 0)
        return 0;
    out[0] = '\0';
    if (reply == NULL)
        return 0;

    msg = cJSON_Parse(reply);
    if (msg == NULL)
        return 0;

    type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "@type"));
    if (type == NULL || strcmp(type, "error") != 0) {
        cJSON_Delete(msg);
        return 0;
    }

    /* copied rather than pointed at, so the caller can print it after the
     * reply is freed - and so two FTP sessions can report at the same time */
    text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "message"));
    if (text != NULL) {
        size_t n = strlen(text);

        if (n >= size)
            n = size - 1;
        memcpy(out, text, n);
        out[n] = '\0';
    }

    cJSON_Delete(msg);
    return -1;
}

int tbox_tdbridge_file(tbox_tdbridge_t *b, long long file_id,
                       tbox_td_file_t *out)
{
    size_t i;
    int found = 0;

    if (out == NULL)
        return -1;
    memset(out, 0, sizeof *out);
    if (b == NULL || file_id == 0)
        return -1;

    tbox_mutex_lock(&b->lock);
    for (i = 0; i < TDB_MAX_FILES; i++) {
        if (b->files[i].file_id != file_id)
            continue;

        out->downloading = b->files[i].downloading;
        out->completed = b->files[i].completed;
        out->failed = b->files[i].failed;
        memcpy(out->path, b->files[i].path, sizeof out->path);
        found = 1;
        break;
    }
    tbox_mutex_unlock(&b->lock);

    return found ? 0 : -1;
}

void tbox_tdbridge_file_forget(tbox_tdbridge_t *b, long long file_id)
{
    size_t i;

    if (b == NULL)
        return;

    tbox_mutex_lock(&b->lock);
    for (i = 0; i < TDB_MAX_FILES; i++) {
        if (b->files[i].file_id == file_id)
            memset(&b->files[i], 0, sizeof b->files[i]);
    }
    tbox_mutex_unlock(&b->lock);
}

int tbox_tdbridge_file_want(tbox_tdbridge_t *b, long long file_id)
{
    size_t slot;
    int is_new;

    if (b == NULL || file_id == 0)
        return -1;

    tbox_mutex_lock(&b->lock);
    slot = file_slot(b, file_id, &is_new);
    if (slot != TDB_MAX_FILES && is_new) {
        memset(&b->files[slot], 0, sizeof b->files[slot]);
        b->files[slot].file_id = file_id;
    }
    tbox_mutex_unlock(&b->lock);

    return slot != TDB_MAX_FILES ? 0 : -1;
}

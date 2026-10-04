/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * tdserve.c - the `tbox serve` runner: TDLib session, archive scan, FTP up,
 * rescan on change. The only file that knows all of them at once.
 *
 *   on_update   the update handler, on the bridge thread: sets flags and
 *               nothing else. It has to stay cheap - a slow handler holds up
 *               the replies that FTP sessions are waiting for.
 *   await_auth  Ready, or "run tbox auth", or closed.
 *   fetch_file  the cache's injected fetcher: downloadFile, then copy.
 *   rescan      walk Saved Messages into a fresh index and swap it in.
 *   tbox_td_serve_run  the order of all of it (see tdserve.h).
 */

#include "tdserve.h"

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: every copy below is length-checked */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "core/index.h"
#include "core/indexfile.h"
#include "core/read.h"
#include "net/ftpd.h"
#include "td/tdbridge.h"
#include "td/tdparams.h"
#include "td/tdscan.h"
#include "util/datadir.h"
#include "util/statusfile.h"
#include "util/tbox_thread.h"

#define TBOX_PATH_MAX 512

/* The watch loop wakes this often, so a Ctrl+C is felt immediately. */
#define WATCH_TICK_MS 200

/* A burst of uploads should be one rescan, not one per message. */
#define RESCAN_SETTLE_MS 400

/* How long authorizationStateReady may take on a cold session. */
#define AUTH_TIMEOUT_MS 60000L

/*
 * How long a "needs a human" authorization state may sit unchanged before it
 * is believed. TDLib reports authorizationStateWaitPhoneNumber even with a
 * perfectly good session while it opens the database and checks the auth key,
 * so a state on its own proves nothing - only one that never leaves does.
 */
#define AUTH_HUMAN_MS 8000L

/* How long `close` may take to come back as authorizationStateClosed. */
#define CLOSE_TIMEOUT_MS 3000L

/* One download: TDLib is asked once, then polled. 20 minutes is far beyond any
 * real transfer; it only bounds a hung one. */
#define DOWNLOAD_TIMEOUT_MS 1200000L
#define DOWNLOAD_POLL_MS 200

/* Copy buffer for TDLib's local file -> our cache entry. */
#define COPY_CHUNK (64 * 1024)

/* ---- what the update handler tells us --------------------------------------- */

#define AUTH_UNKNOWN      0
#define AUTH_READY        1
#define AUTH_NEEDS_LOGIN 2
#define AUTH_CLOSED       3

typedef struct
{
    volatile int auth;        /* one of the AUTH_* values */
    volatile int dirty;       /* the archive may have changed */

    tbox_tdbridge_t *bridge;
    tbox_archive_t archive;
    tbox_cache_t cache;
    tbox_ftpd_t *ftpd;
    long long chat_id;
    char status_path[TBOX_PATH_MAX];
    char index_path[TBOX_PATH_MAX];
    tbox_status_t status;
    volatile bool *should_stop;

} serve_t;

/* ---- small helpers ---------------------------------------------------------- */

/* Bounded copy into a fixed field: never trusts the source to fit. */
static void set_field(char *dst, size_t size, const char *src)
{
    size_t n;

    dst[0] = '\0';
    if (src == NULL)
        return;

    n = strlen(src);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- the update handler (bridge thread) -------------------------------------- */

static void on_update(void *user, const char *json)
{
    serve_t *s = (serve_t *)user;
    cJSON *upd = cJSON_Parse(json);
    const char *type;
    const char *state = "";

    if (upd == NULL)
        return;

    type = cJSON_GetStringValue(cJSON_GetObjectItem(upd, "@type"));
    if (type == NULL)
        type = "";

    if (strcmp(type, "updateAuthorizationState") == 0) {
        const cJSON *st = cJSON_GetObjectItem(upd, "authorization_state");
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(st, "@type"));

        if (name != NULL)
            state = name;

        if (strcmp(state, "authorizationStateReady") == 0) {
            s->auth = AUTH_READY;
        } else if (strcmp(state, "authorizationStateLoggingOut") == 0
                   || strcmp(state, "authorizationStateClosing") == 0
                   || strcmp(state, "authorizationStateClosed") == 0) {
            s->auth = AUTH_CLOSED;
        } else {
            /* Everything else is a state that wants a human: a phone number,
             * a code, a password, a QR scan. `serve` never does those. */
            s->auth = AUTH_NEEDS_LOGIN;
        }
        cJSON_Delete(upd);
        return;
    }

    /* Any of these can add, remove or rename an entry. */
    if (strcmp(type, "updateNewMessage") == 0
        || strcmp(type, "updateDeleteMessages") == 0
        || strcmp(type, "updateMessageContent") == 0
        || strcmp(type, "updateEditMessageContent") == 0
        || strcmp(type, "updateMessageSendSucceeded") == 0) {
        s->dirty = 1;
    }

    cJSON_Delete(upd);
}

/* ---- status ----------------------------------------------------------------- */

/*
 * The runner owns status.json from the moment it starts, and publishes the
 * terminal state itself: only it knows the transport, the endpoint and the
 * real entry count, and clobbering them with a bare "stopped" would make
 * `tbox status` report "no transport" for a run that had one.
 */
static void publish(serve_t *s, tbox_state_t state, const char *error)
{
    s->status.state = state;
    s->status.indexed = (long long)s->archive.count;
    s->status.cache_bytes = tbox_cache_bytes(&s->cache);
    tbox_status_now(s->status.since, sizeof s->status.since);
    set_field(s->status.error, sizeof s->status.error, error);

    /* Observability only: the lock is what protects the session, so a failed
     * status write is worth a word but never fatal. */
    if (tbox_status_write(s->status_path, &s->status) != 0)
        fprintf(stderr, "serve: could not update %s\n", s->status_path);
}

/* ---- authorization ---------------------------------------------------------- */

static int await_auth(serve_t *s)
{
    long waited;
    long human;

    for (waited = 0, human = 0; waited < AUTH_TIMEOUT_MS;
         waited += WATCH_TICK_MS) {
        if (s->should_stop != NULL && *s->should_stop)
            return -1;

        if (s->auth == AUTH_READY)
            return 0;

        if (s->auth == AUTH_CLOSED) {
            fprintf(stderr, "serve: the TDLib session closed itself.\n");
            return -1;
        }

        if (s->auth == AUTH_NEEDS_LOGIN) {
            human += WATCH_TICK_MS;
            if (human >= AUTH_HUMAN_MS)
                break;              /* it is not on its way to Ready */
        } else {
            human = 0;
        }

        tbox_sleep_ms(WATCH_TICK_MS);
    }

    if (s->auth == AUTH_READY)
        return 0;

    if (human > 0)
        fprintf(stderr, "serve: this session is not authorized yet - "
                        "run `tbox auth` first.\n");
    else
        fprintf(stderr, "serve: Telegram did not become ready in %ld s.\n",
                (long)(AUTH_TIMEOUT_MS / 1000));

    return -1;
}

/* ---- the cache's fetcher: one file body -------------------------------------- */

static int copy_file(const char *src, const char *dst)
{
    FILE *in;
    FILE *out;
    char *buf;
    size_t n;
    int rc = 0;

    in = fopen(src, "rb");
    if (in == NULL)
        return -1;

    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }

    buf = malloc(COPY_CHUNK);
    if (buf == NULL) {
        fclose(in);
        fclose(out);
        return -1;
    }

    while ((n = fread(buf, 1, COPY_CHUNK, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }

    free(buf);
    if (fclose(out) != 0)
        rc = -1;
    fclose(in);

    if (rc != 0)
        remove(dst);

    return rc;
}

/*
 * TDLib's own reply can already say the body is on disk: after a previous
 * session the file may still be there, and then no download runs and no
 * updateFile ever arrives. Copies the path out when it does.
 */
static int reply_has_body(const cJSON *reply, char *path, size_t size)
{
    const cJSON *local = cJSON_GetObjectItem(reply, "local");
    const cJSON *done;
    const char *p;

    if (!cJSON_IsObject(local))
        return 0;

    /* cJSON_IsTrue, not cJSON_IsNumber: TDLib sends a real JSON boolean and
     * cJSON does not count a boolean as a number. */
    done = cJSON_GetObjectItem(local, "is_downloading_completed");
    if (!cJSON_IsTrue(done))
        return 0;

    p = cJSON_GetStringValue(cJSON_GetObjectItem(local, "path"));
    if (p == NULL || p[0] == '\0' || strlen(p) >= size)
        return 0;

    strcpy(path, p);
    return 1;
}

static int fetch_file(void *user, long long file_id, const char *dest_path)
{
    serve_t *s = (serve_t *)user;
    tbox_td_file_t f;
    char request[160];
    char why[256];
    char path[sizeof f.path];
    char *reply = NULL;
    long waited;
    int rc;

    if (s->bridge == NULL || file_id == 0)
        return -1;

    /* Reserved before the request, so an updateFile for this download is
     * recorded even if it lands before the reply does. */
    if (tbox_tdbridge_file_want(s->bridge, file_id) != 0) {
        fprintf(stderr, "serve: too many downloads in flight "
                        "(file %lld was not started).\n", file_id);
        return -1;
    }

    /* `file_id`, not `remote_file_id`: this generation of TDLib answers
     * downloadFile with File.id, and gives "File not found" for every other
     * spelling - including remote_file_id as the base64 string that
     * file.remote.id actually holds. */
    snprintf(request, sizeof request,
             "{\"@type\":\"downloadFile\",\"priority\":1,\"offset\":0,"
             "\"limit\":0,\"file_id\":%lld}", file_id);

    if (tbox_tdbridge_call(s->bridge, request, DOWNLOAD_TIMEOUT_MS, &reply)
        != TBOX_TDB_OK) {
        fprintf(stderr, "serve: downloadFile for file %lld did not answer.\n",
                file_id);
        tbox_tdbridge_file_forget(s->bridge, file_id);
        return -1;
    }

    if (tbox_td_reply_error(reply, why, sizeof why) != 0) {
        fprintf(stderr, "serve: Telegram refused to send file %lld: %s\n",
                file_id, why);
        free(reply);
        tbox_tdbridge_file_forget(s->bridge, file_id);
        return -1;
    }

    {
        cJSON *msg = cJSON_Parse(reply);
        int have = 0;

        if (msg != NULL) {
            have = reply_has_body(msg, path, sizeof path);
            cJSON_Delete(msg);
        }
        free(reply);

        if (have) {
            rc = copy_file(path, dest_path);
            tbox_tdbridge_file_forget(s->bridge, file_id);
            return rc;
        }
    }

    for (waited = 0; waited < DOWNLOAD_TIMEOUT_MS;
         waited += DOWNLOAD_POLL_MS) {
        if ((s->should_stop != NULL && *s->should_stop)
            || tbox_tdbridge_stopping(s->bridge)) {
            tbox_tdbridge_file_forget(s->bridge, file_id);
            return 1;           /* cancelled: the cache drops the part file */
        }

        if (tbox_tdbridge_file(s->bridge, file_id, &f) == 0) {
            if (f.completed && f.path[0] != '\0') {
                rc = copy_file(f.path, dest_path);
                tbox_tdbridge_file_forget(s->bridge, file_id);
                return rc;
            }
            if (f.failed) {
                fprintf(stderr, "serve: the download of file %lld failed.\n",
                        file_id);
                tbox_tdbridge_file_forget(s->bridge, file_id);
                return -1;
            }
        }

        tbox_sleep_ms(DOWNLOAD_POLL_MS);
    }

    fprintf(stderr, "serve: the download of file %lld did not finish in time.\n",
            file_id);
    tbox_tdbridge_file_forget(s->bridge, file_id);

    return -1;
}

/* ---- scanning --------------------------------------------------------------- */

/*
 * Walk Saved Messages into a fresh index, then make that the live one.
 *
 * Always from empty: a file deleted in Telegram has to disappear from the
 * tree, and an index seeded from the previous scan could never notice.
 */
static int rescan(serve_t *s, int announce)
{
    tbox_archive_t fresh;
    tbox_archive_t old;
    tbox_scan_stats_t stats;
    int rc;

    tbox_archive_init(&fresh);

    if (announce) {
        printf("serve: indexing Saved Messages...\n");
        fflush(stdout);
    }

    rc = tbox_td_scan(s->bridge, s->chat_id, s->should_stop, 0, &fresh, &stats);
    if (rc == TBOX_SCAN_ERROR) {
        tbox_archive_free(&fresh);
        return -1;
    }

    /* Swap under the server's lock: a session listing right now keeps seeing a
     * whole index, never a half-built one. */
    tbox_ftpd_archive_lock(s->ftpd);
    old = s->archive;
    s->archive = fresh;
    tbox_ftpd_archive_unlock(s->ftpd);

    tbox_archive_free(&old);

    (void)tbox_indexfile_write(s->index_path, &s->archive);
    publish(s, s->status.state, NULL);

    printf("serve: %lld entries from %lld pages (%lld messages walked)\n",
           stats.indexed, stats.pages, stats.scanned);
    if (rc == TBOX_SCAN_CANCELLED)
        printf("serve: scan cancelled - the index is partial.\n");
    fflush(stdout);

    return 0;
}

/*
 * Let a burst land before rescanning. Anything that arrives during the settle
 * is covered by the scan that follows, because an update during the scan
 * raises dirty again and the loop simply goes round once more.
 */
static void settle(serve_t *s)
{
    long waited;

    for (waited = 0; waited < RESCAN_SETTLE_MS; waited += WATCH_TICK_MS)
        tbox_sleep_ms(WATCH_TICK_MS);

    s->dirty = 0;
}

/* ---- the run --------------------------------------------------------------- */

int tbox_td_serve_run(const char *root, volatile bool *should_stop)
{
    serve_t s;
    tbox_td_params_t params;
    tbox_ftpd_opts_t opts;
    char token[TBOX_FTPD_TOKEN_LEN];
    char endpoint[64];
    char user[64];
    char phone[32];
    cJSON *request;
    char *text;
    int rc = -1;

    memset(&s, 0, sizeof s);
    memset(&params, 0, sizeof params);
    memset(&opts, 0, sizeof opts);
    s.should_stop = should_stop;
    tbox_archive_init(&s.archive);

    if (root == NULL || root[0] == '\0') {
        fprintf(stderr, "serve: no data directory.\n");
        return -1;
    }

    if (tbox_status_path(s.status_path, sizeof s.status_path, root) != 0
        || tbox_datadir_index(s.index_path, sizeof s.index_path, root) != 0) {
        fprintf(stderr, "serve: data path is too long.\n");
        return -1;
    }

    /* Adopt the status the command layer already wrote, so every state from
     * here carries the same pid and timestamp rather than a second, parallel
     * document. */
    if (tbox_status_read(s.status_path, &s.status) != 0)
        tbox_status_init(&s.status);

    /* --- 1. credentials and the session directory --- */
    if (strlen(root) >= sizeof params.data_dir) {
        fprintf(stderr, "serve: data path is too long.\n");
        publish(&s, TBOX_STATE_ERROR, "serve: data path is too long");
        return -1;
    }
    strcpy(params.data_dir, root);
    /* prepare_dir takes the root and resolves <root>/tdlib back into it */
    if (tbox_td_prepare_dir(&params) != 0) {
        fprintf(stderr, "serve: cannot use %s for the TDLib session.\n", root);
        publish(&s, TBOX_STATE_ERROR,
                "serve: cannot create the TDLib session directory");
        return -1;
    }
    /* load_credentials prints its own reason */
    if (tbox_td_load_credentials(&params) != 0) {
        publish(&s, TBOX_STATE_ERROR,
                "serve: TG_API_ID / TG_API_HASH are not set");
        return -1;
    }

    /* --- 2. TDLib up, and authorized --- */
    s.bridge = tbox_tdbridge_start();
    if (s.bridge == NULL) {
        fprintf(stderr, "serve: could not start the TDLib bridge.\n");
        return -1;
    }
    tbox_tdbridge_on_update(s.bridge, on_update, &s);

    request = tbox_td_build_parameters(&params);
    if (request == NULL) {
        fprintf(stderr, "serve: could not build the TDLib parameters.\n");
        goto done;
    }
    text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (text == NULL)
        goto done;
    (void)tbox_tdbridge_send(s.bridge, text);
    free(text);

    if (await_auth(&s) != 0) {
        publish(&s, TBOX_STATE_QR_REQUIRED,
                "serve: run `tbox auth` to authorize this session");
        goto done;
    }

    /* --- 3. which chat is the archive --- */
    if (tbox_td_scan_whoami(s.bridge, &s.chat_id, user, sizeof user,
                            phone, sizeof phone) != 0) {
        publish(&s, TBOX_STATE_ERROR,
                "serve: Telegram would not say who this account is");
        goto done;
    }
    set_field(s.status.user, sizeof s.status.user, user);
    set_field(s.status.phone, sizeof s.status.phone, phone);
    publish(&s, TBOX_STATE_AUTHORIZING, NULL);

    /* --- 4. the index mirror, so the tree exists before page 1 lands --- */
    {
        long long mirrored = tbox_indexfile_read(s.index_path, &s.archive);

        if (mirrored > 0)
            printf("serve: %lld entries from the previous run's index\n",
                   mirrored);
    }

    /* --- 5. the cache and the FTP server --- */
    if (tbox_cache_open(&s.cache, root, 0, fetch_file, &s) != 0) {
        publish(&s, TBOX_STATE_ERROR, "serve: cannot open the file cache");
        goto done;
    }

    opts.bind_addr = "127.0.0.1";
    opts.port = 0;                     /* let the OS pick a free one */
    opts.archive = &s.archive;
    opts.cache = &s.cache;

    s.ftpd = tbox_ftpd_start(&opts, token, sizeof token);
    if (s.ftpd == NULL) {
        publish(&s, TBOX_STATE_ERROR, "serve: could not start the FTP server");
        goto done;
    }

    snprintf(endpoint, sizeof endpoint, "127.0.0.1:%d", tbox_ftpd_port(s.ftpd));
    set_field(s.status.transport, sizeof s.status.transport, "ftp");
    set_field(s.status.endpoint, sizeof s.status.endpoint, endpoint);

    /* The token is the point of the loopback-only bind: print it, or nobody
     * can log in. */
    printf("\n  FTP    %s\n", endpoint);
    printf("  user   tbox\n");
    printf("  pass   %s\n", token);
    printf("\n  Point WinSCP or FileZilla at %s (FTP, not SFTP).\n", endpoint);
    printf("  Ctrl+C to stop.\n\n");
    fflush(stdout);

    publish(&s, TBOX_STATE_READY, NULL);

    /* --- 6. the real scan, then watch for changes --- */
    if (rescan(&s, 1) != 0) {
        publish(&s, TBOX_STATE_ERROR, "serve: the archive scan failed");
        goto done;
    }

    while (should_stop == NULL || !*should_stop) {
        if (!s.dirty) {
            tbox_sleep_ms(WATCH_TICK_MS);
            continue;
        }
        settle(&s);
        if (should_stop != NULL && *should_stop)
            break;
        if (rescan(&s, 0) != 0) {
            publish(&s, TBOX_STATE_ERROR, "serve: the archive rescan failed");
            goto done;
        }
    }

    rc = 0;

done:
    /* Stop serving first: a session thread may be inside the fetcher, and it
     * needs TDLib alive to finish. */
    if (s.ftpd != NULL) {
        tbox_ftpd_stop(s.ftpd);
        s.ftpd = NULL;
    }

    if (s.bridge != NULL) {
        long waited;

        /* TDLib wants a closed client, not a dropped one. The close result
         * arrives nested in updateAuthorizationState, so draining means
         * waiting for the handler to see authorizationStateClosed. */
        (void)tbox_tdbridge_send(s.bridge,
                                 "{\"@type\":\"close\",\"@extra\":\"tbox:bye\"}");
        for (waited = 0; waited < CLOSE_TIMEOUT_MS && s.auth != AUTH_CLOSED;
             waited += 50)
            tbox_sleep_ms(50);

        tbox_tdbridge_stop(s.bridge);
        s.bridge = NULL;
    }

    tbox_cache_close(&s.cache);
    tbox_archive_free(&s.archive);

    if (rc == 0)
        publish(&s, TBOX_STATE_STOPPED, NULL);
    else if (s.status.state != TBOX_STATE_QR_REQUIRED
             && s.status.state != TBOX_STATE_ERROR)
        publish(&s, TBOX_STATE_ERROR, "serve: stopped with an error");

    return rc;
}

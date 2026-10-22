/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_engine_download.c — the download over the whole engine (spec 8.6).
 *
 * The engine is driven for real: the loop runs in a thread, the commands go in
 * through tbox_engine_push_command() and every emitted line is captured. The
 * fake TDLib answers the requests the engine makes from an in-memory Saved
 * Messages chat, so the whole path - the command, the operation in the
 * journal, the request for the file, the copy, the events - runs without a
 * dll and without a network.
 *
 * What is checked:
 *   - file.download answers with the operation id right away (spec 6.2);
 *   - the file is asked for and the progress is reported (spec 6.5);
 *   - the file lands with the name of the record, and the operation is done;
 *   - a name that is taken is refused, and the file that is there stays;
 *   - the user may ask for both to be there: the second one gets a number;
 *   - a request that is missing something is refused with the right error.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/engine.h"
#include "engine/protocol.h"
#include "store/db.h"
#include "store/journal.h"
#include "util/fsx.h"
#include "xfer/ops.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define T_SLEEP(ms) Sleep(ms)
#else
#include <pthread.h>
#include <unistd.h>
#define T_SLEEP(ms) usleep((ms) * 1000)
#endif

#include "fake_chat.h"
#include "fake_td.h"

/* The profile and the folders the test uses. A macro, so the paths can be
 * written as one piece. */
#define DIR "tbox_engine_download_test"

/* The file that is in Saved Messages. */
#define OBJECT_ID "0123456789abcdef0123456789abcdef"

/* The hash of "hello" - the content of the file that comes back. */
#define HASH_HELLO \
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"

/* ---- captured output ------------------------------------------------- */

#define OUT_MAX 512
typedef struct Out {
    char *line[OUT_MAX];
    int n;
    int progress; /* how many transfer.progress events were seen */
    int done;     /* how many transfer.done events were seen */
    int failed;   /* how many transfer.failed events were seen */
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Out;

static void out_lock(Out *o) {
#ifdef _WIN32
    EnterCriticalSection(&o->lock);
#else
    pthread_mutex_lock(&o->lock);
#endif
}
static void out_unlock(Out *o) {
#ifdef _WIN32
    LeaveCriticalSection(&o->lock);
#else
    pthread_mutex_unlock(&o->lock);
#endif
}

static void capture(const char *json_line, void *ctx) {
    Out *o = ctx;
    out_lock(o);
    if (o->n < OUT_MAX) {
        o->line[o->n++] = _strdup(json_line);
    }
    if (strstr(json_line, "\"event\":\"transfer.progress\"") != NULL) {
        o->progress++;
    }
    if (strstr(json_line, "\"event\":\"transfer.done\"") != NULL) {
        o->done++;
    }
    if (strstr(json_line, "\"event\":\"transfer.failed\"") != NULL) {
        o->failed++;
    }
    out_unlock(o);
}

/* The first captured line that contains `needle` (and, when `needle2` is
 * given, both), or NULL. */
static const char *find_line(Out *o, const char *needle, const char *needle2) {
    out_lock(o);
    for (int i = 0; i < o->n; i++) {
        if (strstr(o->line[i], needle) != NULL &&
            (needle2 == NULL || strstr(o->line[i], needle2) != NULL)) {
            out_unlock(o);
            return o->line[i];
        }
    }
    out_unlock(o);
    return NULL;
}

/* Waits until a captured line matches; returns it or NULL. */
static const char *wait_for(Out *o, const char *needle, const char *needle2,
                            unsigned ms) {
    unsigned waited = 0;
    for (;;) {
        const char *l = find_line(o, needle, needle2);
        if (l != NULL) {
            return l;
        }
        if (waited >= ms) {
            return NULL;
        }
        T_SLEEP(5);
        waited += 5;
    }
}

/* Waits for the response to request `id` (the line carries "id":N). */
static const char *wait_for_id(Out *o, int id, unsigned ms) {
    char needle[32];
    snprintf(needle, sizeof needle, "\"id\":%d", id);
    return wait_for(o, needle, NULL, ms);
}

/* Waits until at least `n` events of a kind have been seen. */
static int wait_count(Out *o, int *counter, int n, unsigned ms) {
    unsigned waited = 0;
    for (;;) {
        out_lock(o);
        int seen = *counter;
        out_unlock(o);
        if (seen >= n) {
            return 1;
        }
        if (waited >= ms) {
            return 0;
        }
        T_SLEEP(5);
        waited += 5;
    }
}

static void out_free(Out *o) {
    for (int i = 0; i < o->n; i++) {
        free(o->line[i]);
    }
#ifdef _WIN32
    DeleteCriticalSection(&o->lock);
#else
    pthread_mutex_destroy(&o->lock);
#endif
}

/* ---- the fake TDLib answering the engine's requests ------------------ */

/* The world the responder sees. */
typedef struct World {
    FakeChat *chat;
    FakeTd *fake;
    int download_calls;   /* how many files the engine asked for */
    char local_path[512]; /* where the file is on this disk */
} World;

static long long json_num(const cJSON *root, const char *name, long long fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : fallback;
}

/* The update that says the file is here (spec 8.6 step 3). */
static void enqueue_file_ready(World *w, long long file_id, long long size) {
    cJSON *upd = cJSON_CreateObject();
    if (upd == NULL) {
        return;
    }
    cJSON_AddStringToObject(upd, "@type", "updateFile");
    cJSON *file = cJSON_AddObjectToObject(upd, "file");
    cJSON_AddStringToObject(file, "@type", "file");
    cJSON_AddNumberToObject(file, "id", (double)file_id);
    cJSON_AddNumberToObject(file, "size", (double)size);
    cJSON *local = cJSON_AddObjectToObject(file, "local");
    cJSON_AddStringToObject(local, "@type", "localFile");
    cJSON_AddBoolToObject(local, "is_downloading_completed", 1);
    cJSON_AddStringToObject(local, "path", w->local_path);
    cJSON_AddNumberToObject(local, "downloaded_size", (double)size);
    char *json = cJSON_PrintUnformatted(upd);
    cJSON_Delete(upd);
    if (json != NULL) {
        fake_td_enqueue(w->fake, json);
        free(json);
    }
}

/* Answers one request. The fake puts the request's @extra on the reply. */
static char *answer(void *ctx, const char *request) {
    World *w = ctx;
    cJSON *root = cJSON_Parse(request);
    if (root == NULL) {
        return NULL;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "@type");
    const char *t = cJSON_IsString(type) ? cJSON_GetStringValue(type) : "";
    char *out = NULL;
    if (strcmp(t, "getMe") == 0) {
        out = _strdup("{\"@type\":\"user\",\"id\":4242,\"first_name\":\"Test\","
                      "\"last_name\":\"User\",\"is_premium\":true}");
    } else if (strcmp(t, "createPrivateChat") == 0) {
        out = _strdup("{\"@type\":\"chat\",\"id\":1}");
    } else if (strcmp(t, "getOption") == 0) {
        out = _strdup("{\"@type\":\"optionValue\",\"value\":"
                      "{\"@type\":\"optionValueInteger\",\"value\":1024}}");
    } else if (strcmp(t, "getChatHistory") == 0) {
        long long from = json_num(root, "from_message_id", 0);
        long long limit = json_num(root, "limit", 100);
        out = fake_chat_history_json(w->chat, from, 0, (int)limit);
    } else if (strcmp(t, "getMessage") == 0) {
        long long id = json_num(root, "message_id", 0);
        out = fake_chat_message_json(w->chat, id);
    } else if (strcmp(t, "downloadFile") == 0) {
        w->download_calls++;
        long long file_id = json_num(root, "file_id", 0);
        cJSON *file = cJSON_CreateObject();
        if (file != NULL) {
            cJSON_AddStringToObject(file, "@type", "file");
            cJSON_AddNumberToObject(file, "id", (double)file_id);
            cJSON_AddNumberToObject(file, "size", 5);
            cJSON *local = cJSON_AddObjectToObject(file, "local");
            cJSON_AddStringToObject(local, "@type", "localFile");
            cJSON_AddBoolToObject(local, "is_downloading_completed", 0);
            cJSON_AddStringToObject(local, "path", "");
            out = cJSON_PrintUnformatted(file);
            cJSON_Delete(file);
        }
        /* the file arrives a moment later */
        enqueue_file_ready(w, file_id, 5);
    } else if (strcmp(t, "close") == 0) {
        out = _strdup("{\"@type\":\"ok\"}");
    } else {
        out = _strdup("{\"@type\":\"ok\"}");
    }
    cJSON_Delete(root);
    return out;
}

/* ---- engine thread --------------------------------------------------- */

typedef struct Run {
    TBoxEngine *e;
    int rc;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
} Run;

#ifdef _WIN32
static unsigned __stdcall run_main(void *arg) {
#else
static void *run_main(void *arg) {
#endif
    Run *r = arg;
    r->rc = tbox_engine_run(r->e);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void run_in_thread(Run *r) {
#ifdef _WIN32
    uintptr_t h = _beginthreadex(NULL, 0, run_main, r, 0, NULL);
    assert(h != 0);
    r->thread = (HANDLE)h;
#else
    pthread_create(&r->thread, NULL, run_main, r);
#endif
}

static void join_thread(Run *r) {
#ifdef _WIN32
    assert(WaitForSingleObject(r->thread, 30000) == WAIT_OBJECT_0);
    CloseHandle(r->thread);
#else
    pthread_join(r->thread, NULL);
#endif
}

/* ---- the operations in the journal ------------------------------------ */

/* The state an operation is in, read from the journal. */
static void op_state(TBoxDb *db, const char *op_id, char *out, size_t cap) {
    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    assert(tbox_journal_get(db, op_id, &row, &found) == TBOX_DB_OK);
    if (!found) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%s", row.state);
    tbox_journal_free_row(&row);
}

/* Waits until an operation reaches this state. */
static int wait_op_state(TBoxDb *db, const char *op_id, const char *want,
                         unsigned ms) {
    unsigned waited = 0;
    char state[32];
    for (;;) {
        op_state(db, op_id, state, sizeof state);
        if (strcmp(state, want) == 0) {
            return 1;
        }
        if (waited >= ms) {
            return 0;
        }
        T_SLEEP(10);
        waited += 10;
    }
}

/* The id of the operation out of an answer to file.download. */
static int op_id_of(const char *line, char *out, size_t cap) {
    const char *p = strstr(line, "\"op_id\":\"");
    if (p == NULL) {
        return 0;
    }
    p += strlen("\"op_id\":\"");
    size_t i = 0;
    while (p[i] != '\0' && p[i] != '"' && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

/* Writes a file with the given content. */
static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (content != NULL && content[0] != '\0') {
        assert(fwrite(content, 1, strlen(content), f) == strlen(content));
    }
    fclose(f);
}

int main(void) {
    /* Start from a clean profile: a profile that already holds a complete
     * scan reconciles instead of scanning (spec 8.1). */
    tbox_fsx_remove("tbox_engine_download_test/index.db");
    tbox_fsx_remove("tbox_engine_download_test/index.db-wal");
    tbox_fsx_remove("tbox_engine_download_test/index.db-shm");
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);
    assert(tbox_fsx_mkdirs(DIR "/out") == TBOX_FSX_OK);
    assert(tbox_fsx_mkdirs(DIR "/remote") == TBOX_FSX_OK);

    /* what a run of this test leaves behind */
    tbox_fsx_remove(DIR "/out/hello.txt");
    tbox_fsx_remove(DIR "/out/hello (2).txt");
    tbox_fsx_remove(DIR "/out/.hello.txt.tbox-part");

    /* the file Telegram has */
    write_file(DIR "/remote/hello.txt", "hello");

    /* ---- the Saved Messages chat the fake serves --------------------- */
    FakeChat *chat = fake_chat_new();
    char caption[512];
    snprintf(caption, sizeof caption,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"hello.txt\",\"r\":1,\"s\":5,\"h\":\"%s\"}",
             OBJECT_ID, HASH_HELLO);
    fake_chat_add_media(chat, "document", "hello.txt", 5, 9002, caption);

    World world;
    memset(&world, 0, sizeof world);
    world.chat = chat;
    snprintf(world.local_path, sizeof world.local_path, "%s",
             DIR "/remote/hello.txt");

    FakeTd *fake = fake_td_start(4242);
    fake->responder = answer;
    fake->responder_ctx = &world;
    world.fake = fake;

    Out out;
    memset(&out, 0, sizeof out);
#ifdef _WIN32
    InitializeCriticalSection(&out.lock);
#else
    pthread_mutex_init(&out.lock, NULL);
#endif

    TBoxEngineConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.profile_dir = DIR;

    int err = -1;
    TBoxEngine *e = tbox_engine_new(&fake->api, &cfg, capture, &out, &err);
    assert(e != NULL && err == 0);

    Run r;
    memset(&r, 0, sizeof r);
    r.e = e;
    run_in_thread(&r);

    /* ---- login ------------------------------------------------------- */
    tbox_engine_push_command(e,
        "{\"id\":1,\"cmd\":\"auth.start\",\"args\":{\"api_id\":\"123\","
        "\"api_hash\":\"deadbeef\"}}");
    assert(wait_for_id(&out, 1, 3000) != NULL);

    fake_td_enqueue(fake,
        "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":"
        "{\"@type\":\"authorizationStateReady\"}}");
    assert(wait_for(&out, "\"event\":\"auth.ready\"", NULL, 5000) != NULL);
    assert(wait_for(&out, "\"event\":\"sync.done\"", NULL, 5000) != NULL);

    TBoxDb *db = NULL;
    assert(tbox_db_open(DIR, &db) == TBOX_DB_OK);

    /* ---- the download (spec 8.6) ------------------------------------- */
    tbox_engine_push_command(e,
        "{\"id\":2,\"cmd\":\"file.download\",\"args\":{\"object_id\":\"" OBJECT_ID
        "\",\"dest_dir\":\"" DIR "/out\"}}");
    const char *l = wait_for_id(&out, 2, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    char op_id[64];
    assert(op_id_of(l, op_id, sizeof op_id) == 1);
    assert(strlen(op_id) == 16);

    /* the progress and the end of the download are reported (spec 6.5) */
    int got_progress = wait_count(&out, &out.progress, 1, 5000);
    if (!got_progress) {
        for (int i = 0; i < out.n; i++) {
            printf("LINE %d: %s\n", i, out.line[i]);
        }
        printf("download_calls=%d\n", world.download_calls);
        fflush(stdout);
    }
    assert(got_progress == 1);
    l = wait_for(&out, "\"event\":\"transfer.done\"", NULL, 5000);
    assert(l != NULL);
    assert(strstr(l, op_id) != NULL);
    assert(strstr(l, "\"path\":") != NULL);
    assert(strstr(l, "hello.txt") != NULL);

    /* the file is on this disk, with the name of the record */
    TBoxFileInfo info;
    assert(tbox_fsx_file_info(DIR "/out/hello.txt", &info) == TBOX_FSX_OK);
    assert(info.exists && info.size == 5);
    assert(world.download_calls == 1);

    /* the temporary file is gone */
    assert(tbox_fsx_file_info(DIR "/out/.hello.txt.tbox-part", &info) ==
           TBOX_FSX_OK);
    assert(!info.exists);

    /* the operation is done (spec 8.4) */
    assert(wait_op_state(db, op_id, "DONE", 5000) == 1);

    /* ---- a name that is taken is refused ------------------------------ */
    tbox_engine_push_command(e,
        "{\"id\":3,\"cmd\":\"file.download\",\"args\":{\"object_id\":\"" OBJECT_ID
        "\",\"dest_dir\":\"" DIR "/out\"}}");
    l = wait_for_id(&out, 3, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    char op2[64];
    assert(op_id_of(l, op2, sizeof op2) == 1);
    assert(wait_op_state(db, op2, "FAILED", 10000) == 1);
    l = wait_for(&out, "\"event\":\"transfer.failed\"", NULL, 5000);
    assert(l != NULL && strstr(l, "NAME_EXISTS") != NULL);
    /* the file that is there was not touched */
    assert(tbox_fsx_file_info(DIR "/out/hello.txt", &info) == TBOX_FSX_OK);
    assert(info.exists && info.size == 5);

    /* ---- the user may ask for both to be there ------------------------ */
    tbox_engine_push_command(e,
        "{\"id\":4,\"cmd\":\"file.download\",\"args\":{\"object_id\":\"" OBJECT_ID
        "\",\"dest_dir\":\"" DIR "/out\",\"on_exists\":\"keep_both\"}}");
    l = wait_for_id(&out, 4, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    char op3[64];
    assert(op_id_of(l, op3, sizeof op3) == 1);
    assert(wait_op_state(db, op3, "DONE", 10000) == 1);
    assert(tbox_fsx_file_info(DIR "/out/hello (2).txt", &info) == TBOX_FSX_OK);
    assert(info.exists && info.size == 5);

    /* ---- a request that is missing something ------------------------- */
    tbox_engine_push_command(e,
        "{\"id\":5,\"cmd\":\"file.download\",\"args\":{\"object_id\":\"" OBJECT_ID
        "\"}}");
    l = wait_for_id(&out, 5, 5000);
    assert(l != NULL && strstr(l, "BAD_ARGS") != NULL);

    /* a destination that is not there */
    tbox_engine_push_command(e,
        "{\"id\":6,\"cmd\":\"file.download\",\"args\":{\"object_id\":\"" OBJECT_ID
        "\",\"dest_dir\":\"" DIR "/nothing\"}}");
    l = wait_for_id(&out, 6, 5000);
    assert(l != NULL && strstr(l, "IO_ERROR") != NULL);

    /* a file that is not there */
    tbox_engine_push_command(e,
        "{\"id\":7,\"cmd\":\"file.download\",\"args\":{\"object_id\":"
        "\"ffffffffffffffffffffffffffffffff\",\"dest_dir\":\"" DIR "/out\"}}");
    l = wait_for_id(&out, 7, 5000);
    assert(l != NULL && strstr(l, "NOT_FOUND") != NULL);

    /* a mode that does not exist */
    tbox_engine_push_command(e,
        "{\"id\":8,\"cmd\":\"file.download\",\"args\":{\"object_id\":\"" OBJECT_ID
        "\",\"dest_dir\":\"" DIR "/out\",\"on_exists\":\"overwrite\"}}");
    l = wait_for_id(&out, 8, 5000);
    assert(l != NULL && strstr(l, "BAD_ARGS") != NULL);

    /* nothing was asked of Telegram for those */
    assert(world.download_calls == 3);

    /* ---- shutdown ----------------------------------------------------- */
    tbox_engine_push_command(e, "{\"id\":9,\"cmd\":\"engine.shutdown\"}");
    assert(wait_for_id(&out, 9, 5000) != NULL);
    join_thread(&r);
    assert(r.rc == 0);

    tbox_engine_free(e);
    tbox_db_close(db);
    fake_td_free(fake);
    fake_chat_free(chat);
    out_free(&out);
    printf("test_engine_download: ok\n");
    return 0;
}

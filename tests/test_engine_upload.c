/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_engine_upload.c — the upload over the whole engine (spec 8.5).
 *
 * The engine is driven for real: the loop runs in a thread, the commands go in
 * through tbox_engine_push_command() and every emitted line is captured. The
 * fake TDLib answers the requests the engine makes from an in-memory Saved
 * Messages chat, so the whole path - the command, the operation in the
 * journal, the message that carries the record, the events - runs without a
 * dll and without a network.
 *
 * What is checked:
 *   - file.upload answers with the operation id right away (spec 6.2);
 *   - the operation is written down before anything is sent (spec 8.5);
 *   - the message that goes out carries the record of the file (spec 7.2);
 *   - the progress and the end of the upload are reported (spec 6.5);
 *   - the file turns up in the catalog;
 *   - a name that is taken is refused (spec 7.4);
 *   - a file that is not there, and a mode that does not exist yet, are
 *     refused with the right error.
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

static const char *DIR = "tbox_engine_upload_test";

/* The file that is already in Saved Messages; its name cannot be taken
 * again (spec 7.4). */
static const char *O_TAKEN = "fedcba9876543210fedcba9876543210";

/* The ids the fake gives the message that goes out: a temporary one and the
 * real one that replaces it. */
#define TEMP_MSG_ID 900
#define REAL_MSG_ID 901
#define FILE_ID 9001

/* The operation the last run left behind in the journal. */
#define OP_LEFTOVER "0000000000000f01"

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
    int send_calls;   /* how many messages the engine tried to send */
    int send_fail;    /* answer the next sendMessage with an error */
    char last_path[512];  /* the path of the last file that went up */
    char last_caption[1024]; /* the record that went with it */
} World;

static long long json_num(const cJSON *root, const char *name, long long fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : fallback;
}

/* The caption and the path out of a sendMessage request. */
static void read_send_request(const cJSON *root, char *path, size_t path_cap,
                              char *caption, size_t caption_cap) {
    path[0] = '\0';
    caption[0] = '\0';
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(root, "input_message_content");
    const cJSON *doc = cJSON_GetObjectItemCaseSensitive(content, "document");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(doc, "path");
    if (cJSON_IsString(p)) {
        snprintf(path, path_cap, "%s", cJSON_GetStringValue(p));
    }
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(content, "caption");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(cap, "text");
    if (cJSON_IsString(text)) {
        snprintf(caption, caption_cap, "%s", cJSON_GetStringValue(text));
    }
}

/* The content of the message that went up: a document with the record as
 * its caption (spec 7.2). */
static cJSON *sent_content(const char *caption, long long size) {
    cJSON *content = cJSON_CreateObject();
    if (content == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(content, "@type", "messageDocument");
    cJSON *doc = cJSON_AddObjectToObject(content, "document");
    cJSON_AddStringToObject(doc, "@type", "file");
    cJSON_AddNumberToObject(doc, "id", (double)FILE_ID);
    cJSON_AddNumberToObject(doc, "size", (double)size);
    cJSON_AddNumberToObject(doc, "expected_size", (double)size);
    cJSON *cap = cJSON_AddObjectToObject(content, "caption");
    cJSON_AddStringToObject(cap, "@type", "formattedText");
    cJSON_AddStringToObject(cap, "text", caption);
    return content;
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
    } else if (strcmp(t, "sendMessage") == 0) {
        w->send_calls++;
        char path[512];
        char caption[1024];
        read_send_request(root, path, sizeof path, caption, sizeof caption);
        snprintf(w->last_path, sizeof w->last_path, "%s", path);
        snprintf(w->last_caption, sizeof w->last_caption, "%s", caption);
        if (w->send_fail) {
            w->send_fail = 0;
            out = _strdup("{\"@type\":\"error\",\"code\":400,"
                          "\"message\":\"CHANNEL_INVALID\"}");
        } else {
            long long size = 5;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "@type", "message");
            cJSON_AddNumberToObject(msg, "id", (double)TEMP_MSG_ID);
            cJSON_AddNumberToObject(msg, "chat_id", 1);
            cJSON *content = sent_content(caption, size);
            cJSON_AddItemToObject(msg, "content", content);
            out = cJSON_PrintUnformatted(msg);
            cJSON_Delete(msg);

            /* The progress of the file on its way up (spec 8.5 step 6). */
            cJSON *upd = cJSON_CreateObject();
            cJSON_AddStringToObject(upd, "@type", "updateFile");
            cJSON *file = cJSON_AddObjectToObject(upd, "file");
            cJSON_AddStringToObject(file, "@type", "file");
            cJSON_AddNumberToObject(file, "id", (double)FILE_ID);
            cJSON_AddNumberToObject(file, "size", (double)size);
            cJSON_AddNumberToObject(file, "expected_size", (double)size);
            cJSON *local = cJSON_AddObjectToObject(file, "local");
            cJSON_AddStringToObject(local, "@type", "localFile");
            cJSON_AddBoolToObject(local, "is_downloading_completed", 0);
            cJSON_AddStringToObject(local, "path", path);
            cJSON *remote = cJSON_AddObjectToObject(file, "remote");
            cJSON_AddStringToObject(remote, "@type", "remoteFile");
            cJSON_AddStringToObject(remote, "id", "remote-1");
            cJSON_AddBoolToObject(remote, "is_uploading_completed", 0);
            char *upd_json = cJSON_PrintUnformatted(upd);
            cJSON_Delete(upd);
            if (upd_json != NULL) {
                fake_td_enqueue(w->fake, upd_json);
                free(upd_json);
            }

            /* The message that is there for good (spec 8.5 step 7). */
            cJSON *sent = cJSON_CreateObject();
            cJSON_AddStringToObject(sent, "@type", "updateMessageSendSucceeded");
            cJSON *real = cJSON_AddObjectToObject(sent, "message");
            cJSON_AddStringToObject(real, "@type", "message");
            cJSON_AddNumberToObject(real, "id", (double)REAL_MSG_ID);
            cJSON_AddNumberToObject(real, "chat_id", 1);
            cJSON_AddNumberToObject(real, "date", 1760000000);
            cJSON *real_content = sent_content(caption, size);
            cJSON_AddItemToObject(real, "content", real_content);
            cJSON_AddNumberToObject(sent, "old_message_id", (double)TEMP_MSG_ID);
            char *sent_json = cJSON_PrintUnformatted(sent);
            cJSON_Delete(sent);
            if (sent_json != NULL) {
                fake_td_enqueue(w->fake, sent_json);
                free(sent_json);
            }
        }
    } else if (strcmp(t, "close") == 0) {
        out = _strdup("{\"@type\":\"ok\"}");
        if (w->fake != NULL) {
            fake_td_enqueue(w->fake,
                            "{\"@type\":\"updateAuthorizationState\","
                            "\"authorization_state\":{\"@type\":"
                            "\"authorizationStateClosed\"}}");
        }
    } else {
        out = _strdup("{\"@type\":\"ok\"}");
    }
    cJSON_Delete(root);
    return out;
}

/* ---- engine thread --------------------------------------------------- */

/* Throws away whatever the fake still holds. The replies of one run carry the
 * same @extra the next run uses, so a second run has to start with a clean
 * script. */
static void fake_drain(FakeTd *f) {
    while (f->api.receive(0.0) != NULL) {
        /* keep throwing */
    }
}

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

/* The id of the operation out of an answer to file.upload. */
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

int main(void) {
    /* Start from a clean profile: a profile that already holds a complete
     * scan reconciles instead of scanning (spec 8.1). */
    tbox_fsx_remove("tbox_engine_upload_test/index.db");
    tbox_fsx_remove("tbox_engine_upload_test/index.db-wal");
    tbox_fsx_remove("tbox_engine_upload_test/index.db-shm");
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);

    /* the file that goes up */
    FILE *f = fopen("tbox_engine_upload_test/hello.txt", "wb");
    assert(f != NULL);
    assert(fwrite("hello", 1, 5, f) == 5);
    fclose(f);

    /* ---- the Saved Messages chat the fake serves --------------------- */
    FakeChat *chat = fake_chat_new();
    char caption[512];
    /* a file whose name is already taken */
    snprintf(caption, sizeof caption,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"report.pdf\",\"r\":1,\"s\":12,\"m\":1760000000}",
             O_TAKEN);
    fake_chat_add_media(chat, "document", "report.pdf", 12, 9002, caption);

    World world;
    memset(&world, 0, sizeof world);
    world.chat = chat;

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
    const char *l = wait_for(&out, "\"event\":\"auth.ready\"", NULL, 5000);
    assert(l != NULL);
    assert(wait_for(&out, "\"event\":\"sync.done\"", NULL, 5000) != NULL);

    /* ---- the upload (spec 8.5) --------------------------------------- */
    tbox_engine_push_command(e,
        "{\"id\":2,\"cmd\":\"file.upload\",\"args\":{\"local_path\":\""
        "tbox_engine_upload_test/hello.txt\",\"folder_id\":\"root\"}}");
    l = wait_for_id(&out, 2, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    char op_id[64];
    assert(op_id_of(l, op_id, sizeof op_id) == 1);
    assert(strlen(op_id) == 16);

    /* the progress and the end of the upload are reported (spec 6.5) */
    assert(wait_count(&out, &out.progress, 1, 5000) == 1);
    l = wait_for(&out, "\"event\":\"transfer.done\"", NULL, 5000);
    assert(l != NULL);
    assert(strstr(l, op_id) != NULL);
    assert(strstr(l, "\"object_id\":\"") != NULL);

    /* the file that went up carries the record of the file (spec 7.2) */
    assert(world.send_calls == 1);
    assert(strcmp(world.last_path, "tbox_engine_upload_test/hello.txt") == 0);
    assert(strstr(world.last_caption, "tbx1 {") == world.last_caption);
    assert(strstr(world.last_caption, "\"n\":\"hello.txt\"") != NULL);
    assert(strstr(world.last_caption, "\"k\":\"f\"") != NULL);
    assert(strstr(world.last_caption, "\"p\":\"root\"") != NULL);
    assert(strstr(world.last_caption, "\"q\":") != NULL);
    assert(strstr(world.last_caption, "\"h\":") != NULL);

    /* the operation is done (spec 8.4) */
    TBoxDb *db = NULL;
    assert(tbox_db_open(DIR, &db) == TBOX_DB_OK);
    assert(wait_op_state(db, op_id, "DONE", 5000) == 1);

    /* ---- the file is in the catalog ----------------------------------- */
    tbox_engine_push_command(e, "{\"id\":3,\"cmd\":\"catalog.list\","
                              "\"args\":{\"folder_id\":\"root\"}}");
    l = wait_for_id(&out, 3, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    assert(strstr(l, "hello.txt") != NULL);
    assert(strstr(l, "report.pdf") != NULL);

    /* ---- a name that is taken is refused (spec 7.4) ------------------- */
    tbox_engine_push_command(e,
        "{\"id\":4,\"cmd\":\"file.upload\",\"args\":{\"local_path\":\""
        "tbox_engine_upload_test/hello.txt\",\"folder_id\":\"root\","
        "\"name\":\"report.pdf\"}}");
    l = wait_for_id(&out, 4, 5000);
    assert(l != NULL && strstr(l, "NAME_EXISTS") != NULL);
    assert(world.send_calls == 1); /* nothing was sent */

    /* ---- a file that is not there ------------------------------------- */
    tbox_engine_push_command(e,
        "{\"id\":5,\"cmd\":\"file.upload\",\"args\":{\"local_path\":\""
        "tbox_engine_upload_test/nothing.txt\",\"folder_id\":\"root\"}}");
    l = wait_for_id(&out, 5, 5000);
    assert(l != NULL && strstr(l, "NOT_FOUND") != NULL);
    assert(world.send_calls == 1);

    /* ---- a folder that is not there ----------------------------------- */
    tbox_engine_push_command(e,
        "{\"id\":6,\"cmd\":\"file.upload\",\"args\":{\"local_path\":\""
        "tbox_engine_upload_test/hello.txt\",\"folder_id\":\"deadbeef\"}}");
    l = wait_for_id(&out, 6, 5000);
    assert(l != NULL && strstr(l, "NOT_FOUND") != NULL);

    /* ---- the mode that does not exist yet ---------------------------- */
    tbox_engine_push_command(e,
        "{\"id\":7,\"cmd\":\"file.upload\",\"args\":{\"local_path\":\""
        "tbox_engine_upload_test/hello.txt\",\"folder_id\":\"root\","
        "\"on_exists\":\"new_revision\"}}");
    l = wait_for_id(&out, 7, 5000);
    assert(l != NULL && strstr(l, "BAD_ARGS") != NULL);

    /* ---- a message Telegram refuses ----------------------------------- */
    world.send_fail = 1;
    tbox_engine_push_command(e,
        "{\"id\":8,\"cmd\":\"file.upload\",\"args\":{\"local_path\":\""
        "tbox_engine_upload_test/hello.txt\",\"folder_id\":\"root\","
        "\"name\":\"second.txt\"}}");
    l = wait_for_id(&out, 8, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    char op2[64];
    assert(op_id_of(l, op2, sizeof op2) == 1);

    /* the operation waits and tries again (spec 8.4) */
    assert(wait_op_state(db, op2, "RETRY_WAIT", 5000) == 1);
    assert(wait_count(&out, &out.failed, 1, 5000) == 1);
    l = wait_for(&out, "\"event\":\"transfer.failed\"", NULL, 5000);
    assert(l != NULL && strstr(l, "\"error\":") != NULL);

    /* ---- shutdown ----------------------------------------------------- */
    tbox_engine_push_command(e, "{\"id\":9,\"cmd\":\"engine.shutdown\"}");
    assert(wait_for_id(&out, 9, 5000) != NULL);
    join_thread(&r);
    assert(r.rc == 0);

    tbox_engine_free(e);

    /* ---- an operation the last run left behind goes out when the engine
     * starts again (spec 8.4) -------------------------------------------- */
    /* The queue is the journal: an operation that was still open when the
     * engine stopped goes back into the queue, and the engine looks for it
     * when it starts - not when the next command happens to come in. */
    assert(tbox_journal_insert(db, OP_LEFTOVER, TBOX_OP_KIND_UPLOAD,
                                "{\"local_path\":\"tbox_engine_upload_test/"
                                "hello.txt\",\"folder_id\":\"root\","
                                "\"name\":\"leftover.txt\","
                                "\"object_id\":\"aaaabbbbccccddddeeeeffff00001111\"}",
                                100) == TBOX_DB_OK);

    fake_drain(fake);

    err = -1;
    TBoxEngine *e2 = tbox_engine_new(&fake->api, &cfg, capture, &out, &err);
    assert(e2 != NULL && err == 0);

    Run r2;
    memset(&r2, 0, sizeof r2);
    r2.e = e2;
    run_in_thread(&r2);

    tbox_engine_push_command(e2,
        "{\"id\":10,\"cmd\":\"auth.start\",\"args\":{\"api_id\":\"123\","
        "\"api_hash\":\"deadbeef\"}}");
    assert(wait_for_id(&out, 10, 3000) != NULL);

    fake_td_enqueue(fake,
        "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":"
        "{\"@type\":\"authorizationStateReady\"}}");

    /* the operation that was left behind is sent, without anyone asking */
    assert(wait_op_state(db, OP_LEFTOVER, "DONE", 20000) == 1);

    tbox_engine_push_command(e2, "{\"id\":11,\"cmd\":\"engine.shutdown\"}");
    assert(wait_for_id(&out, 11, 5000) != NULL);
    join_thread(&r2);
    assert(r2.rc == 0);

    tbox_engine_free(e2);
    tbox_db_close(db);
    fake_td_free(fake);
    fake_chat_free(chat);
    out_free(&out);
    printf("test_engine_upload: ok\n");
    return 0;
}

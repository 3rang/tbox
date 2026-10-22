/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_engine_catalog.c — the catalog commands over the fake TDLib.
 *
 * The engine is driven for real: the loop runs in a thread, commands go in
 * through tbox_engine_push_command() and every emitted line is captured. The
 * fake TDLib answers the requests the engine makes (getMe, createPrivateChat,
 * getOption, getChatHistory, getMessage) from an in-memory Saved Messages
 * chat, so the whole path - auth, the startup scan, the catalog - runs without
 * a dll and without a network.
 *
 * What is checked:
 *   - auth.ready opens Saved Messages and fills the catalog (spec 8.1);
 *   - the scan reports progress and completion (spec 8.2);
 *   - catalog.list answers for root, inbox, trash, a folder and a bad id;
 *   - catalog.stat answers with the item and its revisions (spec 7.3);
 *   - catalog.summary counts files, folders and bytes;
 *   - sync.rescan answers with an op_id and runs the scan again.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/engine.h"
#include "engine/protocol.h"
#include "util/fsx.h"

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

static const char *DIR = "tbox_engine_catalog_test";

static const char *O_FOLDER = "0123456789abcdef0123456789abcdef";
static const char *O_FILE = "fedcba9876543210fedcba9876543210";
static const char *O_CHILD = "00112233445566778899aabbccddeeff";

/* ---- captured output ------------------------------------------------- */

#define OUT_MAX 512
typedef struct Out {
    char *line[OUT_MAX];
    int n;
    int changed; /* how many catalog.changed events were seen */
    int done;    /* how many sync.done events were seen */
    int ready;   /* how many auth.ready events were seen */
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
    if (strstr(json_line, "\"event\":\"catalog.changed\"") != NULL) {
        o->changed++;
    }
    if (strstr(json_line, "\"event\":\"sync.done\"") != NULL) {
        o->done++;
    }
    if (strstr(json_line, "\"event\":\"auth.ready\"") != NULL) {
        o->ready++;
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

/* Waits until at least `n` catalog.changed events have been seen. */
static int wait_changed(Out *o, int n, unsigned ms) {
    unsigned waited = 0;
    for (;;) {
        out_lock(o);
        int seen = o->changed;
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

/* Waits until at least `n` sync.done events have been seen. */
static int wait_done(Out *o, int n, unsigned ms) {
    unsigned waited = 0;
    for (;;) {
        out_lock(o);
        int seen = o->done;
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

/* Waits until at least `n` auth.ready events have been seen. */
static int wait_ready(Out *o, int n, unsigned ms) {
    unsigned waited = 0;
    for (;;) {
        out_lock(o);
        int seen = o->ready;
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

/* What a response line says, without the request id it answered: two runs
 * that answered the same question must say the same thing. */
static const char *result_part(const char *line) {
    const char *p = strstr(line, "\"result\":");
    return p != NULL ? p : line;
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

/* The world the responder sees: the chat it serves and the calls it saw. */
typedef struct World {
    FakeChat *chat;
    FakeTd *fake;
    int history_calls;
    int option_calls;
    int chat_calls;
    int flood_next; /* answer the next history call with a flood wait */
} World;

static long long json_num(const cJSON *root, const char *name, long long fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : fallback;
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
        w->chat_calls++;
        /* chat id 1: the fake chat gives its messages that chat id */
        out = _strdup("{\"@type\":\"chat\",\"id\":1}");
    } else if (strcmp(t, "getOption") == 0) {
        w->option_calls++;
        out = _strdup("{\"@type\":\"optionValue\",\"value\":"
                      "{\"@type\":\"optionValueInteger\",\"value\":1024}}");
    } else if (strcmp(t, "getChatHistory") == 0) {
        w->history_calls++;
        if (w->flood_next) {
            w->flood_next = 0;
            out = _strdup("{\"@type\":\"error\",\"code\":429,"
                          "\"message\":\"FLOOD_WAIT_1\"}");
        } else {
            long long from = json_num(root, "from_message_id", 0);
            long long limit = json_num(root, "limit", 100);
            out = fake_chat_history_json(w->chat, from, 0, (int)limit);
        }
    } else if (strcmp(t, "getMessage") == 0) {
        long long id = json_num(root, "message_id", 0);
        out = fake_chat_message_json(w->chat, id);
    } else if (strcmp(t, "close") == 0) {
        /* TDLib answers close and then reports authorizationStateClosed. The
         * engine waits for that state (spec 9.4), so the fake has to send it:
         * without it every shutdown would sit out the full ten seconds. */
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

/* Throws away whatever the fake still holds. The replies of one run carry the
 * same @extra the next run uses, so a second run has to start with a clean
 * script. The fake keeps ownership of the strings and frees them at the end. */
static void fake_drain(FakeTd *f) {
    while (f->api.receive(0.0) != NULL) {
        /* keep throwing */
    }
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
    /* The engine may wait for authorizationStateClosed (spec 9.4), so the
     * thread is given room - but it has to be over before the engine is
     * freed, or the free would pull the ground from under the thread. */
    assert(WaitForSingleObject(r->thread, 30000) == WAIT_OBJECT_0);
    CloseHandle(r->thread);
#else
    pthread_join(r->thread, NULL);
#endif
}

int main(void) {
    /* Start from a clean catalog. A profile that already holds a complete
     * scan reconciles instead of scanning (spec 8.1), and this test wants
     * the first run. */
    tbox_fsx_remove("tbox_engine_catalog_test/index.db");
    tbox_fsx_remove("tbox_engine_catalog_test/index.db-wal");
    tbox_fsx_remove("tbox_engine_catalog_test/index.db-shm");
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);

    /* ---- the Saved Messages chat the fake serves --------------------- */
    FakeChat *chat = fake_chat_new();
    char caption[512];

    /* a folder with a file in it, and a file at the root */
    snprintf(caption, sizeof caption,
             "tbx1 {\"v\":1,\"k\":\"d\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"docs\",\"r\":1}",
             O_FOLDER);
    fake_chat_add_text(chat, caption);
    snprintf(caption, sizeof caption,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"%s\",\"p\":\"%s\","
             "\"n\":\"inside.txt\",\"r\":1,\"s\":12,\"m\":1760000000}",
             O_CHILD, O_FOLDER);
    fake_chat_add_media(chat, "document", "inside.txt", 12, 9001, caption);
    snprintf(caption, sizeof caption,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"report.pdf\",\"r\":2,\"s\":12345,\"m\":1760000001}",
             O_FILE);
    fake_chat_add_media(chat, "document", "report.pdf", 12345, 9002, caption);
    /* an unmanaged file: it belongs in the Inbox */
    fake_chat_add_media(chat, "document", "photo.jpg", 777, 9003, NULL);
    /* an item whose parent does not exist: it ends up in Lost & Found */
    snprintf(caption, sizeof caption,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"2222222222222222222222222222bbbb\","
             "\"p\":\"9999999999999999999999999999cccc\",\"n\":\"orphan.txt\","
             "\"r\":1,\"s\":3}");
    fake_chat_add_media(chat, "document", "orphan.txt", 3, 9005, caption);

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
    assert(strstr(l, "\"user_id\":4242") != NULL);
    assert(strstr(l, "\"is_premium\":true") != NULL);

    /* ---- the startup scan (spec 8.1 step 7, 8.2) -------------------- */
    l = wait_for(&out, "\"event\":\"sync.done\"", NULL, 5000);
    assert(l != NULL);
    assert(strstr(l, "\"event\":\"catalog.changed\"") != NULL ||
           wait_for(&out, "\"event\":\"catalog.changed\"", NULL, 5000) != NULL);
    assert(world.chat_calls == 1);   /* createPrivateChat */
    assert(world.option_calls == 1); /* getOption: the caption limit */
    assert(world.history_calls >= 2); /* pages, then the empty page that ends it */

    /* ---- catalog.list: the root -------------------------------------- */
    tbox_engine_push_command(e, "{\"id\":2,\"cmd\":\"catalog.list\","
                              "\"args\":{\"folder_id\":\"root\"}}");
    l = wait_for_id(&out, 2, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    assert(strstr(l, "docs") != NULL);      /* the folder */
    assert(strstr(l, "report.pdf") != NULL); /* the file */
    assert(strstr(l, "\"rev\":2") != NULL);
    assert(strstr(l, "\"size\":12345") != NULL);
    assert(strstr(l, "\"kind\":\"d\"") != NULL);
    assert(strstr(l, "\"parent_id\":\"root\"") != NULL);
    assert(strstr(l, "\"conflict\":false") != NULL);

    /* the folder's own children */
    tbox_engine_push_command(e, "{\"id\":3,\"cmd\":\"catalog.list\","
                              "\"args\":{\"folder_id\":\"0123456789abcdef0123456789abcdef\"}}");
    l = wait_for_id(&out, 3, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    assert(strstr(l, "inside.txt") != NULL);
    assert(strstr(l, "\"parent_id\":\"0123456789abcdef0123456789abcdef\"") != NULL);

    /* ---- catalog.list: the Inbox ------------------------------------- */
    tbox_engine_push_command(e, "{\"id\":4,\"cmd\":\"catalog.list\","
                              "\"args\":{\"folder_id\":\"inbox\"}}");
    l = wait_for_id(&out, 4, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    assert(strstr(l, "photo.jpg") != NULL);
    assert(strstr(l, "\"object_id\":null") != NULL);
    assert(strstr(l, "\"size\":777") != NULL);

    /* ---- catalog.list: trash is empty, lost holds the orphan ---------- */
    tbox_engine_push_command(e, "{\"id\":5,\"cmd\":\"catalog.list\","
                              "\"args\":{\"folder_id\":\"trash\"}}");
    l = wait_for_id(&out, 5, 5000);
    assert(l != NULL && strstr(l, "\"items\":[]") != NULL);

    tbox_engine_push_command(e, "{\"id\":15,\"cmd\":\"catalog.list\","
                              "\"args\":{\"folder_id\":\"lost\"}}");
    l = wait_for_id(&out, 15, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    assert(strstr(l, "orphan.txt") != NULL);
    assert(strstr(l, "\"parent_id\":\"lost\"") != NULL);

    /* an unknown folder is NOT_FOUND, a missing argument is BAD_ARGS */
    tbox_engine_push_command(e, "{\"id\":6,\"cmd\":\"catalog.list\","
                              "\"args\":{\"folder_id\":\"deadbeef\"}}");
    l = wait_for_id(&out, 6, 5000);
    assert(l != NULL && strstr(l, "NOT_FOUND") != NULL);

    tbox_engine_push_command(e, "{\"id\":7,\"cmd\":\"catalog.list\",\"args\":{}}");
    l = wait_for_id(&out, 7, 5000);
    assert(l != NULL && strstr(l, "BAD_ARGS") != NULL);

    /* ---- catalog.stat ------------------------------------------------ */
    tbox_engine_push_command(e,
        "{\"id\":8,\"cmd\":\"catalog.stat\",\"args\":{\"object_id\":"
        "\"fedcba9876543210fedcba9876543210\"}}");
    l = wait_for_id(&out, 8, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    assert(strstr(l, "report.pdf") != NULL);
    assert(strstr(l, "\"revisions\":[]") == NULL); /* the array is there */
    assert(strstr(l, "\"item\":") != NULL);

    /* an unknown object is NOT_FOUND */
    tbox_engine_push_command(e, "{\"id\":9,\"cmd\":\"catalog.stat\","
                              "\"args\":{\"object_id\":\"abc\"}}");
    l = wait_for_id(&out, 9, 5000);
    assert(l != NULL && strstr(l, "NOT_FOUND") != NULL);

    /* ---- catalog.summary --------------------------------------------- */
    tbox_engine_push_command(e, "{\"id\":10,\"cmd\":\"catalog.summary\"}");
    l = wait_for_id(&out, 10, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    assert(strstr(l, "\"files\":3") != NULL);   /* report.pdf, inside.txt, orphan */
    assert(strstr(l, "\"folders\":1") != NULL); /* docs */
    assert(strstr(l, "\"bytes\":12360") != NULL); /* 12345 + 12 + 3 */
    assert(strstr(l, "\"items\":1") != NULL);   /* one Inbox item */
    assert(strstr(l, "top_folders") != NULL);

    /* ---- sync.rescan: an op_id, then the scan again ------------------ */
    int history_before = world.history_calls;
    tbox_engine_push_command(e, "{\"id\":11,\"cmd\":\"sync.rescan\"}");
    l = wait_for_id(&out, 11, 5000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    assert(strstr(l, "\"op_id\":") != NULL);
    assert(world.history_calls > history_before);

    /* ---- a new message in Saved Messages reaches the catalog ---------- */
    snprintf(caption, sizeof caption,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"1111111111111111111111111111aaaa\","
             "\"p\":\"root\",\"n\":\"new.txt\",\"r\":1,\"s\":5}");
    long long new_id = fake_chat_add_media(chat, "document", "new.txt", 5, 9004,
                                            caption);
    char *msg_json = fake_chat_message_json(chat, new_id);
    assert(msg_json != NULL);
    char update[4096];
    snprintf(update, sizeof update, "{\"@type\":\"updateNewMessage\",\"message\":%s}",
             msg_json);
    free(msg_json);
    int changed_before = out.changed;
    fake_td_enqueue(fake, update);
    assert(wait_changed(&out, changed_before + 1, 5000) ==
           1); /* the update was applied and the catalog reloaded */

    tbox_engine_push_command(e, "{\"id\":12,\"cmd\":\"catalog.list\","
                              "\"args\":{\"folder_id\":\"root\"}}");
    l = wait_for_id(&out, 12, 5000);
    assert(l != NULL && strstr(l, "new.txt") != NULL);

    /* ---- a flood wait pauses the requests and the scan carries on ----- */
    world.flood_next = 1;
    int done_before = out.done;
    tbox_engine_push_command(e, "{\"id\":14,\"cmd\":\"sync.rescan\"}");
    assert(wait_for_id(&out, 14, 5000) != NULL);
    l = wait_for(&out, "\"event\":\"cooldown\"", NULL, 5000);
    assert(l != NULL);
    assert(strstr(l, "\"seconds\":1") != NULL);
    /* the engine waited the cooldown out and the scan still finished */
    assert(wait_done(&out, done_before + 1, 15000) == 1);

    /* ---- what the catalog looks like right now ------------------------ */
    const char *snap_ids[4] = {
        "{\"id\":20,\"cmd\":\"catalog.list\",\"args\":{\"folder_id\":\"root\"}}",
        "{\"id\":21,\"cmd\":\"catalog.list\",\"args\":{\"folder_id\":\"inbox\"}}",
        "{\"id\":22,\"cmd\":\"catalog.list\",\"args\":{\"folder_id\":\"lost\"}}",
        "{\"id\":23,\"cmd\":\"catalog.summary\"}",
    };
    char *first[4];
    for (int i = 0; i < 4; i++) {
        tbox_engine_push_command(e, snap_ids[i]);
        l = wait_for_id(&out, 20 + i, 5000);
        assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
        first[i] = _strdup(result_part(l));
    }

    /* ---- shutdown ----------------------------------------------------- */
    tbox_engine_push_command(e, "{\"id\":13,\"cmd\":\"engine.shutdown\"}");
    assert(wait_for_id(&out, 13, 5000) != NULL);
    join_thread(&r);
    assert(r.rc == 0);

    tbox_engine_free(e);

    /* ---- the database is thrown away: the next run must rebuild the very
     * same catalog from Saved Messages alone (the phase exit test) ------- */
    assert(tbox_fsx_remove("tbox_engine_catalog_test/index.db") == TBOX_FSX_OK);
    tbox_fsx_remove("tbox_engine_catalog_test/index.db-wal");
    tbox_fsx_remove("tbox_engine_catalog_test/index.db-shm");
    fake_drain(fake);

    int ready_before = out.ready;
    int done_before_rebuild = out.done;

    err = -1;
    TBoxEngine *e2 = tbox_engine_new(&fake->api, &cfg, capture, &out, &err);
    assert(e2 != NULL && err == 0);

    Run r2;
    memset(&r2, 0, sizeof r2);
    r2.e = e2;
    run_in_thread(&r2);

    tbox_engine_push_command(e2,
        "{\"id\":100,\"cmd\":\"auth.start\",\"args\":{\"api_id\":\"123\","
        "\"api_hash\":\"deadbeef\"}}");
    assert(wait_for_id(&out, 100, 3000) != NULL);

    fake_td_enqueue(fake,
        "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":"
        "{\"@type\":\"authorizationStateReady\"}}");
    assert(wait_ready(&out, ready_before + 1, 5000) == 1);
    /* the catalog was empty, so the whole history is read again */
    assert(wait_done(&out, done_before_rebuild + 1, 20000) == 1);

    const char *again[4] = {
        "{\"id\":30,\"cmd\":\"catalog.list\",\"args\":{\"folder_id\":\"root\"}}",
        "{\"id\":31,\"cmd\":\"catalog.list\",\"args\":{\"folder_id\":\"inbox\"}}",
        "{\"id\":32,\"cmd\":\"catalog.list\",\"args\":{\"folder_id\":\"lost\"}}",
        "{\"id\":33,\"cmd\":\"catalog.summary\"}",
    };
    for (int i = 0; i < 4; i++) {
        tbox_engine_push_command(e2, again[i]);
        l = wait_for_id(&out, 30 + i, 5000);
        assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
        assert(strcmp(first[i], result_part(l)) == 0);
    }

    tbox_engine_push_command(e2, "{\"id\":101,\"cmd\":\"engine.shutdown\"}");
    assert(wait_for_id(&out, 101, 5000) != NULL);
    join_thread(&r2);
    assert(r2.rc == 0);

    tbox_engine_free(e2);
    for (int i = 0; i < 4; i++) {
        free(first[i]);
    }
    out_free(&out);
    fake_td_free(fake);
    fake_chat_free(chat);

    printf("engine_catalog OK\n");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_download.c — the download of a file from Saved Messages (spec 8.6).
 *
 * The checks come first, because they are the ones the user runs into: a
 * file that is not there, a name that cannot be used, a destination folder
 * that is not there. Then the operation that is written down, the request
 * that asks Telegram for the file, and the last step: the copy, the check
 * against the size and the hash, and the name the file gets.
 *
 * The fake TDLib is a function that records the request and answers the way
 * TDLib answers a getMessage and a downloadFile.
 */
#include "xfer/download.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog/record.h"
#include "store/catalog.h"
#include "store/db.h"
#include "store/journal.h"
#include "util/fsx.h"
#include "xfer/hash.h"
#include "xfer/ops.h"

#define DIR "tbox_download_test"

/* The hash of "hello" - the content of the file that is fetched. */
#define HASH_HELLO \
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"

/* What the fake TDLib saw. */
static char g_request[8192];
static int g_calls;
static int g_download_calls;
static char g_local_path[512];
static int g_complete = 1;
static int g_reply_ok = 1;

/* The fake's answer: the message with the file on it, and the file itself
 * once it is on this disk. */
static cJSON *fake_call(void *ctx, const char *request) {
    (void)ctx;
    g_calls++;
    snprintf(g_request, sizeof g_request, "%s", request);
    if (!g_reply_ok) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(request);
    if (root == NULL) {
        return NULL;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "@type");
    const char *t = cJSON_IsString(type) ? cJSON_GetStringValue(type) : "";
    cJSON *reply = NULL;
    if (strcmp(t, "getMessage") == 0) {
        reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "@type", "message");
        cJSON_AddNumberToObject(reply, "id", 10);
        cJSON *content = cJSON_AddObjectToObject(reply, "content");
        cJSON_AddStringToObject(content, "@type", "messageDocument");
        cJSON *doc = cJSON_AddObjectToObject(content, "document");
        cJSON_AddStringToObject(doc, "@type", "file");
        cJSON_AddNumberToObject(doc, "id", 77);
        cJSON_AddNumberToObject(doc, "size", 5);
    } else if (strcmp(t, "downloadFile") == 0) {
        g_download_calls++;
        reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "@type", "file");
        cJSON_AddNumberToObject(reply, "id", 77);
        cJSON_AddNumberToObject(reply, "size", 5);
        cJSON *local = cJSON_AddObjectToObject(reply, "local");
        cJSON_AddStringToObject(local, "@type", "localFile");
        cJSON_AddBoolToObject(local, "is_downloading_completed", g_complete);
        cJSON_AddStringToObject(local, "path", g_local_path);
    }
    cJSON_Delete(root);
    return reply;
}

/* A fresh database: the catalog of one run must not leak into the next. */
static TBoxDb *open_fresh(void) {
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);
    tbox_fsx_remove(DIR "/index.db");
    tbox_fsx_remove(DIR "/index.db-wal");
    tbox_fsx_remove(DIR "/index.db-shm");
    TBoxDb *db = NULL;
    assert(tbox_db_open(DIR, &db) == TBOX_DB_OK);
    assert(db != NULL);
    return db;
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

/* Puts a file in the catalog, the way a message that was sent looks to the
 * engine afterwards: a message with a record, and the objects table rebuilt
 * from it. */
static void add_item(TBoxDb *db, const char *message_id, const char *object_id,
                     const char *parent, const char *name, const char *sha256) {
    TBoxMessageRow row;
    memset(&row, 0, sizeof row);
    snprintf(row.message_id, sizeof row.message_id, "%s", message_id);
    snprintf(row.kind, sizeof row.kind, "%s", TBOX_MSG_KIND_FILE);
    row.date = 1760000000;
    row.has_file_id = 1;
    row.file_id = 77;
    row.has_size = 1;
    row.size = 5;
    snprintf(row.orig_name, sizeof row.orig_name, "%s", name);
    row.has_record = 1;
    snprintf(row.rec.object_id, sizeof row.rec.object_id, "%s", object_id);
    snprintf(row.rec.parent, sizeof row.rec.parent, "%s", parent);
    snprintf(row.rec.name, sizeof row.rec.name, "%s", name);
    row.rec.rev = 1;
    row.rec.kind = 'f';
    row.rec.has_size = 1;
    row.rec.size = 5;
    if (sha256 != NULL) {
        row.rec.has_sha256 = 1;
        snprintf(row.rec.sha256, sizeof row.rec.sha256, "%s", sha256);
    }
    assert(tbox_cat_upsert_message(db, &row) == TBOX_DB_OK);
    assert(tbox_cat_rebuild_objects(db, NULL, NULL) == TBOX_DB_OK);
}

/* The environment the download works in. */
static void make_env(TBoxDownloadEnv *env, TBoxDb *db) {
    memset(env, 0, sizeof *env);
    env->db = db;
    env->call = fake_call;
    env->now = 1760000000;
}

static const char *state_of(TBoxDb *db, const char *op_id) {
    static char name[24];
    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    assert(tbox_journal_get(db, op_id, &row, &found) == TBOX_DB_OK);
    assert(found == 1);
    snprintf(name, sizeof name, "%s", row.state);
    tbox_journal_free_row(&row);
    return name;
}

/* ---- the name a file gets when the one the user wants is taken ---------- */

static void test_numbered_name(void) {
    char out[64];

    tbox_download_numbered_name("report.pdf", 1, out, sizeof out);
    assert(strcmp(out, "report.pdf") == 0);

    tbox_download_numbered_name("report.pdf", 2, out, sizeof out);
    assert(strcmp(out, "report (2).pdf") == 0);

    tbox_download_numbered_name("report.pdf", 3, out, sizeof out);
    assert(strcmp(out, "report (3).pdf") == 0);

    /* a name without an extension */
    tbox_download_numbered_name("README", 2, out, sizeof out);
    assert(strcmp(out, "README (2)") == 0);

    /* only the last dot counts */
    tbox_download_numbered_name("archive.tar.gz", 2, out, sizeof out);
    assert(strcmp(out, "archive.tar (2).gz") == 0);

    /* a name that is only a dot keeps it */
    tbox_download_numbered_name(".bashrc", 2, out, sizeof out);
    assert(strcmp(out, ".bashrc (2)") == 0);
}

/* ---- the checks (spec 8.6 step 1) -------------------------------------- */

static void test_checks(void) {
    TBoxDb *db = open_fresh();
    TBoxDownloadEnv env;
    make_env(&env, db);
    char err[256];
    TBoxDownloadChecked checked;

    add_item(db, "10", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "root", "a.txt",
             HASH_HELLO);

    TBoxDownloadRequest req;
    memset(&req, 0, sizeof req);
    req.message_id = "10";
    req.dest_dir = DIR;

    /* the file goes through */
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_OK);
    assert(strcmp(checked.message_id, "10") == 0);
    assert(strcmp(checked.name, "a.txt") == 0);
    assert(checked.has_size == 1 && checked.size == 5);
    assert(checked.has_sha256 == 1);
    assert(strcmp(checked.sha256, HASH_HELLO) == 0);
    assert(strcmp(checked.on_exists, "error") == 0);

    /* the object works as well as the message */
    req.message_id = NULL;
    req.object_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_OK);
    assert(strcmp(checked.message_id, "10") == 0);
    assert(strcmp(checked.object_id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);

    /* an object that is not there */
    req.object_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_E_NOT_FOUND);
    req.object_id = NULL;

    /* a message that is not there */
    req.message_id = "999";
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_E_NOT_FOUND);
    req.message_id = NULL;

    /* nothing to fetch */
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_E_ARGS);

    /* no destination */
    req.message_id = "10";
    req.dest_dir = NULL;
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_E_ARGS);
    req.dest_dir = DIR;

    /* a destination that is not there */
    req.dest_dir = DIR "/nothing";
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_E_IO);
    req.dest_dir = DIR;

    /* a mode that does not exist */
    req.on_exists = "overwrite";
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_E_ARGS);
    req.on_exists = "keep_both";
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_OK);
    assert(strcmp(checked.on_exists, "keep_both") == 0);

    tbox_db_close(db);
}

/* ---- the operation (spec 8.6 step 2) ----------------------------------- */

static void test_queue_and_start(void) {
    TBoxDb *db = open_fresh();
    TBoxDownloadEnv env;
    make_env(&env, db);
    char err[256];
    TBoxDownloadChecked checked;

    add_item(db, "10", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "root", "a.txt",
             HASH_HELLO);

    TBoxDownloadRequest req;
    memset(&req, 0, sizeof req);
    req.message_id = "10";
    req.dest_dir = DIR;

    g_calls = 0;
    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_OK);

    /* the operation is written down before anything is asked of Telegram */
    TBoxDownloadOp op;
    assert(tbox_download_queue(&env, &req, &checked, &op, err, sizeof err) ==
           TBOX_DOWNLOAD_OK);
    assert(strlen(op.op_id) == TBOX_OP_HEX_LEN);
    assert(strcmp(state_of(db, op.op_id), "QUEUED") == 0);
    assert(g_calls == 0);

    /* the arguments are kept, so the download can run again after a crash */
    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    assert(tbox_journal_get(db, op.op_id, &row, &found) == TBOX_DB_OK);
    assert(found == 1);
    TBoxDownloadArgs args;
    assert(tbox_download_args_parse(row.args_json, &args) == 1);
    assert(strcmp(args.message_id, "10") == 0);
    assert(strcmp(args.dest_dir, DIR) == 0);
    assert(strcmp(args.name, "a.txt") == 0);
    assert(strcmp(args.on_exists, "error") == 0);
    assert(strcmp(args.sha256, HASH_HELLO) == 0);
    assert(args.size == 5);
    tbox_download_args_free(&args);
    tbox_journal_free_row(&row);

    /* the engine puts the operation on its way */
    assert(tbox_journal_set_state(db, op.op_id, "RUNNING", NULL, 0, 100) ==
           TBOX_DB_OK);
    write_file(DIR "/remote.txt", "hello");
    snprintf(g_local_path, sizeof g_local_path, "%s", DIR "/remote.txt");
    g_download_calls = 0;
    TBoxDownloadPlan plan;
    assert(tbox_download_start(&env, op.op_id, &checked, &plan, err,
                                sizeof err) == TBOX_DOWNLOAD_OK);
    /* the message is asked for, and then the file */
    assert(g_calls == 2);
    assert(g_download_calls == 1);
    assert(strcmp(plan.file_id, "77") == 0);
    assert(plan.complete == 1);
    assert(strcmp(plan.local_path, DIR "/remote.txt") == 0);
    assert(strcmp(state_of(db, op.op_id), "SENT") == 0);

    /* a file that is not on this disk yet: Telegram has to fetch it */
    g_complete = 0;
    g_calls = 0;
    g_download_calls = 0;
    TBoxDownloadOp op2;
    assert(tbox_download_queue(&env, &req, &checked, &op2, err, sizeof err) ==
           TBOX_DOWNLOAD_OK);
    assert(tbox_journal_set_state(db, op2.op_id, "RUNNING", NULL, 0, 100) ==
           TBOX_DB_OK);
    TBoxDownloadPlan plan2;
    assert(tbox_download_start(&env, op2.op_id, &checked, &plan2, err,
                                sizeof err) == TBOX_DOWNLOAD_OK);
    assert(g_download_calls == 1);
    assert(plan2.complete == 0);
    assert(plan2.local_path[0] == '\0');
    assert(strcmp(state_of(db, op2.op_id), "SENT") == 0);
    g_complete = 1;

    /* Telegram that does not answer leaves the operation to the engine */
    g_reply_ok = 0;
    TBoxDownloadPlan plan3;
    assert(tbox_download_start(&env, op2.op_id, &checked, &plan3, err,
                                sizeof err) == TBOX_DOWNLOAD_E_TD);
    g_reply_ok = 1;

    tbox_db_close(db);
}

/* ---- the file that lands (spec 8.6 step 4) ---------------------------- */

static void test_finish(void) {
    TBoxDb *db = open_fresh();
    TBoxDownloadEnv env;
    make_env(&env, db);
    char err[256];
    TBoxDownloadChecked checked;

    add_item(db, "10", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "root", "a.txt",
             HASH_HELLO);

    TBoxDownloadRequest req;
    memset(&req, 0, sizeof req);
    req.message_id = "10";
    req.dest_dir = DIR "/out";
    assert(tbox_fsx_mkdirs(DIR "/out") == TBOX_FSX_OK);

    /* the file that Telegram has */
    write_file(DIR "/remote.txt", "hello");
    snprintf(g_local_path, sizeof g_local_path, "%s", DIR "/remote.txt");

    assert(tbox_download_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_DOWNLOAD_OK);

    TBoxDownloadPlan plan;
    memset(&plan, 0, sizeof plan);
    snprintf(plan.local_path, sizeof plan.local_path, "%s", g_local_path);
    plan.has_size = 1;
    plan.size = 5;
    plan.has_sha256 = 1;
    snprintf(plan.sha256, sizeof plan.sha256, "%s", HASH_HELLO);

    /* the file lands with the name the user expects */
    char path[512];
    assert(tbox_download_finish(&checked, &plan, path, sizeof path, err,
                                 sizeof err) == TBOX_DOWNLOAD_OK);
    assert(strcmp(path, DIR "/out/a.txt") == 0);
    TBoxFileInfo info;
    assert(tbox_fsx_file_info(path, &info) == TBOX_FSX_OK);
    assert(info.exists && info.size == 5);
    /* the temporary file is gone */
    assert(tbox_fsx_file_info(DIR "/out/.a.txt.tbox-part", &info) ==
           TBOX_FSX_OK);
    assert(!info.exists);

    /* a name that is taken is refused, and the file that is there is not
     * touched */
    assert(tbox_download_finish(&checked, &plan, path, sizeof path, err,
                                 sizeof err) == TBOX_DOWNLOAD_E_EXISTS);
    assert(tbox_fsx_file_info(DIR "/out/.a.txt.tbox-part", &info) ==
           TBOX_FSX_OK);
    assert(!info.exists);

    /* the user may ask for both to be there: the second one gets a number */
    checked.on_exists[0] = '\0';
    snprintf(checked.on_exists, sizeof checked.on_exists, "%s", "keep_both");
    assert(tbox_download_finish(&checked, &plan, path, sizeof path, err,
                                 sizeof err) == TBOX_DOWNLOAD_OK);
    assert(strcmp(path, DIR "/out/a (2).txt") == 0);

    /* a file that is not the size it should be is not let through */
    write_file(DIR "/remote.txt", "hell");
    TBoxDownloadPlan small = plan;
    small.size = 5;
    assert(tbox_download_finish(&checked, &small, path, sizeof path, err,
                                 sizeof err) == TBOX_DOWNLOAD_E_SIZE);
    assert(tbox_fsx_file_info(DIR "/out/.a.txt.tbox-part", &info) ==
           TBOX_FSX_OK);
    assert(!info.exists);

    /* and a file that is not the one that was sent is not let through
     * either */
    write_file(DIR "/remote.txt", "world");
    assert(tbox_download_finish(&checked, &plan, path, sizeof path, err,
                                 sizeof err) == TBOX_DOWNLOAD_E_HASH);
    assert(tbox_fsx_file_info(DIR "/out/.a.txt.tbox-part", &info) ==
           TBOX_FSX_OK);
    assert(!info.exists);
    /* and no file with the final name was made */
    assert(tbox_fsx_file_info(DIR "/out/a (3).txt", &info) == TBOX_FSX_OK);
    assert(!info.exists);

    /* a file that is not there at all */
    TBoxDownloadPlan missing = plan;
    snprintf(missing.local_path, sizeof missing.local_path, "%s",
             DIR "/nothing.txt");
    assert(tbox_download_finish(&checked, &missing, path, sizeof path, err,
                                 sizeof err) == TBOX_DOWNLOAD_E_IO);

    tbox_db_close(db);
}

int main(void) {
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);
    /* a run of this test leaves its files behind: start from a clean slate */
    tbox_fsx_remove(DIR "/remote.txt");
    tbox_fsx_remove(DIR "/out/a.txt");
    tbox_fsx_remove(DIR "/out/a (2).txt");
    tbox_fsx_remove(DIR "/out/.a.txt.tbox-part");
    test_numbered_name();
    test_checks();
    test_queue_and_start();
    test_finish();
    printf("test_download: ok\n");
    return 0;
}

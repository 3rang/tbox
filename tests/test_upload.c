/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_upload.c — the upload of a local file (spec 8.5).
 *
 * The checks come first, because they are the ones the user runs into: a
 * path that is not a file, a file that is too big, a folder that is not
 * there, a name that is taken. Then the hash, the record that goes with the
 * file, and the message that carries that record.
 *
 * The fake TDLib is a function that records the request and answers with a
 * message, the way TDLib answers a sendMessage.
 */
#include "xfer/upload.h"

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

#define DIR "tbox_upload_test"

/* What the fake TDLib saw. */
static char g_request[8192];
static int g_calls;
static int g_reply_ok = 1;

/* The fake's answer to sendMessage: a message with a temporary id and a
 * file that is being uploaded. */
static cJSON *fake_call(void *ctx, const char *request) {
    (void)ctx;
    g_calls++;
    snprintf(g_request, sizeof g_request, "%s", request);
    if (!g_reply_ok) {
        return NULL;
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "@type", "message");
    cJSON_AddNumberToObject(reply, "id", 4242);
    cJSON *content = cJSON_AddObjectToObject(reply, "content");
    cJSON_AddStringToObject(content, "@type", "messageDocument");
    cJSON *doc = cJSON_AddObjectToObject(content, "document");
    cJSON_AddStringToObject(doc, "@type", "file");
    cJSON_AddNumberToObject(doc, "id", 77);
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

/* Puts an item in the catalog, the way a message that was sent looks to
 * the engine afterwards: a message with a record, and the objects table
 * rebuilt from it. */
static void add_item(TBoxDb *db, const char *message_id, const char *object_id,
                     const char *parent, const char *name) {
    TBoxMessageRow row;
    memset(&row, 0, sizeof row);
    snprintf(row.message_id, sizeof row.message_id, "%s", message_id);
    snprintf(row.kind, sizeof row.kind, "%s", TBOX_MSG_KIND_FILE);
    row.date = 1760000000;
    row.has_record = 1;
    snprintf(row.rec.object_id, sizeof row.rec.object_id, "%s", object_id);
    snprintf(row.rec.parent, sizeof row.rec.parent, "%s", parent);
    snprintf(row.rec.name, sizeof row.rec.name, "%s", name);
    row.rec.rev = 1;
    row.rec.kind = 'f';
    assert(tbox_cat_upsert_message(db, &row) == TBOX_DB_OK);
    assert(tbox_cat_rebuild_objects(db, NULL, NULL) == TBOX_DB_OK);
}

/* The environment the upload works in. */
static void make_env(TBoxUploadEnv *env, TBoxDb *db) {
    memset(env, 0, sizeof *env);
    env->db = db;
    env->call = fake_call;
    env->chat_id = 12345;
    env->caption_limit = 1024;
    env->is_premium = 0;
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

/* ---- the hash ---------------------------------------------------------- */

static void test_hash(void) {
    char hex[TBOX_SHA256_HEX_LEN + 1];
    long long size = 0;

    /* the vector from FIPS 180-2 */
    tbox_fsx_remove(DIR "/abc.txt");
    assert(tbox_hash_file(DIR "/abc.txt", hex, &size) != 0); /* not there */
    write_file(DIR "/abc.txt", "abc");
    assert(tbox_hash_file(DIR "/abc.txt", hex, &size) == 0);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    assert(size == 3);

    /* the empty file */
    write_file(DIR "/empty.txt", "");
    assert(tbox_hash_file(DIR "/empty.txt", hex, &size) == 0);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    assert(size == 0);

    /* a file that is not there has no hash */
    assert(tbox_hash_file(DIR "/nothing.txt", hex, &size) != 0);
    assert(hex[0] == '\0');
}

/* ---- the name of a file ------------------------------------------------ */

static void test_name_from_path(void) {
    char name[64];

    tbox_upload_name_from_path("C:\\Users\\me\\Documents\\report.txt", name,
                               sizeof name);
    assert(strcmp(name, "report.txt") == 0);

    tbox_upload_name_from_path("/home/me/report.txt", name, sizeof name);
    assert(strcmp(name, "report.txt") == 0);

    tbox_upload_name_from_path("report.txt", name, sizeof name);
    assert(strcmp(name, "report.txt") == 0);

    /* a trailing separator does not hide the name */
    tbox_upload_name_from_path("dir\\", name, sizeof name);
    assert(strcmp(name, "dir") == 0);

    tbox_upload_name_from_path(NULL, name, sizeof name);
    assert(name[0] == '\0');
}

/* ---- the checks (spec 8.5 steps 1 and 2) ------------------------------- */

static void test_checks(void) {
    TBoxDb *db = open_fresh();
    TBoxUploadEnv env;
    make_env(&env, db);
    char err[256];
    TBoxUploadChecked checked;

    write_file(DIR "/a.txt", "hello");
    assert(tbox_fsx_mkdirs(DIR "/sub") == TBOX_FSX_OK);

    TBoxUploadRequest req;
    memset(&req, 0, sizeof req);
    req.local_path = DIR "/a.txt";
    req.folder_id = "root";

    /* a file that is there goes through */
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_OK);
    assert(strcmp(checked.parent, "root") == 0);
    assert(strcmp(checked.name, "a.txt") == 0);
    assert(checked.size == 5);

    /* a path that is not there */
    req.local_path = DIR "/nothing.txt";
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_E_FILE);
    assert(err[0] != '\0');

    /* a folder is not a file */
    req.local_path = DIR "/sub";
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_E_FILE);

    /* no path at all */
    req.local_path = NULL;
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_E_ARGS);

    /* a file that is too big */
    req.local_path = DIR "/a.txt";
    env.max_bytes = 3;
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_E_TOO_LARGE);
    env.max_bytes = 0;

    /* a folder that is not there */
    req.folder_id = "0123456789abcdef0123456789abcdef";
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_E_FOLDER);
    req.folder_id = "root";

    /* the name is free, and the name of a file that is there is not */
    add_item(db, "10", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "root", "a.txt");
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_E_EXISTS);

    /* the compare does not care about the case (spec 7.4) */
    req.name = "A.TXT";
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_E_EXISTS);

    /* a name that is too long is refused, not cut short */
    char longname[TBOX_NAME_MAX_BYTES + 2];
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    req.name = longname;
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_E_NAME);

    tbox_db_close(db);
}

/* ---- the upload (spec 8.5 steps 3 to 5) ------------------------------- */

static void test_upload(void) {
    TBoxDb *db = open_fresh();
    TBoxUploadEnv env;
    make_env(&env, db);
    char err[256];
    TBoxUploadChecked checked;

    write_file(DIR "/a.txt", "hello");

    TBoxUploadRequest req;
    memset(&req, 0, sizeof req);
    req.local_path = DIR "/a.txt";
    req.folder_id = "root";

    g_calls = 0;
    assert(tbox_upload_check(&env, &req, &checked, err, sizeof err) ==
           TBOX_UPLOAD_OK);

    /* the operation is written down before anything is sent */
    TBoxUploadOp op;
    assert(tbox_upload_queue(&env, &req, &checked, &op, err, sizeof err) ==
           TBOX_UPLOAD_OK);
    assert(strlen(op.op_id) == TBOX_OP_HEX_LEN);
    assert(strlen(op.object_id) == TBOX_ID_HEX_LEN);
    assert(strcmp(state_of(db, op.op_id), "QUEUED") == 0);
    assert(g_calls == 0); /* nothing was sent yet */

    /* the arguments are kept, so the upload can run again after a crash */
    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    assert(tbox_journal_get(db, op.op_id, &row, &found) == TBOX_DB_OK);
    assert(found == 1);
    TBoxUploadArgs args;
    assert(tbox_upload_args_parse(row.args_json, &args) == 1);
    assert(strcmp(args.local_path, DIR "/a.txt") == 0);
    assert(strcmp(args.folder_id, "root") == 0);
    assert(strcmp(args.name, "a.txt") == 0);
    assert(strcmp(args.object_id, op.object_id) == 0);
    tbox_upload_args_free(&args);
    tbox_journal_free_row(&row);

    /* the engine puts the operation on its way and then sends it */
    assert(tbox_journal_set_state(db, op.op_id, "RUNNING", NULL, 0, 100) ==
           TBOX_DB_OK);
    TBoxUploadPlan plan;
    assert(tbox_upload_send(&env, op.op_id, op.object_id, &checked,
                             req.local_path, &plan, err, sizeof err) ==
           TBOX_UPLOAD_OK);
    assert(g_calls == 1);
    assert(strcmp(state_of(db, op.op_id), "SENT") == 0);
    assert(strcmp(plan.message_id, "4242") == 0);
    assert(strcmp(plan.file_id, "77") == 0);

    /* the request carries the file and the record */
    cJSON *sent = cJSON_Parse(g_request);
    assert(sent != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(sent, "@type")->valuestring,
                  "sendMessage") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(sent, "chat_id")->valuedouble ==
           12345.0);
    cJSON *content =
        cJSON_GetObjectItemCaseSensitive(sent, "input_message_content");
    cJSON *doc = cJSON_GetObjectItemCaseSensitive(content, "document");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(doc, "@type")->valuestring,
                  "inputFileLocal") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(doc, "path")->valuestring,
                  DIR "/a.txt") == 0);
    cJSON *caption = cJSON_GetObjectItemCaseSensitive(content, "caption");
    const char *text = cJSON_GetObjectItemCaseSensitive(caption, "text")->valuestring;

    /* the caption is a record that says what the file is */
    TBoxRecord rec;
    assert(tbox_record_parse(text, &rec) == TBOX_REC_OK);
    assert(rec.kind == 'f');
    assert(strcmp(rec.object_id, op.object_id) == 0);
    assert(strcmp(rec.parent, "root") == 0);
    assert(strcmp(rec.name, "a.txt") == 0);
    assert(rec.rev == 1);
    assert(rec.has_size == 1);
    assert(rec.size == 5);
    assert(rec.has_sha256 == 1);
    assert(strcmp(rec.sha256,
                  "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") == 0);
    assert(rec.has_op == 1);
    assert(strcmp(rec.op_id, op.op_id) == 0);
    cJSON_Delete(sent);

    /* a message that is not accepted leaves the operation to the engine */
    TBoxUploadOp op2;
    assert(tbox_upload_queue(&env, &req, &checked, &op2, err, sizeof err) ==
           TBOX_UPLOAD_OK);
    assert(tbox_journal_set_state(db, op2.op_id, "RUNNING", NULL, 0, 100) ==
           TBOX_DB_OK);
    g_reply_ok = 0;
    TBoxUploadPlan plan2;
    assert(tbox_upload_send(&env, op2.op_id, op2.object_id, &checked,
                             req.local_path, &plan2, err, sizeof err) ==
           TBOX_UPLOAD_E_TD);
    /* the operation is still on its way: the engine decides to wait or to
     * give up (spec 8.4) */
    assert(strcmp(state_of(db, op2.op_id), "RUNNING") == 0);
    g_reply_ok = 1;

    tbox_db_close(db);
}

/* ---- the limits -------------------------------------------------------- */

static void test_limits(void) {
    assert(tbox_upload_size_limit(0) == 2000LL * 1024LL * 1024LL);
    assert(tbox_upload_size_limit(1) == 4000LL * 1024LL * 1024LL);
}

int main(void) {
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);
    /* a run of this test leaves its files behind: start from a clean slate */
    tbox_fsx_remove(DIR "/abc.txt");
    tbox_fsx_remove(DIR "/empty.txt");
    tbox_fsx_remove(DIR "/a.txt");
    tbox_fsx_remove(DIR "/nothing.txt");
    test_hash();
    test_name_from_path();
    test_checks();
    test_upload();
    test_limits();
    printf("test_upload: ok\n");
    return 0;
}

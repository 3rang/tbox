/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * upload.c — the upload of a local file into Saved Messages (spec 8.5).
 *
 * The order of the steps is the order of the spec: the file is checked, the
 * folder and the name are resolved, the operation is written to the journal,
 * the content is hashed, the record is built and only then is the message
 * handed to Telegram. Nothing is sent before every check has passed.
 *
 * The operation is written down before the message goes out, so an engine
 * that dies in between leaves a trail: the next run looks for the message
 * (the record carries the operation id) and does not send the file twice
 * (spec 8.4).
 */
#include "xfer/upload.h"

#include <stdlib.h>
#include <string.h>

#include "store/journal.h"
#include "store/query.h"
#include "util/fsx.h"
#include "util/rand.h"
#include "xfer/hash.h"
#include "xfer/ops.h"

#ifdef _WIN32
#define TBOX_STRDUP _strdup
#else
#define TBOX_STRDUP strdup
#endif

/* ---- small helpers --------------------------------------------------- */

static void set_err(char *err, size_t cap, const char *text) {
    if (err == NULL || cap == 0) {
        return;
    }
    snprintf(err, cap, "%s", text != NULL ? text : "");
}

const char *tbox_upload_status_text(TBoxUploadStatus status) {
    switch (status) {
    case TBOX_UPLOAD_OK:
        return "ok";
    case TBOX_UPLOAD_E_ARGS:
        return "the request is incomplete";
    case TBOX_UPLOAD_E_FILE:
        return "there is no file at that path";
    case TBOX_UPLOAD_E_TOO_LARGE:
        return "the file is too big to send";
    case TBOX_UPLOAD_E_FOLDER:
        return "that folder cannot be used";
    case TBOX_UPLOAD_E_NAME:
        return "that name cannot be used";
    case TBOX_UPLOAD_E_EXISTS:
        return "that name is taken in that folder";
    case TBOX_UPLOAD_E_IO:
        return "the file could not be read";
    case TBOX_UPLOAD_E_CAPTION:
        return "the record does not fit in a caption";
    case TBOX_UPLOAD_E_DB:
        return "the operation could not be written down";
    case TBOX_UPLOAD_E_TD:
        return "Telegram refused the message";
    case TBOX_UPLOAD_E_INTERNAL:
        return "something went wrong";
    default:
        return "unknown";
    }
}

long long tbox_upload_size_limit(int is_premium) {
    return is_premium ? TBOX_UPLOAD_MAX_BYTES_PREMIUM : TBOX_UPLOAD_MAX_BYTES;
}

/* The limit that applies: the one the caller sets, or the account's. */
static long long upload_limit(const TBoxUploadEnv *env) {
    if (env->max_bytes > 0) {
        return env->max_bytes;
    }
    return tbox_upload_size_limit(env->is_premium);
}

void tbox_upload_name_from_path(const char *path, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (path == NULL) {
        return;
    }
    /* The last part of the path. Both separators are understood, because a
     * path may come from either world. */
    size_t end = strlen(path);
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
        end--;
    }
    size_t start = end;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') {
        start--;
    }
    size_t n = end - start;
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, path + start, n);
    out[n] = '\0';
}

/* ---- the checks (spec 8.5 steps 1 and 2) ------------------------------- */

/* The folder the file goes into: "root", or the id of a folder that is
 * there and is live. A conflict id is reduced to the object it belongs to. */
static TBoxUploadStatus resolve_folder(TBoxDb *db, const char *folder_id,
                                        char *parent, size_t parent_cap) {
    if (strcmp(folder_id, TBOX_FOLDER_ROOT) == 0) {
        snprintf(parent, parent_cap, "%s", TBOX_FOLDER_ROOT);
        return TBOX_UPLOAD_OK;
    }
    TBoxListItem item;
    memset(&item, 0, sizeof item);
    int found = 0;
    if (tbox_query_get(db, folder_id, &item, &found) != TBOX_DB_OK) {
        return TBOX_UPLOAD_E_DB;
    }
    if (!found || item.kind != 'd' || item.has_trashed) {
        return TBOX_UPLOAD_E_FOLDER;
    }
    snprintf(parent, parent_cap, "%.*s", TBOX_ID_HEX_LEN, item.object_id);
    return TBOX_UPLOAD_OK;
}

/* The name the file goes by: the one the caller gave, or the file's own.
 * A name that is too long is refused (the record holds 1-255 bytes, spec
 * 7.2); the rest is made safe (S8). */
static TBoxUploadStatus resolve_name(const TBoxUploadRequest *req,
                                      char *name, size_t name_cap,
                                      int *too_long) {
    *too_long = 0;
    char raw[TBOX_NAME_MAX_BYTES + 1];
    if (req->name != NULL && req->name[0] != '\0') {
        if (strlen(req->name) > TBOX_NAME_MAX_BYTES) {
            *too_long = 1;
            return TBOX_UPLOAD_E_NAME;
        }
        snprintf(raw, sizeof raw, "%s", req->name);
    } else {
        tbox_upload_name_from_path(req->local_path, raw, sizeof raw);
    }
    tbox_name_sanitize(raw, name, name_cap);
    if (name[0] == '\0') {
        return TBOX_UPLOAD_E_NAME;
    }
    return TBOX_UPLOAD_OK;
}

TBoxUploadStatus tbox_upload_check(const TBoxUploadEnv *env,
                                    const TBoxUploadRequest *req,
                                    TBoxUploadChecked *out, char *err,
                                    size_t err_cap) {
    if (env == NULL || req == NULL || out == NULL || env->db == NULL) {
        set_err(err, err_cap, "the upload has no catalog");
        return TBOX_UPLOAD_E_ARGS;
    }
    memset(out, 0, sizeof *out);
    if (req->local_path == NULL || req->local_path[0] == '\0') {
        set_err(err, err_cap, "a local path is required");
        return TBOX_UPLOAD_E_ARGS;
    }
    if (req->folder_id == NULL || req->folder_id[0] == '\0') {
        set_err(err, err_cap, "a folder is required");
        return TBOX_UPLOAD_E_ARGS;
    }

    /* 1. the file is there */
    TBoxFileInfo info;
    if (tbox_fsx_file_info(req->local_path, &info) != TBOX_FSX_OK) {
        set_err(err, err_cap, "the file could not be looked at");
        return TBOX_UPLOAD_E_IO;
    }
    if (!info.exists || info.is_dir) {
        set_err(err, err_cap, "there is no file at that path");
        return TBOX_UPLOAD_E_FILE;
    }

    /* 2. it is not too big */
    if (info.size > upload_limit(env)) {
        set_err(err, err_cap, "the file is too big to send");
        return TBOX_UPLOAD_E_TOO_LARGE;
    }

    /* 3. the folder is there and is live */
    TBoxUploadStatus st =
        resolve_folder(env->db, req->folder_id, out->parent, sizeof out->parent);
    if (st != TBOX_UPLOAD_OK) {
        set_err(err, err_cap, tbox_upload_status_text(st));
        return st;
    }

    /* 4. the name is usable and free */
    int too_long = 0;
    st = resolve_name(req, out->name, sizeof out->name, &too_long);
    if (st != TBOX_UPLOAD_OK) {
        set_err(err, err_cap,
                too_long ? "that name is too long" : "that name cannot be used");
        return st;
    }
    TBoxListItem same;
    int found = 0;
    if (tbox_query_find_by_name(env->db, out->parent, out->name, &same, &found) !=
        TBOX_DB_OK) {
        set_err(err, err_cap, "the catalog is unreadable");
        return TBOX_UPLOAD_E_DB;
    }
    if (found) {
        set_err(err, err_cap,
                "there is already something with that name in that folder");
        return TBOX_UPLOAD_E_EXISTS;
    }

    out->size = info.size;
    out->mtime = info.mtime;
    return TBOX_UPLOAD_OK;
}

/* ---- the operation (spec 8.5 step 4) ---------------------------------- */

/* The arguments of an upload, as the journal keeps them: enough to run it
 * again after a crash. */
char *tbox_upload_args_json(const TBoxUploadArgs *args) {
    if (args == NULL) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "local_path",
                            args->local_path != NULL ? args->local_path : "");
    cJSON_AddStringToObject(o, "folder_id",
                            args->folder_id != NULL ? args->folder_id : "root");
    if (args->name != NULL && args->name[0] != '\0') {
        cJSON_AddStringToObject(o, "name", args->name);
    }
    if (args->object_id != NULL && args->object_id[0] != '\0') {
        cJSON_AddStringToObject(o, "object_id", args->object_id);
    }
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

void tbox_upload_args_free(TBoxUploadArgs *args) {
    if (args == NULL) {
        return;
    }
    free(args->local_path);
    free(args->folder_id);
    free(args->name);
    free(args->object_id);
    memset(args, 0, sizeof *args);
}

int tbox_upload_args_parse(const char *args_json, TBoxUploadArgs *out) {
    if (args_json == NULL || out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(args_json);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return 0;
    }
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "local_path");
    if (!cJSON_IsString(path) || cJSON_GetStringValue(path) == NULL ||
        cJSON_GetStringValue(path)[0] == '\0') {
        cJSON_Delete(root);
        return 0;
    }
    out->local_path = TBOX_STRDUP(cJSON_GetStringValue(path));
    const cJSON *folder = cJSON_GetObjectItemCaseSensitive(root, "folder_id");
    out->folder_id = TBOX_STRDUP(cJSON_IsString(folder)
                                     ? cJSON_GetStringValue(folder)
                                     : TBOX_FOLDER_ROOT);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name)) {
        out->name = TBOX_STRDUP(cJSON_GetStringValue(name));
    }
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "object_id");
    if (cJSON_IsString(object)) {
        out->object_id = TBOX_STRDUP(cJSON_GetStringValue(object));
    }
    cJSON_Delete(root);
    if (out->local_path == NULL || out->folder_id == NULL) {
        tbox_upload_args_free(out);
        return 0;
    }
    return 1;
}

TBoxUploadStatus tbox_upload_queue(const TBoxUploadEnv *env,
                                    const TBoxUploadRequest *req,
                                    const TBoxUploadChecked *checked,
                                    TBoxUploadOp *out, char *err,
                                    size_t err_cap) {
    if (env == NULL || req == NULL || checked == NULL || out == NULL ||
        env->db == NULL) {
        set_err(err, err_cap, "the upload has no catalog");
        return TBOX_UPLOAD_E_ARGS;
    }
    memset(out, 0, sizeof *out);

    if (tbox_rand_hex(out->op_id, sizeof out->op_id, TBOX_OP_HEX_LEN / 2) !=
            TBOX_RAND_OK ||
        tbox_rand_hex(out->object_id, sizeof out->object_id,
                      TBOX_ID_HEX_LEN / 2) != TBOX_RAND_OK) {
        set_err(err, err_cap, "no ids could be made");
        return TBOX_UPLOAD_E_INTERNAL;
    }

    TBoxUploadArgs args;
    memset(&args, 0, sizeof args);
    args.local_path = TBOX_STRDUP(req->local_path);
    args.folder_id = TBOX_STRDUP(checked->parent);
    args.name = TBOX_STRDUP(checked->name);
    args.object_id = TBOX_STRDUP(out->object_id);
    char *args_json = tbox_upload_args_json(&args);
    tbox_upload_args_free(&args);
    if (args_json == NULL) {
        set_err(err, err_cap, "the arguments could not be written");
        return TBOX_UPLOAD_E_INTERNAL;
    }

    TBoxDbStatus rc = tbox_journal_insert(env->db, out->op_id,
                                          TBOX_OP_KIND_UPLOAD, args_json,
                                          env->now);
    free(args_json);
    if (rc != TBOX_DB_OK) {
        set_err(err, err_cap, "the operation could not be written down");
        return TBOX_UPLOAD_E_DB;
    }
    return TBOX_UPLOAD_OK;
}

/* ---- the message (spec 8.5 steps 3 and 5) ----------------------------- */

/* The request that sends the file with the record as its caption. The path
 * and the record are put in as json values, never spliced into text, so a
 * name with a quote or a backslash in it is still sent correctly. */
static char *build_send_request(const TBoxUploadEnv *env, const char *caption,
                                 const char *local_path) {
    cJSON *req = cJSON_CreateObject();
    if (req == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(req, "@type", "sendMessage");
    cJSON_AddNumberToObject(req, "chat_id", (double)env->chat_id);
    cJSON *content = cJSON_AddObjectToObject(req, "input_message_content");
    cJSON_AddStringToObject(content, "@type", "inputMessageDocument");
    cJSON *doc = cJSON_AddObjectToObject(content, "document");
    cJSON_AddStringToObject(doc, "@type", "inputFileLocal");
    cJSON_AddStringToObject(doc, "path", local_path);
    cJSON *cap = cJSON_AddObjectToObject(content, "caption");
    cJSON_AddStringToObject(cap, "@type", "formattedText");
    cJSON_AddStringToObject(cap, "text", caption);
    /* The file keeps its own name: tbox is a file drive, not a photo app. */
    cJSON_AddBoolToObject(content, "disable_content_type_detection", 1);
    char *out = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return out;
}

/* The temporary message id and the id of the file being uploaded, out of the
 * reply to sendMessage. */
static void read_send_reply(const cJSON *reply, TBoxUploadPlan *plan) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(reply, "id");
    if (cJSON_IsNumber(id)) {
        snprintf(plan->message_id, sizeof plan->message_id, "%lld",
                 (long long)id->valuedouble);
    }
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(reply, "content");
    const cJSON *doc = cJSON_GetObjectItemCaseSensitive(content, "document");
    const cJSON *fid = cJSON_GetObjectItemCaseSensitive(doc, "id");
    if (cJSON_IsNumber(fid)) {
        snprintf(plan->file_id, sizeof plan->file_id, "%lld",
                 (long long)fid->valuedouble);
    }
}

TBoxUploadStatus tbox_upload_send(const TBoxUploadEnv *env, const char *op_id,
                                   const char *object_id,
                                   const TBoxUploadChecked *checked,
                                   const char *local_path,
                                   TBoxUploadPlan *plan, char *err,
                                   size_t err_cap) {
    if (env == NULL || op_id == NULL || op_id[0] == '\0' || checked == NULL ||
        local_path == NULL || local_path[0] == '\0' || plan == NULL ||
        env->db == NULL || env->call == NULL) {
        set_err(err, err_cap, "the upload has no catalog or no connection");
        return TBOX_UPLOAD_E_ARGS;
    }
    memset(plan, 0, sizeof *plan);
    snprintf(plan->op_id, sizeof plan->op_id, "%s", op_id);
    snprintf(plan->object_id, sizeof plan->object_id, "%s",
             object_id != NULL ? object_id : "");
    snprintf(plan->parent, sizeof plan->parent, "%s", checked->parent);
    snprintf(plan->name, sizeof plan->name, "%s", checked->name);

    /* the file as it is now: the record describes what goes out */
    TBoxFileInfo info;
    if (tbox_fsx_file_info(local_path, &info) != TBOX_FSX_OK || !info.exists ||
        info.is_dir) {
        set_err(err, err_cap, "there is no file at that path");
        return TBOX_UPLOAD_E_FILE;
    }
    if (info.size > upload_limit(env)) {
        set_err(err, err_cap, "the file is too big to send");
        return TBOX_UPLOAD_E_TOO_LARGE;
    }

    /* the hash, so the catalog can tell later whether the file that comes
     * back is the file that went out */
    char sha256[TBOX_SHA256_HEX_LEN + 1];
    if (tbox_hash_file(local_path, sha256, NULL) != 0) {
        set_err(err, err_cap, "the file could not be read");
        return TBOX_UPLOAD_E_IO;
    }
    snprintf(plan->sha256, sizeof plan->sha256, "%s", sha256);
    plan->size = info.size;
    plan->mtime = info.mtime;

    /* the record that goes with the file */
    TBoxRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.version = TBOX_RECORD_VERSION;
    rec.kind = 'f';
    snprintf(rec.object_id, sizeof rec.object_id, "%s", plan->object_id);
    snprintf(rec.parent, sizeof rec.parent, "%s", plan->parent);
    snprintf(rec.name, sizeof rec.name, "%s", plan->name);
    rec.rev = 1;
    rec.has_size = 1;
    rec.size = info.size;
    rec.has_sha256 = 1;
    snprintf(rec.sha256, sizeof rec.sha256, "%s", sha256);
    rec.has_mtime = 1;
    rec.mtime = info.mtime;
    rec.has_op = 1;
    snprintf(rec.op_id, sizeof rec.op_id, "%s", op_id);

    char caption[TBOX_UPLOAD_CAPTION_MAX];
    TBoxRecordStatus rs = tbox_record_build(&rec, caption, sizeof caption,
                                            env->caption_limit);
    if (rs == TBOX_REC_E_TOO_LONG) {
        set_err(err, err_cap, "that name is too long for a caption");
        return TBOX_UPLOAD_E_CAPTION;
    }
    if (rs != TBOX_REC_OK) {
        set_err(err, err_cap, "the record could not be built");
        return TBOX_UPLOAD_E_CAPTION;
    }
    snprintf(plan->caption, sizeof plan->caption, "%s", caption);

    /* the message goes out */
    char *request = build_send_request(env, caption, local_path);
    if (request == NULL) {
        set_err(err, err_cap, "the request could not be built");
        return TBOX_UPLOAD_E_INTERNAL;
    }
    cJSON *reply = env->call(env->call_ctx, request);
    free(request);
    if (reply == NULL) {
        /* Telegram did not take the message. The engine decides what
         * happens next: wait and try again, or give up (spec 8.4). */
        set_err(err, err_cap, "Telegram did not take the message");
        return TBOX_UPLOAD_E_TD;
    }
    read_send_reply(reply, plan);
    cJSON_Delete(reply);
    if (plan->message_id[0] == '\0') {
        set_err(err, err_cap, "Telegram did not answer with a message");
        return TBOX_UPLOAD_E_TD;
    }

    /* the operation waits for the message to arrive */
    (void)tbox_journal_set_temp_msg(env->db, op_id, plan->message_id, env->now);
    if (tbox_journal_set_state(env->db, op_id, "SENT", NULL, 0, env->now) !=
        TBOX_DB_OK) {
        set_err(err, err_cap, "the operation could not be written down");
        return TBOX_UPLOAD_E_DB;
    }
    plan->rev = 1;
    return TBOX_UPLOAD_OK;
}

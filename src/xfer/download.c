/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * download.c — the download of a file from Saved Messages (spec 8.6).
 *
 * The order of the steps is the order of the spec: the message is resolved
 * and the name is made safe, the file is asked for, and when it is there it
 * is copied under a temporary name, checked against the size and the hash
 * the catalog remembers, and only then given its real name.
 *
 * The temporary name is what makes a failed download harmless: a file that
 * is half there never gets the name the user expects, and the copy is
 * removed when anything goes wrong.
 */
#include "xfer/download.h"

#include <stdlib.h>
#include <string.h>

#include "catalog/record.h"
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

const char *tbox_download_status_text(TBoxDownloadStatus status) {
    switch (status) {
    case TBOX_DOWNLOAD_OK:
        return "ok";
    case TBOX_DOWNLOAD_E_ARGS:
        return "the request is incomplete";
    case TBOX_DOWNLOAD_E_NOT_FOUND:
        return "there is no such file";
    case TBOX_DOWNLOAD_E_NAME:
        return "that name cannot be used";
    case TBOX_DOWNLOAD_E_EXISTS:
        return "there is already a file with that name";
    case TBOX_DOWNLOAD_E_IO:
        return "the file could not be read or written";
    case TBOX_DOWNLOAD_E_HASH:
        return "the file is not the one that was sent";
    case TBOX_DOWNLOAD_E_SIZE:
        return "the file is not the size it should be";
    case TBOX_DOWNLOAD_E_DB:
        return "the operation could not be written down";
    case TBOX_DOWNLOAD_E_TD:
        return "Telegram refused";
    case TBOX_DOWNLOAD_E_INTERNAL:
        return "something went wrong";
    default:
        return "unknown";
    }
}

/* The name a file gets when the one the user wants is taken: "report.pdf"
 * becomes "report (2).pdf". The extension is whatever follows the last dot
 * (a name that only is a dot keeps it). */
void tbox_download_numbered_name(const char *name, int number, char *out,
                                 size_t out_cap) {
    if (out == NULL || out_cap == 0) {
        return;
    }
    out[0] = '\0';
    if (name == NULL || name[0] == '\0') {
        return;
    }
    if (number <= 1) {
        snprintf(out, out_cap, "%s", name);
        return;
    }
    const char *dot = strrchr(name, '.');
    size_t stem = dot != NULL ? (size_t)(dot - name) : strlen(name);
    if (dot != NULL && stem == 0) {
        /* a name that starts with a dot has no extension: the dot belongs to
         * the name (".bashrc" becomes ".bashrc (2)") */
        stem = strlen(name);
        dot = NULL;
    }
    if (stem >= out_cap) {
        stem = out_cap - 1;
    }
    memcpy(out, name, stem);
    out[stem] = '\0';
    snprintf(out + stem, out_cap - stem, " (%d)%s", number,
             dot != NULL ? dot : "");
}

/* The path of a file in a folder. */
static int join_path(char *dst, size_t cap, const char *dir, const char *name) {
    int n = snprintf(dst, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* ---- the checks (spec 8.6 step 1) ------------------------------------- */

/* The message the request names: by message id, or the current revision of
 * an object. */
static TBoxDownloadStatus resolve_message(TBoxDb *db,
                                           const TBoxDownloadRequest *req,
                                           TBoxMessageInfo *msg, int *found) {
    char message_id[TBOX_MSGID_MAX_BYTES + 1];
    message_id[0] = '\0';
    if (req->message_id != NULL && req->message_id[0] != '\0') {
        snprintf(message_id, sizeof message_id, "%s", req->message_id);
    } else if (req->object_id != NULL && req->object_id[0] != '\0') {
        TBoxListItem item;
        memset(&item, 0, sizeof item);
        int item_found = 0;
        if (tbox_query_get(db, req->object_id, &item, &item_found) != TBOX_DB_OK) {
            return TBOX_DOWNLOAD_E_DB;
        }
        if (!item_found) {
            return TBOX_DOWNLOAD_E_NOT_FOUND;
        }
        snprintf(message_id, sizeof message_id, "%s", item.message_id);
    } else {
        return TBOX_DOWNLOAD_E_ARGS;
    }
    if (message_id[0] == '\0') {
        return TBOX_DOWNLOAD_E_NOT_FOUND;
    }
    if (tbox_query_message(db, message_id, msg, found) != TBOX_DB_OK) {
        return TBOX_DOWNLOAD_E_DB;
    }
    if (!*found) {
        return TBOX_DOWNLOAD_E_NOT_FOUND;
    }
    return TBOX_DOWNLOAD_OK;
}

TBoxDownloadStatus tbox_download_check(const TBoxDownloadEnv *env,
                                        const TBoxDownloadRequest *req,
                                        TBoxDownloadChecked *out, char *err,
                                        size_t err_cap) {
    if (env == NULL || req == NULL || out == NULL || env->db == NULL) {
        set_err(err, err_cap, "the download has no catalog");
        return TBOX_DOWNLOAD_E_ARGS;
    }
    memset(out, 0, sizeof *out);
    if (req->dest_dir == NULL || req->dest_dir[0] == '\0') {
        set_err(err, err_cap, "a destination folder is required");
        return TBOX_DOWNLOAD_E_ARGS;
    }
    if (!tbox_fsx_is_dir(req->dest_dir)) {
        set_err(err, err_cap, "that destination folder is not there");
        return TBOX_DOWNLOAD_E_IO;
    }
    if (req->on_exists != NULL && req->on_exists[0] != '\0' &&
        strcmp(req->on_exists, "error") != 0 &&
        strcmp(req->on_exists, "keep_both") != 0) {
        set_err(err, err_cap, "on_exists must be \"error\" or \"keep_both\"");
        return TBOX_DOWNLOAD_E_ARGS;
    }
    snprintf(out->on_exists, sizeof out->on_exists, "%s",
             (req->on_exists != NULL && req->on_exists[0] != '\0')
                 ? req->on_exists
                 : "error");
    snprintf(out->dest_dir, sizeof out->dest_dir, "%s", req->dest_dir);

    /* the message */
    TBoxMessageInfo msg;
    int found = 0;
    TBoxDownloadStatus st = resolve_message(env->db, req, &msg, &found);
    if (st != TBOX_DOWNLOAD_OK) {
        set_err(err, err_cap, tbox_download_status_text(st));
        return st;
    }
    if (strcmp(msg.kind, "file") != 0 && strcmp(msg.kind, "unmanaged") != 0) {
        set_err(err, err_cap, "that is not a file");
        return TBOX_DOWNLOAD_E_NOT_FOUND;
    }
    snprintf(out->message_id, sizeof out->message_id, "%s", msg.message_id);
    if (req->object_id != NULL && req->object_id[0] != '\0') {
        snprintf(out->object_id, sizeof out->object_id, "%.32s", req->object_id);
    }

    /* the name: the one of the record, or the name Telegram gave the file.
     * Both are untrusted, so both go through the rules of S8. */
    char raw[TBOX_NAME_MAX_BYTES * 4 + 1];
    raw[0] = '\0';
    if (msg.has_name && msg.name[0] != '\0') {
        snprintf(raw, sizeof raw, "%s", msg.name);
    } else if (msg.orig_name[0] != '\0') {
        snprintf(raw, sizeof raw, "%s", msg.orig_name);
    }
    if (raw[0] == '\0') {
        set_err(err, err_cap, "that file has no name");
        return TBOX_DOWNLOAD_E_NAME;
    }
    tbox_name_sanitize(raw, out->name, sizeof out->name);
    if (out->name[0] == '\0') {
        set_err(err, err_cap, "that name cannot be used");
        return TBOX_DOWNLOAD_E_NAME;
    }

    /* what the file should look like when it arrives */
    if (msg.has_sha256 && msg.sha256[0] != '\0') {
        out->has_sha256 = 1;
        snprintf(out->sha256, sizeof out->sha256, "%s", msg.sha256);
    }
    if (msg.has_size) {
        out->has_size = 1;
        out->size = msg.size;
    }
    return TBOX_DOWNLOAD_OK;
}

/* ---- the operation ---------------------------------------------------- */

/* The arguments of a download, as the journal keeps them: enough to run it
 * again after a crash. */
char *tbox_download_args_json(const TBoxDownloadArgs *args) {
    if (args == NULL) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "message_id",
                            args->message_id != NULL ? args->message_id : "");
    if (args->object_id != NULL && args->object_id[0] != '\0') {
        cJSON_AddStringToObject(o, "object_id", args->object_id);
    }
    cJSON_AddStringToObject(o, "dest_dir",
                            args->dest_dir != NULL ? args->dest_dir : "");
    cJSON_AddStringToObject(o, "name", args->name != NULL ? args->name : "");
    cJSON_AddStringToObject(o, "on_exists",
                            args->on_exists != NULL ? args->on_exists : "error");
    if (args->sha256 != NULL && args->sha256[0] != '\0') {
        cJSON_AddStringToObject(o, "sha256", args->sha256);
    }
    cJSON_AddNumberToObject(o, "size", (double)args->size);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

void tbox_download_args_free(TBoxDownloadArgs *args) {
    if (args == NULL) {
        return;
    }
    free(args->message_id);
    free(args->object_id);
    free(args->dest_dir);
    free(args->name);
    free(args->on_exists);
    free(args->sha256);
    memset(args, 0, sizeof *args);
    args->size = -1;
}

int tbox_download_args_parse(const char *args_json, TBoxDownloadArgs *out) {
    if (args_json == NULL || out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->size = -1;
    cJSON *root = cJSON_Parse(args_json);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return 0;
    }
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(root, "message_id");
    const cJSON *dir = cJSON_GetObjectItemCaseSensitive(root, "dest_dir");
    if (!cJSON_IsString(mid) || cJSON_GetStringValue(mid) == NULL ||
        cJSON_GetStringValue(mid)[0] == '\0' || !cJSON_IsString(dir) ||
        cJSON_GetStringValue(dir) == NULL ||
        cJSON_GetStringValue(dir)[0] == '\0') {
        cJSON_Delete(root);
        return 0;
    }
    out->message_id = TBOX_STRDUP(cJSON_GetStringValue(mid));
    out->dest_dir = TBOX_STRDUP(cJSON_GetStringValue(dir));
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "object_id");
    if (cJSON_IsString(object)) {
        out->object_id = TBOX_STRDUP(cJSON_GetStringValue(object));
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name)) {
        out->name = TBOX_STRDUP(cJSON_GetStringValue(name));
    }
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "on_exists");
    if (cJSON_IsString(mode)) {
        out->on_exists = TBOX_STRDUP(cJSON_GetStringValue(mode));
    }
    const cJSON *hash = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    if (cJSON_IsString(hash)) {
        out->sha256 = TBOX_STRDUP(cJSON_GetStringValue(hash));
    }
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (cJSON_IsNumber(size)) {
        out->size = (long long)size->valuedouble;
    }
    cJSON_Delete(root);
    if (out->message_id == NULL || out->dest_dir == NULL) {
        tbox_download_args_free(out);
        return 0;
    }
    return 1;
}

TBoxDownloadStatus tbox_download_queue(const TBoxDownloadEnv *env,
                                        const TBoxDownloadRequest *req,
                                        const TBoxDownloadChecked *checked,
                                        TBoxDownloadOp *out, char *err,
                                        size_t err_cap) {
    if (env == NULL || req == NULL || checked == NULL || out == NULL ||
        env->db == NULL) {
        set_err(err, err_cap, "the download has no catalog");
        return TBOX_DOWNLOAD_E_ARGS;
    }
    memset(out, 0, sizeof *out);
    if (tbox_rand_hex(out->op_id, sizeof out->op_id, TBOX_OP_HEX_LEN / 2) !=
        TBOX_RAND_OK) {
        set_err(err, err_cap, "no ids could be made");
        return TBOX_DOWNLOAD_E_INTERNAL;
    }

    TBoxDownloadArgs args;
    memset(&args, 0, sizeof args);
    args.size = -1;
    args.message_id = TBOX_STRDUP(checked->message_id);
    args.object_id = TBOX_STRDUP(checked->object_id);
    args.dest_dir = TBOX_STRDUP(checked->dest_dir);
    args.name = TBOX_STRDUP(checked->name);
    args.on_exists = TBOX_STRDUP(checked->on_exists);
    args.sha256 = TBOX_STRDUP(checked->sha256);
    args.size = checked->has_size ? checked->size : -1;
    char *args_json = tbox_download_args_json(&args);
    tbox_download_args_free(&args);
    if (args_json == NULL) {
        set_err(err, err_cap, "the arguments could not be written");
        return TBOX_DOWNLOAD_E_INTERNAL;
    }

    TBoxDbStatus rc = tbox_journal_insert(env->db, out->op_id,
                                          TBOX_OP_KIND_DOWNLOAD, args_json,
                                          env->now);
    free(args_json);
    if (rc != TBOX_DB_OK) {
        set_err(err, err_cap, "the operation could not be written down");
        return TBOX_DOWNLOAD_E_DB;
    }
    return TBOX_DOWNLOAD_OK;
}

/* ---- asking Telegram for the file (spec 8.6 step 2) -------------------- */

/* The file of a message: a document, a video, an audio, a voice note or one
 * of the sizes of a photo. */
static const cJSON *message_file(const cJSON *message) {
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(message, "content");
    if (content == NULL) {
        return NULL;
    }
    static const char *const direct[] = {"document", "video", "audio",
                                         "voice_note"};
    for (size_t i = 0; i < sizeof direct / sizeof direct[0]; i++) {
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(content, direct[i]);
        if (cJSON_IsObject(f)) {
            return f;
        }
    }
    const cJSON *photo = cJSON_GetObjectItemCaseSensitive(content, "photo");
    if (cJSON_IsObject(photo)) {
        const cJSON *sizes = cJSON_GetObjectItemCaseSensitive(photo, "sizes");
        if (cJSON_IsArray(sizes)) {
            const cJSON *last =
                cJSON_GetArrayItem(sizes, (int)cJSON_GetArraySize(sizes) - 1);
            const cJSON *f = cJSON_GetObjectItemCaseSensitive(last, "photo");
            if (cJSON_IsObject(f)) {
                return f;
            }
        }
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(photo, "photo");
        if (cJSON_IsObject(f)) {
            return f;
        }
    }
    return NULL;
}

/* The path of the file on this disk, when it is already there. */
static void read_local(const cJSON *file, char *path, size_t cap, int *done) {
    path[0] = '\0';
    *done = 0;
    const cJSON *local = cJSON_GetObjectItemCaseSensitive(file, "local");
    if (local == NULL) {
        return;
    }
    const cJSON *ready =
        cJSON_GetObjectItemCaseSensitive(local, "is_downloading_completed");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(local, "path");
    if (!cJSON_IsTrue(ready) || !cJSON_IsString(p) ||
        cJSON_GetStringValue(p) == NULL) {
        return;
    }
    snprintf(path, cap, "%s", cJSON_GetStringValue(p));
    *done = 1;
}

TBoxDownloadStatus tbox_download_start(const TBoxDownloadEnv *env,
                                        const char *op_id,
                                        const TBoxDownloadChecked *checked,
                                        TBoxDownloadPlan *plan, char *err,
                                        size_t err_cap) {
    if (env == NULL || op_id == NULL || op_id[0] == '\0' || checked == NULL ||
        plan == NULL || env->db == NULL || env->call == NULL) {
        set_err(err, err_cap, "the download has no catalog or no connection");
        return TBOX_DOWNLOAD_E_ARGS;
    }
    memset(plan, 0, sizeof *plan);
    snprintf(plan->op_id, sizeof plan->op_id, "%s", op_id);
    snprintf(plan->message_id, sizeof plan->message_id, "%s",
             checked->message_id);
    snprintf(plan->name, sizeof plan->name, "%s", checked->name);
    snprintf(plan->dest_dir, sizeof plan->dest_dir, "%s", checked->dest_dir);
    if (checked->has_sha256) {
        plan->has_sha256 = 1;
        snprintf(plan->sha256, sizeof plan->sha256, "%s", checked->sha256);
    }
    if (checked->has_size) {
        plan->has_size = 1;
        plan->size = checked->size;
    }

    /* the message, with the file on it */
    char request[128];
    snprintf(request, sizeof request,
             "{\"@type\":\"getMessage\",\"message_id\":%lld}",
             atoll(checked->message_id));
    cJSON *reply = env->call(env->call_ctx, request);
    if (reply == NULL) {
        set_err(err, err_cap, "Telegram did not answer with the message");
        return TBOX_DOWNLOAD_E_TD;
    }
    const cJSON *file = message_file(reply);
    if (file == NULL) {
        cJSON_Delete(reply);
        set_err(err, err_cap, "that message has no file on it");
        return TBOX_DOWNLOAD_E_NOT_FOUND;
    }
    const cJSON *fid = cJSON_GetObjectItemCaseSensitive(file, "id");
    char local_path[TBOX_DOWNLOAD_PATH_MAX];
    int complete = 0;
    read_local(file, local_path, sizeof local_path, &complete);
    long long file_size = 0;
    int has_file_size = 0;
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size");
    if (cJSON_IsNumber(size)) {
        has_file_size = 1;
        file_size = (long long)size->valuedouble;
    }
    cJSON_Delete(reply);
    if (!cJSON_IsNumber(fid)) {
        set_err(err, err_cap, "Telegram did not answer with a file");
        return TBOX_DOWNLOAD_E_TD;
    }
    snprintf(plan->file_id, sizeof plan->file_id, "%lld",
             (long long)fid->valuedouble);
    /* the catalog knows the size of the file; Telegram's own number is the
     * fallback, so a truncated download is caught either way */
    if (!plan->has_size && has_file_size) {
        plan->has_size = 1;
        plan->size = file_size;
    }

    if (!complete) {
        /* the file has to come over the wire */
        char dl[160];
        snprintf(dl, sizeof dl,
                 "{\"@type\":\"downloadFile\",\"file_id\":%lld,\"priority\":16,"
                 "\"offset\":0,\"limit\":0,\"synchronous\":false}",
                 atoll(plan->file_id));
        cJSON *dl_reply = env->call(env->call_ctx, dl);
        if (dl_reply == NULL) {
            set_err(err, err_cap, "Telegram did not take the request");
            return TBOX_DOWNLOAD_E_TD;
        }
        /* the answer is the file itself: the path is only there when the
         * file was already on this disk */
        read_local(dl_reply, local_path, sizeof local_path, &complete);
        cJSON_Delete(dl_reply);
    }
    if (complete) {
        snprintf(plan->local_path, sizeof plan->local_path, "%s", local_path);
        plan->complete = 1;
    }

    /* the operation is with Telegram now */
    if (tbox_journal_set_state(env->db, op_id, "SENT", NULL, 0, env->now) !=
        TBOX_DB_OK) {
        set_err(err, err_cap, "the operation could not be written down");
        return TBOX_DOWNLOAD_E_DB;
    }
    return TBOX_DOWNLOAD_OK;
}

/* ---- the file that lands (spec 8.6 step 4) ---------------------------- */

/* The name the file gets: the one the user wants, or the next free one
 * when the user allows both to be there. */
static TBoxDownloadStatus choose_name(const TBoxDownloadChecked *checked,
                                       char *out, size_t cap) {
    char candidate[TBOX_DOWNLOAD_PATH_MAX];
    TBoxFileInfo info;
    if (join_path(candidate, sizeof candidate, checked->dest_dir,
                  checked->name) != 0) {
        return TBOX_DOWNLOAD_E_NAME;
    }
    if (tbox_fsx_file_info(candidate, &info) != TBOX_FSX_OK || !info.exists) {
        snprintf(out, cap, "%s", checked->name);
        return TBOX_DOWNLOAD_OK;
    }
    if (strcmp(checked->on_exists, "keep_both") != 0) {
        return TBOX_DOWNLOAD_E_EXISTS;
    }
    for (int n = 2; n < 1000; n++) {
        char numbered[TBOX_NAME_MAX_BYTES + 32];
        tbox_download_numbered_name(checked->name, n, numbered, sizeof numbered);
        if (join_path(candidate, sizeof candidate, checked->dest_dir,
                      numbered) != 0) {
            break;
        }
        if (tbox_fsx_file_info(candidate, &info) != TBOX_FSX_OK || !info.exists) {
            snprintf(out, cap, "%s", numbered);
            return TBOX_DOWNLOAD_OK;
        }
    }
    return TBOX_DOWNLOAD_E_EXISTS;
}

TBoxDownloadStatus tbox_download_finish(const TBoxDownloadChecked *checked,
                                         const TBoxDownloadPlan *plan,
                                         char *path_out, size_t path_cap,
                                         char *err, size_t err_cap) {
    if (checked == NULL || plan == NULL || path_out == NULL) {
        set_err(err, err_cap, "the download is incomplete");
        return TBOX_DOWNLOAD_E_ARGS;
    }
    path_out[0] = '\0';
    if (plan->local_path[0] == '\0') {
        set_err(err, err_cap, "the file was not downloaded");
        return TBOX_DOWNLOAD_E_IO;
    }

    /* the copy lands on a temporary name first: a file that is not the one
     * that was sent never gets the name the user expects */
    char part_name[TBOX_NAME_MAX_BYTES + 32];
    snprintf(part_name, sizeof part_name, ".%s.tbox-part", checked->name);
    char part_path[TBOX_DOWNLOAD_PATH_MAX];
    if (join_path(part_path, sizeof part_path, checked->dest_dir, part_name) !=
        0) {
        set_err(err, err_cap, "that path is too long");
        return TBOX_DOWNLOAD_E_NAME;
    }
    if (tbox_fsx_copy(plan->local_path, part_path) != TBOX_FSX_OK) {
        set_err(err, err_cap, "the file could not be written");
        return TBOX_DOWNLOAD_E_IO;
    }

    /* the size has to be the one the catalog remembers */
    TBoxFileInfo info;
    if (tbox_fsx_file_info(part_path, &info) != TBOX_FSX_OK) {
        (void)tbox_fsx_remove(part_path);
        set_err(err, err_cap, "the file could not be looked at");
        return TBOX_DOWNLOAD_E_IO;
    }
    if (plan->has_size && info.size != plan->size) {
        (void)tbox_fsx_remove(part_path);
        set_err(err, err_cap, "the file is not the size it should be");
        return TBOX_DOWNLOAD_E_SIZE;
    }

    /* and the content has to be the content that was sent */
    if (plan->has_sha256) {
        char hex[TBOX_SHA256_HEX_LEN + 1];
        if (tbox_hash_file(part_path, hex, NULL) != 0) {
            (void)tbox_fsx_remove(part_path);
            set_err(err, err_cap, "the file could not be read");
            return TBOX_DOWNLOAD_E_IO;
        }
        if (strcmp(hex, plan->sha256) != 0) {
            (void)tbox_fsx_remove(part_path);
            set_err(err, err_cap, "the file is not the one that was sent");
            return TBOX_DOWNLOAD_E_HASH;
        }
    }

    /* the name it gets */
    char final_name[TBOX_NAME_MAX_BYTES + 32];
    TBoxDownloadStatus st = choose_name(checked, final_name,
                                         sizeof final_name);
    if (st != TBOX_DOWNLOAD_OK) {
        (void)tbox_fsx_remove(part_path);
        set_err(err, err_cap, "there is already a file with that name");
        return st;
    }
    char final_path[TBOX_DOWNLOAD_PATH_MAX];
    if (join_path(final_path, sizeof final_path, checked->dest_dir,
                  final_name) != 0) {
        (void)tbox_fsx_remove(part_path);
        set_err(err, err_cap, "that path is too long");
        return TBOX_DOWNLOAD_E_NAME;
    }
    if (tbox_fsx_rename_no_overwrite(part_path, final_path) != TBOX_FSX_OK) {
        (void)tbox_fsx_remove(part_path);
        set_err(err, err_cap, "the file could not be put in place");
        return TBOX_DOWNLOAD_E_IO;
    }
    snprintf(path_out, path_cap, "%s", final_path);
    return TBOX_DOWNLOAD_OK;
}

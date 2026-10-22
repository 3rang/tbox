/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * upload.h — the upload of a local file into Saved Messages (spec 8.5).
 *
 * An upload is the one thing that leaves the machine, so every step is
 * checked before anything is sent:
 *
 *   1. the file is there, and it is a file (not a folder);
 *   2. it is not bigger than the limit (2000 MiB, 4000 MiB for a premium
 *      account);
 *   3. the folder exists and is live, and the name is free in it;
 *   4. the content is hashed, so the catalog can tell later whether the file
 *      that comes back is the file that went out;
 *   5. the operation is written to the journal, so a crash in the middle
 *      leaves a trail the next run can pick up;
 *   6. the message is sent with the record as its caption - the record
 *      carries the id of the operation, which is how an upload that already
 *      went out is recognized and not sent a second time (spec 8.4).
 *
 * The work is in three pieces, so that the engine can answer the caller
 * with the operation id right away (spec 6.2) and still send the file one
 * at a time (spec 8.5):
 *
 *   tbox_upload_check  - the checks; they are quick and they answer the
 *                         caller directly.
 *   tbox_upload_queue  - the operation is written down; its id is what the
 *                         caller is told.
 *   tbox_upload_send   - the hash, the record and the message.
 *
 * The module needs three things from the outside: the catalog (through the
 * database handle), a way to talk to TDLib, and the clock. Everything else
 * - the events, the retry queue - is the engine's.
 */
#ifndef TBOX_XFER_UPLOAD_H
#define TBOX_XFER_UPLOAD_H

#include <stddef.h>

#include "cJSON.h"

#include "catalog/record.h"
#include "catalog/resolve.h"
#include "store/db.h"

/* The size limit of a file (spec 8.5). */
#define TBOX_UPLOAD_MAX_BYTES         (2000LL * 1024LL * 1024LL)
#define TBOX_UPLOAD_MAX_BYTES_PREMIUM (4000LL * 1024LL * 1024LL)

/* The longest record an upload can produce: the prefix, the fixed keys and
 * the longest name the spec allows, with room to spare. */
#define TBOX_UPLOAD_CAPTION_MAX 1024

/* What an upload request asks for. */
typedef struct TBoxUploadRequest {
    const char *local_path; /* the file to send */
    const char *folder_id;  /* "root" or the id of a folder */
    const char *name;       /* the name in the catalog; NULL: the file's own */
} TBoxUploadRequest;

/* Sends one TDLib request and returns the parsed reply, which the caller
 * owns and frees with cJSON_Delete. NULL when the call did not come back. */
typedef cJSON *(*TBoxUploadCallFn)(void *ctx, const char *request);

/* The world the upload needs. */
typedef struct TBoxUploadEnv {
    TBoxDb *db;
    TBoxUploadCallFn call;
    void *call_ctx;
    long long chat_id;     /* Saved Messages */
    unsigned caption_limit; /* TDLib's message_caption_length_max; 0: none */
    int is_premium;        /* a premium account may send twice as much */
    long long max_bytes;   /* the limit that applies; 0: the account's */
    long long now;         /* the Unix seconds the rows are stamped with */
} TBoxUploadEnv;

/* What the checks of spec 8.5 found out. */
typedef struct TBoxUploadChecked {
    char parent[TBOX_ID_HEX_LEN + 8]; /* "root" or a 32 hex id */
    char name[TBOX_NAME_MAX_BYTES + 1];
    long long size;
    long long mtime;
} TBoxUploadChecked;

/* The ids an upload goes by. */
typedef struct TBoxUploadOp {
    char op_id[TBOX_OP_HEX_LEN + 1];     /* what the protocol answers with */
    char object_id[TBOX_ID_HEX_LEN + 1]; /* what the record carries */
} TBoxUploadOp;

/* What an upload is: the ids, the record and the message that carries it. */
typedef struct TBoxUploadPlan {
    char op_id[TBOX_OP_HEX_LEN + 1];
    char object_id[TBOX_ID_HEX_LEN + 1];
    char parent[TBOX_ID_HEX_LEN + 8];
    char name[TBOX_NAME_MAX_BYTES + 1];
    long long rev;
    long long size;
    long long mtime;
    char sha256[TBOX_SHA256_HEX_LEN + 1];
    char caption[TBOX_UPLOAD_CAPTION_MAX];
    char file_id[TBOX_MSGID_MAX_BYTES + 1];    /* the file being uploaded */
    char message_id[TBOX_MSGID_MAX_BYTES + 1]; /* the temporary message id */
} TBoxUploadPlan;

/* Why an upload did not happen. */
typedef enum TBoxUploadStatus {
    TBOX_UPLOAD_OK = 0,
    TBOX_UPLOAD_E_ARGS,      /* the request is missing something */
    TBOX_UPLOAD_E_FILE,      /* there is no file at that path */
    TBOX_UPLOAD_E_TOO_LARGE, /* over the size limit */
    TBOX_UPLOAD_E_FOLDER,    /* the folder is not there, or is not live */
    TBOX_UPLOAD_E_NAME,      /* the name cannot be used */
    TBOX_UPLOAD_E_EXISTS,    /* the name is taken in that folder */
    TBOX_UPLOAD_E_IO,        /* the file could not be read */
    TBOX_UPLOAD_E_CAPTION,   /* the record does not fit in a caption */
    TBOX_UPLOAD_E_DB,        /* the journal could not be written */
    TBOX_UPLOAD_E_TD,        /* Telegram refused the message */
    TBOX_UPLOAD_E_INTERNAL   /* something went wrong on this side */
} TBoxUploadStatus;

/* A short name for a status, for the log. */
const char *tbox_upload_status_text(TBoxUploadStatus status);

/* The largest file that may be sent. */
long long tbox_upload_size_limit(int is_premium);

/* The name a file goes by when the caller does not give one: the last part
 * of the path, without the directories. Both separators are understood. */
void tbox_upload_name_from_path(const char *path, char *out, size_t out_size);

/* The checks of spec 8.5: the file is there and is a file, it is not too
 * big, the folder is live, and the name is free in it.
 *
 * Returns TBOX_UPLOAD_OK and fills *out. When the status is not OK, the
 * reason is written to err (when err is not NULL). */
TBoxUploadStatus tbox_upload_check(const TBoxUploadEnv *env,
                                    const TBoxUploadRequest *req,
                                    TBoxUploadChecked *out, char *err,
                                    size_t err_cap);

/* Writes the operation down (spec 8.5 step 4) and hands back the ids: the
 * op id is what the protocol answers with, the object id is what the
 * record carries. The operation starts in the QUEUED state; the engine
 * picks it up when it is its turn (one upload at a time). */
TBoxUploadStatus tbox_upload_queue(const TBoxUploadEnv *env,
                                    const TBoxUploadRequest *req,
                                    const TBoxUploadChecked *checked,
                                    TBoxUploadOp *out, char *err,
                                    size_t err_cap);

/* The rest of the upload (spec 8.5 steps 3 and 5): the hash, the record and
 * the message that carries it.
 *
 * The operation must already be in the journal and in the RUNNING state:
 * the engine owns the states (spec 8.4) and the waits between the tries.
 * On success the operation waits for the message to arrive (the SENT state)
 * and the plan holds the ids of the file and of the temporary message. */
TBoxUploadStatus tbox_upload_send(const TBoxUploadEnv *env, const char *op_id,
                                   const char *object_id,
                                   const TBoxUploadChecked *checked,
                                   const char *local_path,
                                   TBoxUploadPlan *plan, char *err,
                                   size_t err_cap);

/* ---- the arguments of an upload, as the journal keeps them ------------- */

/* What the journal remembers about an upload: enough to run it again. The
 * strings are heap text; free them with tbox_upload_args_free(). */
typedef struct TBoxUploadArgs {
    char *local_path;
    char *folder_id;
    char *name;      /* NULL when the file's own name is used */
    char *object_id; /* the object this upload is */
} TBoxUploadArgs;

/* Reads the arguments of an operation back. Returns 0 when the json is not
 * an upload's arguments. */
int tbox_upload_args_parse(const char *args_json, TBoxUploadArgs *out);

/* The arguments as json, for the journal. The caller frees the result. */
char *tbox_upload_args_json(const TBoxUploadArgs *args);

void tbox_upload_args_free(TBoxUploadArgs *args);

#endif /* TBOX_XFER_UPLOAD_H */

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * download.h — the download of a file from Saved Messages (spec 8.6).
 *
 * A download is the other half of the upload: a file that was sent earlier
 * comes back to a folder the user picked. The steps are the ones of the
 * spec:
 *
 *   1. the message is resolved and the name is made safe (S8);
 *   2. Telegram is asked for the file (downloadFile);
 *   3. the progress comes with updateFile;
 *   4. when the file is there it is copied next to its destination under a
 *      hidden name, checked against the size and the hash the catalog
 *      remembers, and only then given its real name.
 *
 * The last point is the one that matters: a file that is half there, or that
 * is not the file that was sent, never gets the name the user expects. The
 * copy lands on a temporary name first, so a failure leaves nothing behind
 * that looks like a finished download.
 *
 * The work is in four pieces, so that the engine can answer the caller with
 * the operation id right away (spec 6.2) and still do the waiting and the
 * writing itself:
 *
 *   tbox_download_check  - the checks; they are quick and they answer the
 *                          caller directly.
 *   tbox_download_queue  - the operation is written down.
 *   tbox_download_start  - the file is asked for.
 *   tbox_download_finish - the copy, the check and the rename.
 *
 * The module needs the catalog (through the database handle), a way to talk
 * to TDLib and the clock. Everything else - the events, the retry queue, the
 * two-at-a-time rule - is the engine's.
 */
#ifndef TBOX_XFER_DOWNLOAD_H
#define TBOX_XFER_DOWNLOAD_H

#include <stddef.h>

#include "cJSON.h"

#include "catalog/record.h"
#include "store/db.h"

/* The longest path the module builds: the destination folder and the name
 * of the file in it. */
#define TBOX_DOWNLOAD_PATH_MAX 600

/* What a download request asks for. */
typedef struct TBoxDownloadRequest {
    const char *object_id;  /* the object to fetch; may be NULL */
    const char *message_id; /* or one message; may be NULL */
    const char *dest_dir;   /* where the file goes */
    const char *on_exists;  /* "error" (the default) or "keep_both" */
} TBoxDownloadRequest;

/* Sends one TDLib request and returns the parsed reply, which the caller
 * owns and frees with cJSON_Delete. NULL when the call did not come back. */
typedef cJSON *(*TBoxDownloadCallFn)(void *ctx, const char *request);

/* The world the download needs. */
typedef struct TBoxDownloadEnv {
    TBoxDb *db;
    TBoxDownloadCallFn call;
    void *call_ctx;
    long long now; /* the Unix seconds the rows are stamped with */
} TBoxDownloadEnv;

/* What the checks of spec 8.6 found out. */
typedef struct TBoxDownloadChecked {
    char      message_id[TBOX_MSGID_MAX_BYTES + 1];
    char      object_id[TBOX_ID_HEX_LEN + 1];
    char      name[TBOX_NAME_MAX_BYTES + 1];
    char      dest_dir[TBOX_DOWNLOAD_PATH_MAX];
    char      on_exists[16];
    char      sha256[TBOX_SHA256_HEX_LEN + 1];
    int       has_sha256;
    long long size;
    int       has_size;
} TBoxDownloadChecked;

/* The id of the operation. */
typedef struct TBoxDownloadOp {
    char op_id[TBOX_OP_HEX_LEN + 1];
} TBoxDownloadOp;

/* What a download is doing: which file, and where it goes. */
typedef struct TBoxDownloadPlan {
    char      op_id[TBOX_OP_HEX_LEN + 1];
    char      message_id[TBOX_MSGID_MAX_BYTES + 1];
    char      file_id[TBOX_MSGID_MAX_BYTES + 1];
    char      local_path[TBOX_DOWNLOAD_PATH_MAX]; /* where TDLib put it */
    char      name[TBOX_NAME_MAX_BYTES + 1];
    char      dest_dir[TBOX_DOWNLOAD_PATH_MAX];
    char      sha256[TBOX_SHA256_HEX_LEN + 1];
    int       has_sha256;
    long long size;
    int       has_size;
    int       complete; /* the file is already on this disk */
} TBoxDownloadPlan;

/* Why a download did not happen. */
typedef enum TBoxDownloadStatus {
    TBOX_DOWNLOAD_OK = 0,
    TBOX_DOWNLOAD_E_ARGS,     /* the request is missing something */
    TBOX_DOWNLOAD_E_NOT_FOUND,/* there is no such file */
    TBOX_DOWNLOAD_E_NAME,     /* the name cannot be used */
    TBOX_DOWNLOAD_E_EXISTS,   /* the name is taken and may not be shared */
    TBOX_DOWNLOAD_E_IO,       /* the file could not be read or written */
    TBOX_DOWNLOAD_E_HASH,     /* the file is not the one that was sent */
    TBOX_DOWNLOAD_E_SIZE,     /* the file is not the size it should be */
    TBOX_DOWNLOAD_E_DB,       /* the journal could not be written */
    TBOX_DOWNLOAD_E_TD,       /* Telegram refused */
    TBOX_DOWNLOAD_E_INTERNAL  /* something went wrong on this side */
} TBoxDownloadStatus;

/* A short name for a status, for the log. */
const char *tbox_download_status_text(TBoxDownloadStatus status);

/* The checks of spec 8.6 step 1: the message is resolved, the name is made
 * safe (S8) and the destination folder has to be there.
 *
 * Returns TBOX_DOWNLOAD_OK and fills *out. When the status is not OK, the
 * reason is written to err (when err is not NULL). */
TBoxDownloadStatus tbox_download_check(const TBoxDownloadEnv *env,
                                        const TBoxDownloadRequest *req,
                                        TBoxDownloadChecked *out, char *err,
                                        size_t err_cap);

/* Writes the operation down so it survives a crash, and hands back the id
 * the protocol answers with. The operation starts in the QUEUED state; the
 * engine picks it up when it is its turn (two downloads at a time). */
TBoxDownloadStatus tbox_download_queue(const TBoxDownloadEnv *env,
                                        const TBoxDownloadRequest *req,
                                        const TBoxDownloadChecked *checked,
                                        TBoxDownloadOp *out, char *err,
                                        size_t err_cap);

/* Asks Telegram for the file (spec 8.6 step 2). When the file is already on
 * this disk the plan says so and the caller finishes it right away. */
TBoxDownloadStatus tbox_download_start(const TBoxDownloadEnv *env,
                                        const char *op_id,
                                        const TBoxDownloadChecked *checked,
                                        TBoxDownloadPlan *plan, char *err,
                                        size_t err_cap);

/* The last step (spec 8.6 step 4): the copy, the check of the size and the
 * hash, and the rename to the name the user expects. The path of the file
 * that landed is written to path_out.
 *
 * A failure leaves nothing behind: the temporary file goes away and no file
 * with the final name is created. */
TBoxDownloadStatus tbox_download_finish(const TBoxDownloadChecked *checked,
                                         const TBoxDownloadPlan *plan,
                                         char *path_out, size_t path_cap,
                                         char *err, size_t err_cap);

/* The name a file gets when the one the user wants is taken: "report.pdf"
 * becomes "report (2).pdf", then "report (3).pdf" and so on. */
void tbox_download_numbered_name(const char *name, int number, char *out,
                                 size_t out_cap);

/* ---- the arguments of a download, as the journal keeps them ------------- */

/* What the journal remembers about a download: enough to run it again. The
 * strings are heap text; free them with tbox_download_args_free(). */
typedef struct TBoxDownloadArgs {
    char *message_id;
    char *object_id;
    char *dest_dir;
    char *name;
    char *on_exists;
    char *sha256;   /* empty when the catalog does not know the hash */
    long long size; /* -1 when the catalog does not know the size */
} TBoxDownloadArgs;

/* The arguments as json, for the journal. The caller frees the result. */
char *tbox_download_args_json(const TBoxDownloadArgs *args);

/* Reads the arguments of an operation back. Returns 0 when the json is not
 * a download's arguments. */
int tbox_download_args_parse(const char *args_json, TBoxDownloadArgs *out);

void tbox_download_args_free(TBoxDownloadArgs *args);

#endif /* TBOX_XFER_DOWNLOAD_H */

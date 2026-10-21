/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * log.h — the engine's log file: one-line JSON records, redacted, rotated
 * by size (spec 9.5).
 *
 * File: <dir>/logs/engine.log plus rotations engine.log.1 / engine.log.2
 * (the .1 file is the newest fully-written one). Every record
 *    {"ts":"2026-10-21T12:00:00Z","lvl":"info","mod":"xfer",
 *     "op":"…","msg":"…","code":"…"}
 * obeys rule S6: no user content ever reaches the file - no message text,
 * captions, file names, QR tokens, passwords, keys or phone numbers; IDs
 * and error codes only. Two layers enforce that:
 *
 *   1. Structurally, callers log fixed short strings, never user data.
 *   2. Defensively, tbox_log_write() scrubs every text it is handed
 *      through tbox_log_redact(), so a caller mistake can never leak.
 */
#ifndef TBOX_LOG_H
#define TBOX_LOG_H

#include <stddef.h>

/* Spec 9.5 defaults: rotate at 5 MiB, keep 3 files. A TBoxLog opened with
 * rotate_bytes == 0 or keep_files < 1 takes these. */
#define TBOX_LOG_ROTATE_BYTES ((size_t)5 << 20)
#define TBOX_LOG_KEEP_FILES 3

/* Redacted text never exceeds this many bytes including the NUL. */
#define TBOX_LOG_MAX_TEXT 512

typedef enum TBoxLogLevel {
    TBOX_LOG_DEBUG = 0,
    TBOX_LOG_INFO = 1,
    TBOX_LOG_WARN = 2,
    TBOX_LOG_ERROR = 3
} TBoxLogLevel;

typedef enum TBoxLogStatus {
    TBOX_LOG_OK = 0,
    TBOX_LOG_E_ARGS = -1,  /* NULL or empty required argument */
    TBOX_LOG_E_MEM = -2,   /* out of memory */
    TBOX_LOG_E_MKDIR = -3, /* could not create the logs directory */
    TBOX_LOG_E_OPEN = -4,  /* could not open or rotate the log file */
    TBOX_LOG_E_IO = -5     /* could not write the record */
} TBoxLogStatus;

typedef struct TBoxLog TBoxLog;

/* Opens <dir>/logs/engine.log, creating <dir>/logs as needed. The file
 * appends across engine restarts; rotation happens when the NEXT record
 * would push it past rotate_bytes, so a file never exceeds the limit by
 * one record. Failures are reported through the return code; a failed
 * open must not crash the engine - log failures are never fatal. */
TBoxLogStatus tbox_log_open(const char *dir, size_t rotate_bytes, int keep_files, TBoxLog **out);
void tbox_log_close(TBoxLog *log); /* NULL is a no-op */

/* One JSON record; op and code may be NULL to omit the field. msg and
 * code are redacted before they are written. Records of the same shape
 * are byte-identical in length, which keeps rotation accounting simple. */
TBoxLogStatus tbox_log_write(TBoxLog *log, TBoxLogLevel lvl, const char *mod,
                             const char *op, const char *msg, const char *code);

/* The redaction unit (S6, defense in depth): copies src into out
 *   - dropping control characters (tab survives), and
 *   - replacing the value of any secret-bearing span with "<redacted>",
 *   - bounding the copy to min(out_size, TBOX_LOG_MAX_TEXT) - 1 bytes,
 *     cut at a UTF-8 code point boundary.
 * Triggers, matched case-insensitively: password=, passwd=, api_hash=,
 * secret=, token=, key=. The value runs to the first delimiter (space,
 * tab, comma, semicolon, quote, &, ), }, |) or the end of the text - that
 * covers URLs like tg://login?token=... . out[0] is set to '\0' first if
 * there is nothing to say; out NULL or out_size 0 writes nothing. */
char *tbox_log_redact(char *out, size_t out_size, const char *src);

#endif /* TBOX_LOG_H */

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * log.c — JSON-lines file log with redaction and size rotation (spec 9.5,
 * rule S6). See log.h for the contract; notes that only matter here:
 *
 *   - Redaction never rewrites a buffer it is also reading: masking runs
 *     from a raw copy into a second buffer, so an inserted "<redacted>"
 *     can never clobber unread input.
 *   - Rotation must close the file BEFORE renaming (Windows refuses to
 *     rename a file another handle has open), then create a fresh file,
 *     so the chain is: close, delete the oldest, shift the rest up,
 *     rename the current one, open fresh.
 *   - Writing never fails the caller's operation: a log that cannot be
 *     written is diagnostics lost, not work lost. The engine only checks
 *     the status code where it wants to (either way it keeps running).
 */
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h> /* CRITICAL_SECTION */
#else
#include <pthread.h>
#endif

#include "cJSON.h"
#include "util/fsx.h"

/* Path buffers: same 600-byte path rule as the session code; work buffers
 * for redaction are stack arrays so threads never share them. */
#define TBOX_LOG_PATH 600
#define TBOX_LOG_RAW 2048  /* inputs are copied bounded to this first */
#define TBOX_LOG_WORK 2560 /* masking lives here before the final bound */

struct TBoxLog {
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    FILE *fp;
    size_t bytes;         /* bytes written to the current file */
    size_t rotate_bytes;
    int keep_files;
    char path[TBOX_LOG_PATH]; /* <dir>/logs/engine.log */
};

/* ---- time ------------------------------------------------------------ */

/* UTC, second resolution, exactly 20 chars + NUL: 2026-10-21T12:00:00Z.
 * log.c is the ONE place in the C core that reads a clock: a log line
 * without a timestamp is worthless, unlike user-visible text, which the
 * UI owns wording (and clocks) for. */
static void log_ts_utc(char out[24]) {
    time_t now = time(NULL);
    struct tm tm_utc;
#ifdef _WIN32
    if (gmtime_s(&tm_utc, &now) != 0) {
        memset(&tm_utc, 0, sizeof tm_utc);
    }
#else
    struct tm *r = gmtime_r(&now, &tm_utc);
    if (r == NULL) {
        memset(&tm_utc, 0, sizeof tm_utc);
    }
#endif
    if (strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        out[0] = '\0'; /* cannot happen with cap 24, but stay honest */
    }
}

/* ---- redaction (S6) --------------------------------------------------- */

static const char MASKED[] = "<redacted>";

static const char *const TRIGGERS[] = {
    "password=", "passwd=", "api_hash=", "secret=", "token=", "key="
};

static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* Case-insensitive prefix match of one trigger at s. */
static int trigger_at(const unsigned char *s, const char *trig) {
    while (*trig != '\0') {
        if (ascii_lower(*s) != ascii_lower((unsigned char)*trig)) {
            return 0;
        }
        s++;
        trig++;
    }
    return 1;
}

/* Value spans end at the first delimiter or end of text. */
static int is_value_delim(unsigned char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == ',' || c == ';' ||
           c == '\"' || c == '\'' || c == '&' || c == ')' || c == '}' || c == '|';
}

/* Appends what fits of a range to out (cap bytes incl. NUL); returns the
 * new used count. snprintf-style bounds, no unchecked arithmetic. */
static size_t put_range(char *dst, size_t cap, size_t used, const char *src, size_t n) {
    if (used + 1 >= cap) {
        return used;
    }
    size_t room = cap - 1 - used;
    size_t take = n < room ? n : room;
    memcpy(dst + used, src, take);
    return used + take;
}

/* Writes the masked form of in (already bounded, controls dropped) into
 * out: for every secret span the value is replaced by "<redacted>". */
static void mask_spans(const char *in, char out[], size_t cap) {
    size_t len = strlen(in);
    size_t used = 0;
    size_t r = 0;
    while (r < len) {
        /* earliest trigger at or after r */
        size_t best = len;
        size_t tlen = 0;
        for (size_t t = 0; t < sizeof TRIGGERS / sizeof TRIGGERS[0]; t++) {
            size_t tl = strlen(TRIGGERS[t]);
            for (size_t i = r; i + tl <= len; i++) {
                if (trigger_at((const unsigned char *)(in + i), TRIGGERS[t])) {
                    if (i < best) {
                        best = i;
                        tlen = tl;
                    }
                    break; /* earliest hit for this trigger */
                }
            }
        }
        if (best == len) {
            break; /* no more triggers; tail copied below */
        }
        /* keep the trigger (the label is useful in a log), mask its value */
        used = put_range(out, cap, used, in + r, best + tlen - r);
        if (used + sizeof MASKED - 1 + 1 > cap) {
            r = len; /* no room for the marker itself: drop the rest */
            break;
        }
        memcpy(out + used, MASKED, sizeof MASKED - 1);
        used += sizeof MASKED - 1;
        /* skip only the value up to its delimiter */
        size_t v = best + tlen;
        while (v < len && !is_value_delim((unsigned char)in[v])) {
            v++;
        }
        r = v;
    }
    used = put_range(out, cap, used, in + r, len - r);
    out[used] = '\0';
}

/* Longest prefix of s (maxbytes or fewer) that never splits a code point.
 * A code point needs 2-4 bytes according to its lead byte; anything else
 * (ASCII, or an odd continuation byte) counts as one byte. */
static size_t utf8_cut(const char *s, size_t maxbytes) {
    size_t i = 0;
    while (i < maxbytes) {
        unsigned char lead = (unsigned char)s[i];
        size_t need = 1;
        if (lead >= 0xC0) {
            if ((lead & 0xE0) == 0xC0) {
                need = 2;
            } else if ((lead & 0xF0) == 0xE0) {
                need = 3;
            } else if ((lead & 0xF8) == 0xF0) {
                need = 4;
            }
        }
        if (i + need > maxbytes) {
            break;
        }
        i += need;
    }
    return i;
}

char *tbox_log_redact(char *out, size_t out_size, const char *src) {
    if (out == NULL || out_size == 0) {
        return out;
    }
    out[0] = '\0';
    char raw[TBOX_LOG_RAW];
    char masked[TBOX_LOG_WORK];
    if (src == NULL) {
        return out;
    }
    /* 1. bounded copy, control characters dropped (tab survives) */
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '\t' || (c >= 0x20 && c != 0x7F)) {
            if (w + 1 >= sizeof raw) {
                break;
            }
            raw[w++] = (char)c;
        }
    }
    raw[w] = '\0';
    /* 2. mask secret spans (read raw, write masked - no aliasing) */
    mask_spans(raw, masked, sizeof masked);
    /* 3. final bound + UTF-8-safe cut into the caller's buffer */
    size_t cap = out_size - 1 < TBOX_LOG_MAX_TEXT - 1 ? out_size - 1 : TBOX_LOG_MAX_TEXT - 1;
    size_t cut = utf8_cut(masked, cap);
    memcpy(out, masked, cut);
    out[cut] = '\0';
    return out;
}

/* ---- files ------------------------------------------------------------ */

/* All file operations go through util/fsx, the one doorway from a UTF-8
 * path to the OS: W functions on Windows, so a profile directory under a
 * non-ASCII user name works exactly like an ASCII one. */

/* Builds <path>[.<n>]; the snprintf result is checked (§16) - truncation
 * cannot happen by construction (600-byte path + 3-char suffix), and a
 * failure still yields an empty string, never a half-written path. */
static int log_path_n(const TBoxLog *log, int n, char *dst, size_t cap) {
    int wrote = n <= 0 ? snprintf(dst, cap, "%s", log->path)
                       : snprintf(dst, cap, "%s.%d", log->path, n);
    if (wrote < 0 || (size_t)wrote >= cap) {
        dst[0] = '\0';
        return -1;
    }
    return 0;
}

/* Rotates: caller has flushed; the current file is closed if open. */
static TBoxLogStatus log_rotate(TBoxLog *log) {
    char from[TBOX_LOG_PATH + 16];
    char to[TBOX_LOG_PATH + 16];
    if (log->keep_files >= 2) {
        /* the file that would fall out of the window ages out first */
        if (log_path_n(log, log->keep_files - 1, to, sizeof to) == 0) {
            (void)tbox_fsx_remove(to);
        }
        for (int i = log->keep_files - 2; i >= 1; i--) {
            if (log_path_n(log, i, from, sizeof from) != 0 ||
                log_path_n(log, i + 1, to, sizeof to) != 0) {
                break; /* cannot happen by construction; stay safe */
            }
            (void)tbox_fsx_rename(from, to);
        }
        if (log_path_n(log, 1, to, sizeof to) == 0 &&
            tbox_fsx_rename(log->path, to) != TBOX_FSX_OK) {
            (void)tbox_fsx_remove(log->path);
        }
    } else {
        (void)tbox_fsx_remove(log->path);
    }
    log->fp = tbox_fsx_fopen(log->path, "wb");
    if (log->fp == NULL) {
        return TBOX_LOG_E_OPEN;
    }
    log->bytes = 0;
    return TBOX_LOG_OK;
}

/* Reopens the current file after open/rotation failure or close, sized
 * from reality (the file may have grown on disk). */
static TBoxLogStatus log_reopen(TBoxLog *log) {
    log->fp = tbox_fsx_fopen(log->path, "ab");
    if (log->fp == NULL) {
        return TBOX_LOG_E_OPEN;
    }
    if (fseek(log->fp, 0, SEEK_END) == 0) {
        long size = ftell(log->fp);
        if (size >= 0) {
            log->bytes = (size_t)size;
        }
    }
    return TBOX_LOG_OK;
}

TBoxLogStatus tbox_log_open(const char *dir, size_t rotate_bytes, int keep_files, TBoxLog **out) {
    if (out == NULL) {
        return TBOX_LOG_E_ARGS;
    }
    *out = NULL;
    if (dir == NULL || dir[0] == '\0') {
        return TBOX_LOG_E_ARGS;
    }
    TBoxLog *log = calloc(1, sizeof *log);
    if (log == NULL) {
        return TBOX_LOG_E_MEM;
    }
#ifdef _WIN32
    InitializeCriticalSection(&log->lock);
#else
    pthread_mutex_init(&log->lock, NULL);
#endif
    log->rotate_bytes = rotate_bytes != 0 ? rotate_bytes : TBOX_LOG_ROTATE_BYTES;
    log->keep_files = keep_files >= 1 ? keep_files : TBOX_LOG_KEEP_FILES;
    char logsdir[TBOX_LOG_PATH];
    int n = snprintf(logsdir, sizeof logsdir, "%s/logs", dir);
    if (n < 0 || (size_t)n >= sizeof logsdir) {
        tbox_log_close(log);
        return TBOX_LOG_E_ARGS;
    }
    if (tbox_fsx_mkdirs(logsdir) != TBOX_FSX_OK) {
        tbox_log_close(log);
        return TBOX_LOG_E_MKDIR;
    }
    n = snprintf(log->path, sizeof log->path, "%s/engine.log", logsdir);
    if (n < 0 || (size_t)n >= sizeof log->path) {
        tbox_log_close(log);
        return TBOX_LOG_E_ARGS;
    }
    TBoxLogStatus st = log_reopen(log);
    if (st != TBOX_LOG_OK) {
        tbox_log_close(log);
        return st;
    }
    *out = log;
    return TBOX_LOG_OK;
}

void tbox_log_close(TBoxLog *log) {
    if (log == NULL) {
        return;
    }
    if (log->fp != NULL) {
        (void)fflush(log->fp);
        (void)fclose(log->fp);
        log->fp = NULL;
    }
#ifdef _WIN32
    DeleteCriticalSection(&log->lock);
#else
    pthread_mutex_destroy(&log->lock);
#endif
    free(log);
}

TBoxLogStatus tbox_log_write(TBoxLog *log, TBoxLogLevel lvl, const char *mod,
                             const char *op, const char *msg, const char *code) {
    if (log == NULL || mod == NULL || mod[0] == '\0' || msg == NULL ||
        lvl < TBOX_LOG_DEBUG || lvl > TBOX_LOG_ERROR) {
        return TBOX_LOG_E_ARGS;
    }
    /* everything textual goes through redaction first, always */
    char rmsg[TBOX_LOG_MAX_TEXT];
    (void)tbox_log_redact(rmsg, sizeof rmsg, msg);
    char rcode[TBOX_LOG_MAX_TEXT];
    if (code != NULL) {
        (void)tbox_log_redact(rcode, sizeof rcode, code);
    }
    char ts[24];
    log_ts_utc(ts);
    static const char *const LVL_NAME[] = {"debug", "info", "warn", "error"};
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return TBOX_LOG_E_MEM;
    }
    cJSON_AddStringToObject(o, "ts", ts);
    cJSON_AddStringToObject(o, "lvl", LVL_NAME[lvl]);
    cJSON_AddStringToObject(o, "mod", mod);
    if (op != NULL && op[0] != '\0') {
        cJSON_AddStringToObject(o, "op", op);
    }
    cJSON_AddStringToObject(o, "msg", rmsg);
    if (code != NULL && rcode[0] != '\0') {
        cJSON_AddStringToObject(o, "code", rcode);
    }
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (line == NULL) {
        return TBOX_LOG_E_MEM;
    }
    TBoxLogStatus status = TBOX_LOG_OK;
    size_t need = strlen(line) + 1; /* the newline on disk */
#ifdef _WIN32
    EnterCriticalSection(&log->lock);
#else
    pthread_mutex_lock(&log->lock);
#endif
    if (log->fp == NULL) {
        status = log_reopen(log);
    }
    if (status == TBOX_LOG_OK && log->bytes > 0 && log->bytes + need > log->rotate_bytes) {
        (void)fflush(log->fp);
        (void)fclose(log->fp);
        log->fp = NULL;
        status = log_rotate(log);
    }
    if (status == TBOX_LOG_OK) {
        int written = fprintf(log->fp, "%s\n", line);
        if (written < 0) {
            (void)fclose(log->fp); /* next write reopens and resizes */
            log->fp = NULL;
            status = TBOX_LOG_E_IO;
        } else {
            log->bytes += (size_t)written;
        }
    }
#ifdef _WIN32
    LeaveCriticalSection(&log->lock);
#else
    pthread_mutex_unlock(&log->lock);
#endif
    free(line);
    return status;
}

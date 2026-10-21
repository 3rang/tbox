/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_log.c — the engine's file log without tdjson, network or terminal:
 * redaction (S6), record shape (9.5), rotation at the size limit, and the
 * argument/failure paths. The integration checks read the actual file the
 * way the later no-secrets grep will.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h> /* MultiByteToWideChar, _wfopen for the wide reader */
#define TBOX_ACCESS _access
#define TBOX_F_OK 0
#else
#include <unistd.h>
#define TBOX_ACCESS access
#define TBOX_F_OK F_OK
#endif

#include "util/log.h"
#include "cJSON.h"

static const char *ROOT = "tbox_log_test";

static FILE *open_ro(const char *path) {
#ifdef _WIN32
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0) {
        return NULL;
    }
    return f;
#else
    return fopen(path, "rb");
#endif
}

/* Whole-file read with a fresh handle; asserts that the file is there. */
static char *slurp_open(FILE *f, size_t *out_len) {
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size >= 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    char *buf = malloc((size_t)size + 1);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)size, f) == (size_t)size);
    (void)fclose(f);
    buf[size] = '\0';
    *out_len = (size_t)size;
    return buf;
}

static char *slurp(const char *path, size_t *out_len) {
    return slurp_open(open_ro(path), out_len);
}

/* The reader twin of the log module's W-API rule (§16): reads a file
 * whose path may contain non-ASCII characters, exactly like the engine
 * writes it. */
static char *slurp_w(const char *utf8, size_t *out_len) {
#ifdef _WIN32
    wchar_t w[400];
    assert(MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 400) > 0);
    FILE *f = _wfopen(w, L"rb");
#else
    FILE *f = fopen(utf8, "rb");
#endif
    return slurp_open(f, out_len);
}

/* Deletes every rotation state <dir>/logs/engine.log* might have left. */
static void wipe(const char *dir) {
    char p[400];
    static const char *const SUFFIX[] = {"", ".1", ".2", ".3"};
    for (size_t i = 0; i < sizeof SUFFIX / sizeof SUFFIX[0]; i++) {
        snprintf(p, sizeof p, "%s/logs/engine.log%s", dir, SUFFIX[i]);
        (void)remove(p);
    }
}

/* Creates an empty file where a test needs a directory name taken. */
static void touch(const char *path) {
    FILE *f = open_ro(path);
    if (f != NULL) {
        (void)fclose(f);
        return;
    }
#ifdef _WIN32
    f = NULL;
    assert(fopen_s(&f, path, "wb") == 0);
#else
    f = fopen(path, "wb");
#endif
    assert(f != NULL);
    (void)fclose(f);
}

static int count_char(const char *s, char c) {
    int n = 0;
    for (; *s != '\0'; s++) {
        if (*s == c) {
            n++;
        }
    }
    return n;
}

/* NUL-delimited repeats of one 2-byte code point (é lives at 0xC3 0xA9). */
static void fill_repeat(char *dst, size_t cap, size_t reps) {
    static const unsigned char E_BYTES[] = {0xC3u, 0xA9u};
    size_t used = 0;
    for (size_t i = 0; i < reps; i++) {
        if (used + sizeof E_BYTES + 1 > cap) {
            break;
        }
        memcpy(dst + used, E_BYTES, sizeof E_BYTES);
        used += sizeof E_BYTES;
    }
    dst[used] = '\0';
}

/* ---- redaction: the S6 unit ------------------------------------------ */

static void test_redact_triggers(void) {
    char out[TBOX_LOG_MAX_TEXT];
    static const struct {
        const char *in;
        const char *want;
    } CASES[] = {
        {"tg://login?token=AA2FF0C1-CAFE-BEEF", "tg://login?token=<redacted>"},
        {"password=hunter2 done", "password=<redacted> done"},
        {"api_hash=0123456789abcdef0123456789abcdef, next", "api_hash=<redacted>, next"},
        {"key=abc;def", "key=<redacted>;def"},
        {"notes Token=XYZ end", "notes Token=<redacted> end"},
        {"two key=A and key=B", "two key=<redacted> and key=<redacted>"},
        {"FLOOD_WAIT_3 please retry", "FLOOD_WAIT_3 please retry"},
        {"plain text, nothing secret", "plain text, nothing secret"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char *r = tbox_log_redact(out, sizeof out, CASES[i].in);
        assert(r == out);
        assert(strcmp(out, CASES[i].want) == 0);
    }
    /* control characters are dropped; the tab survives ("e" is split from
     * \x01 — a hex escape would swallow it, 0x1E is not what we mean) */
    assert(strcmp(tbox_log_redact(out, sizeof out, "a\tb\nc\rd\x01" "e"), "a\tbcde") == 0);
    /* an empty value still masks the span */
    assert(strcmp(tbox_log_redact(out, sizeof out, "password="), "password=<redacted>") == 0);
    /* tiny caller buffers: never write past the bound (not called "small":
     * windows.h's rpcndr.h defines that as a macro) */
    char tiny[4];
    assert(strcmp(tbox_log_redact(tiny, sizeof tiny, "token=SECRET"), "tok") == 0);
    /* degenerate calls are safe and say nothing */
    assert(tbox_log_redact(out, 0, "never written") == out);
    assert(strcmp(tbox_log_redact(out, sizeof out, NULL), "") == 0);
    /* a long plain text is bounded and unharmed outside the secret span */
    char big[900];
    memset(big, 'x', 500);
    snprintf(big + 500, sizeof big - 500, "%s", " password=SECRETTAIL");
    (void)tbox_log_redact(out, sizeof out, big);
    assert(strstr(out, "SECRET") == NULL); /* the value never survives */
    assert(strlen(out) == TBOX_LOG_MAX_TEXT - 1);
}

static void test_redact_bounds(void) {
    /* an é is 2 bytes (0xC3 0xA9); the cut must never split one */
    char in[700];
    fill_repeat(in, sizeof in, 299); /* 299 pairs = 598 bytes + NUL */
    char out[64]; /* cap 63 bytes: 31 pairs fit, the 32nd would need 64 */
    (void)tbox_log_redact(out, sizeof out, in);
    assert(strlen(out) == 62);
    assert(memcmp(out, in, 62) == 0); /* whole pairs, in order, byte-equal */
    /* plain ASCII truncation goes exactly to the module bound */
    char wide[900];
    memset(wide, 'y', sizeof wide);
    wide[sizeof wide - 1] = '\0';
    char roomy[TBOX_LOG_MAX_TEXT * 2];
    (void)tbox_log_redact(roomy, sizeof roomy, wide);
    assert(strlen(roomy) == TBOX_LOG_MAX_TEXT - 1);
    /* a caller bound below the module bound wins */
    char mid[100];
    (void)tbox_log_redact(mid, sizeof mid, wide);
    assert(strlen(mid) == 99);
}

/* ---- record shape (spec 9.5) ------------------------------------------ */

static void test_record_shape(void) {
    wipe("tbox_log_test/rec");
    TBoxLog *log = NULL;
    assert(tbox_log_open("tbox_log_test/rec", 0, 0, &log) == TBOX_LOG_OK);
    assert(log != NULL);
    assert(tbox_log_write(log, TBOX_LOG_INFO, "xfer", "op-1", "hi", "FLOOD_WAIT_3") == TBOX_LOG_OK);
    assert(tbox_log_write(log, TBOX_LOG_ERROR, "xfer", NULL, "bad", NULL) == TBOX_LOG_OK);
    tbox_log_close(log);

    size_t len = 0;
    char *text = slurp("tbox_log_test/rec/logs/engine.log", &len);
    /* exactly two records, one line each */
    assert(text[len - 1] == '\n');
    assert(count_char(text, '\n') == 2);

    /* parse both lines */
    char *cut = strchr(text, '\n');
    assert(cut != NULL);
    *cut = '\0';
    cJSON *first = cJSON_Parse(text);
    assert(first != NULL);
    cJSON *second = cJSON_Parse(cut + 1);
    assert(second != NULL);

    assert(cJSON_IsObject(first) && cJSON_IsObject(second));
    /* timestamps: UTC ISO8601, exactly 20 chars */
    cJSON *ts = cJSON_GetObjectItem(first, "ts");
    assert(cJSON_IsString(ts) && strlen(ts->valuestring) == 20);
    assert(ts->valuestring[4] == '-' && ts->valuestring[7] == '-');
    assert(ts->valuestring[10] == 'T' && ts->valuestring[19] == 'Z');

    /* the fields the spec names, with the values written */
    assert(strcmp(cJSON_GetObjectItem(first, "lvl")->valuestring, "info") == 0);
    assert(strcmp(cJSON_GetObjectItem(second, "lvl")->valuestring, "error") == 0);
    assert(strcmp(cJSON_GetObjectItem(first, "mod")->valuestring, "xfer") == 0);
    assert(strcmp(cJSON_GetObjectItem(first, "op")->valuestring, "op-1") == 0);
    assert(cJSON_GetObjectItem(second, "op") == NULL); /* omitted when not given */
    assert(strcmp(cJSON_GetObjectItem(first, "msg")->valuestring, "hi") == 0);
    assert(strcmp(cJSON_GetObjectItem(second, "msg")->valuestring, "bad") == 0);
    assert(strcmp(cJSON_GetObjectItem(first, "code")->valuestring, "FLOOD_WAIT_3") == 0);
    assert(cJSON_GetObjectItem(second, "code") == NULL);

    cJSON_Delete(first);
    cJSON_Delete(second);
    free(text);
}

/* The S6 point of the redaction, end to end: whatever a caller hands the
 * writer, the FILE never holds the secret. */
static void test_secret_never_reaches_the_file(void) {
    wipe("tbox_log_test/secret");
    TBoxLog *log = NULL;
    assert(tbox_log_open("tbox_log_test/secret", 0, 0, &log) == TBOX_LOG_OK);
    assert(tbox_log_write(log, TBOX_LOG_INFO, "sect", "sc-1", "tg://login?token=TOKEN123XYZ", NULL) == TBOX_LOG_OK);
    tbox_log_close(log);

    size_t len = 0;
    char *text = slurp("tbox_log_test/secret/logs/engine.log", &len);
    assert(strstr(text, "TOKEN123XYZ") == NULL);
    assert(strstr(text, "<redacted>") != NULL);
    free(text);
}

/* ---- rotation ---------------------------------------------------------- */

/* Writes one uniform record (same shape as the rotation records) and
 * returns its exact on-disk size, so the rotation tests size the limit
 * off reality instead of hand-counting JSON bytes. */
static size_t seed_record(const char *dir) {
    wipe(dir);
    TBoxLog *log = NULL;
    assert(tbox_log_open(dir, 0, 0, &log) == TBOX_LOG_OK);
    assert(tbox_log_write(log, TBOX_LOG_INFO, "rot", "aa", "rec-00", "e") == TBOX_LOG_OK);
    tbox_log_close(log);
    char p[400];
    snprintf(p, sizeof p, "%s/logs/engine.log", dir);
    size_t len = 0;
    char *text = slurp(p, &len);
    free(text);
    return len;
}

static void test_rotation_keeps_the_files_the_spec_names(void) {
    char p[400];
    size_t tlen = 0;
    char *text = NULL;
    TBoxLog *log = NULL;

    /* one uniform record's exact size drives the limit in both phases */
    size_t one_record = seed_record("tbox_log_test/seed");
    assert(one_record > 60); /* sanity: a shaped record is not tiny */
    /* one record fits, two do not, so every file holds exactly one */
    size_t limit = one_record + 16;

    /* three files kept: engine.log + .1 + .2, and nothing beyond .2 */
    wipe("tbox_log_test/rot3");
    assert(tbox_log_open("tbox_log_test/rot3", limit, 3, &log) == TBOX_LOG_OK);
    for (int i = 1; i <= 5; i++) {
        char msg[8];
        snprintf(msg, sizeof msg, "rec-%02d", i);
        assert(tbox_log_write(log, TBOX_LOG_INFO, "rot", "aa", msg, "e") == TBOX_LOG_OK);
    }
    tbox_log_close(log);
    text = slurp("tbox_log_test/rot3/logs/engine.log", &tlen);
    assert(strstr(text, "rec-05") != NULL && strstr(text, "rec-04") == NULL);
    free(text);
    snprintf(p, sizeof p, "tbox_log_test/rot3/logs/engine.log.1");
    text = slurp(p, &tlen);
    assert(strstr(text, "rec-04") != NULL && strstr(text, "rec-03") == NULL);
    free(text);
    snprintf(p, sizeof p, "tbox_log_test/rot3/logs/engine.log.2");
    text = slurp(p, &tlen);
    assert(strstr(text, "rec-03") != NULL && strstr(text, "rec-02") == NULL);
    free(text);
    assert(TBOX_ACCESS("tbox_log_test/rot3/logs/engine.log.3", TBOX_F_OK) != 0);

    /* two files kept: no .2 bucket at all */
    wipe("tbox_log_test/rot2");
    assert(tbox_log_open("tbox_log_test/rot2", limit, 2, &log) == TBOX_LOG_OK);
    for (int i = 1; i <= 4; i++) {
        char msg[8];
        snprintf(msg, sizeof msg, "rec-%02d", i);
        assert(tbox_log_write(log, TBOX_LOG_INFO, "rot", "aa", msg, "e") == TBOX_LOG_OK);
    }
    tbox_log_close(log);
    text = slurp("tbox_log_test/rot2/logs/engine.log", &tlen);
    assert(strstr(text, "rec-04") != NULL);
    free(text);
    snprintf(p, sizeof p, "tbox_log_test/rot2/logs/engine.log.1");
    text = slurp(p, &tlen);
    assert(strstr(text, "rec-03") != NULL);
    free(text);
    assert(TBOX_ACCESS("tbox_log_test/rot2/logs/engine.log.2", TBOX_F_OK) != 0);
}

/* ---- argument and failure paths ---------------------------------------- */

static void test_failures_are_code_not_crash(void) {
    TBoxLog *log = NULL;
    assert(tbox_log_open(NULL, 0, 0, &log) == TBOX_LOG_E_ARGS);
    assert(log == NULL);
    assert(tbox_log_open("", 0, 0, &log) == TBOX_LOG_E_ARGS);
    assert(tbox_log_open("tbox_log_test/failures", 0, 0, NULL) == TBOX_LOG_E_ARGS);
    assert(tbox_log_write(NULL, TBOX_LOG_INFO, "m", NULL, "hi", NULL) == TBOX_LOG_E_ARGS);

    assert(tbox_log_open("tbox_log_test/failures", 0, 0, &log) == TBOX_LOG_OK);
    /* out-of-range arguments come back as codes, not crashes */
    assert(tbox_log_write(log, (TBoxLogLevel)99, "m", NULL, "hi", NULL) == TBOX_LOG_E_ARGS);
    assert(tbox_log_write(log, TBOX_LOG_DEBUG, "", NULL, "hi", NULL) == TBOX_LOG_E_ARGS);
    assert(tbox_log_write(log, TBOX_LOG_DEBUG, "m", NULL, NULL, NULL) == TBOX_LOG_E_ARGS);
    /* a record that passes the argument check still lands */
    assert(tbox_log_write(log, TBOX_LOG_WARN, "m", NULL, "ok", NULL) == TBOX_LOG_OK);
    tbox_log_close(log);
    tbox_log_close(NULL); /* NULL-safe no-op */

    /* a directory name already taken by a FILE fails with E_MKDIR */
    char stuck[400];
    snprintf(stuck, sizeof stuck, "%s/stuckfile", ROOT);
    touch(stuck);
    assert(tbox_log_open("tbox_log_test/stuckfile", 0, 0, &log) == TBOX_LOG_E_MKDIR);
    assert(log == NULL);
}

/* The Windows W-API rule (§16) in action: a profile directory whose name
 * holds non-ASCII characters must behave exactly like an ASCII one, and
 * a reopened engine must append to the very same file. */
static void test_non_ascii_profile_dir(void) {
    static const char *dir = "tbox_log_test/prof\xC3\xA9"; /* "profé" */
    TBoxLog *log = NULL;
    assert(tbox_log_open(dir, 0, 0, &log) == TBOX_LOG_OK);
    assert(tbox_log_write(log, TBOX_LOG_INFO, "uni", NULL, "unicode-dir", NULL) == TBOX_LOG_OK);
    tbox_log_close(log);
    /* reopen: must append to the same file the W-APIs created */
    assert(tbox_log_open(dir, 0, 0, &log) == TBOX_LOG_OK);
    assert(tbox_log_write(log, TBOX_LOG_INFO, "uni", NULL, "unicode-dir2", NULL) == TBOX_LOG_OK);
    tbox_log_close(log);
    char p[400];
    snprintf(p, sizeof p, "%s/logs/engine.log", dir);
    size_t len = 0;
    char *text = slurp_w(p, &len);
    assert(strstr(text, "unicode-dir2") != NULL);
    free(text);
}

int main(void) {
    test_redact_triggers();
    test_redact_bounds();
    test_record_shape();
    test_secret_never_reaches_the_file();
    test_rotation_keeps_the_files_the_spec_names();
    test_failures_are_code_not_crash();
    test_non_ascii_profile_dir();
    printf("test_tbox_log: ok\n");
    return 0;
}

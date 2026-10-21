/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_fsx.c — the filesystem primitives: UTF-8 paths through the OS,
 * recursive mkdir, POSIX-rename semantics, the spec 9.2 durable
 * replace, and spec 9.3 engine.lock exclusion (second engine -> busy,
 * release -> acquirable again).
 *
 * Everything lives under tbox_fsx_test/ next to the test binary, the
 * same isolation style as the log tests. Names stay ASCII: the
 * non-ASCII path guarantee is already exercised end to end by the log
 * suite, which now runs through these same primitives.
 */
#include "util/fsx.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define TBOX_ACCESS _access
#define TBOX_F_OK 0
#else
#include <unistd.h>
#define TBOX_ACCESS access
#define TBOX_F_OK F_OK
#endif

#define DIR "tbox_fsx_test"

static int exists(const char *path) {
    return TBOX_ACCESS(path, TBOX_F_OK) == 0;
}

/* Writes content through the module itself (so the test's own writes go
 * down the UTF-8/W path too). */
static void put(const char *path, const char *content) {
    FILE *f = tbox_fsx_fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(content, 1, strlen(content), f) == strlen(content));
    assert(fclose(f) == 0);
}

static char *slurp(const char *path) {
    FILE *f = tbox_fsx_fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size >= 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    char *buf = malloc((size_t)size + 1);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)size, f) == (size_t)size);
    assert(fclose(f) == 0);
    buf[size] = '\0';
    return buf;
}

/* ---- mkdirs ----------------------------------------------------------- */

static void test_mkdirs_is_recursive_and_tolerant(void) {
    assert(tbox_fsx_mkdirs(DIR "/a/b/c") == TBOX_FSX_OK);
    assert(exists(DIR "/a/b/c"));
    assert(tbox_fsx_mkdirs(DIR "/a/b/c") == TBOX_FSX_OK); /* already there */
    assert(tbox_fsx_mkdirs(NULL) == TBOX_FSX_E_ARGS);
    assert(tbox_fsx_mkdirs("") == TBOX_FSX_E_ARGS);
    /* a FILE where a directory belongs is an error, not a shrug */
    put(DIR "/a/x", "not a directory");
    assert(tbox_fsx_mkdirs(DIR "/a/x/y") == TBOX_FSX_E_IO);
}

static void test_is_dir_branches(void) {
    assert(tbox_fsx_mkdirs(DIR "/d") == TBOX_FSX_OK);
    assert(tbox_fsx_is_dir(DIR "/d") == 1);
    put(DIR "/d/file", "x");
    assert(tbox_fsx_is_dir(DIR "/d/file") == 0); /* a file is not a dir */
    assert(tbox_fsx_is_dir(DIR "/nope") == 0);   /* missing, not an error */
    assert(tbox_fsx_is_dir(NULL) == 0);
    assert(tbox_fsx_is_dir("") == 0);
}

/* ---- fopen / rename / remove ------------------------------------------ */

static void test_fopen_roundtrip(void) {
    put(DIR "/rw.txt", "hello");
    char *s = slurp(DIR "/rw.txt");
    assert(strcmp(s, "hello") == 0);
    free(s);
    assert(tbox_fsx_fopen(DIR "/missing.txt", "rb") == NULL);
    assert(tbox_fsx_fopen(NULL, "rb") == NULL);
    assert(tbox_fsx_fopen(DIR "/rw.txt", "") == NULL);
}

static void test_rename_and_remove(void) {
    put(DIR "/from.txt", "payload");
    assert(tbox_fsx_rename(DIR "/from.txt", DIR "/to.txt") == TBOX_FSX_OK);
    assert(!exists(DIR "/from.txt"));
    assert(exists(DIR "/to.txt"));
    /* POSIX rename() replaces the target; both platforms must agree */
    put(DIR "/to.txt", "old");
    put(DIR "/from2.txt", "new");
    assert(tbox_fsx_rename(DIR "/from2.txt", DIR "/to.txt") == TBOX_FSX_OK);
    char *s = slurp(DIR "/to.txt");
    assert(strcmp(s, "new") == 0);
    free(s);
    assert(tbox_fsx_rename(DIR "/ghost.txt", DIR "/x.txt") == TBOX_FSX_E_IO);
    assert(tbox_fsx_rename(NULL, DIR "/x.txt") == TBOX_FSX_E_ARGS);
    assert(tbox_fsx_remove(DIR "/to.txt") == TBOX_FSX_OK);
    assert(!exists(DIR "/to.txt"));
    assert(tbox_fsx_remove(DIR "/to.txt") == TBOX_FSX_E_IO); /* gone already */
    assert(tbox_fsx_remove(NULL) == TBOX_FSX_E_ARGS);
}

/* ---- spec 9.2: temp + flush + atomic rename --------------------------- */

static void test_replace_overwrites_and_keeps_old_on_failure(void) {
    put(DIR "/dest.txt", "old");
    put(DIR "/temp.txt", "new");
    assert(tbox_fsx_replace(DIR "/temp.txt", DIR "/dest.txt") == TBOX_FSX_OK);
    assert(!exists(DIR "/temp.txt"));
    char *s = slurp(DIR "/dest.txt");
    assert(strcmp(s, "new") == 0);
    free(s);
    /* a missing source fails without touching the destination */
    assert(tbox_fsx_replace(DIR "/ghost.txt", DIR "/dest.txt") == TBOX_FSX_E_IO);
    s = slurp(DIR "/dest.txt");
    assert(strcmp(s, "new") == 0);
    free(s);
    assert(tbox_fsx_replace(NULL, DIR "/dest.txt") == TBOX_FSX_E_ARGS);
}

/* ---- spec 9.3: engine.lock -------------------------------------------- */

static void test_lock_excludes_a_second_engine(void) {
    const char *prof = DIR "/prof1";
    assert(tbox_fsx_mkdirs(prof) == TBOX_FSX_OK);
    TBoxFsxLock *a = NULL;
    assert(tbox_fsx_lock(prof, &a) == TBOX_FSX_OK);
    assert(a != NULL);
    assert(exists(DIR "/prof1/engine.lock")); /* spec 4: engine.lock lives here */
    /* the second engine on the same profile is refused -> PROFILE_BUSY */
    TBoxFsxLock *b = NULL;
    assert(tbox_fsx_lock(prof, &b) == TBOX_FSX_E_BUSY);
    assert(b == NULL);
    tbox_fsx_unlock(a);
    /* release is real: the profile is acquirable again */
    a = NULL;
    assert(tbox_fsx_lock(prof, &a) == TBOX_FSX_OK);
    assert(a != NULL);
    tbox_fsx_unlock(a);
}

static void test_lock_argument_rules(void) {
    TBoxFsxLock *c = NULL;
    assert(tbox_fsx_lock(NULL, &c) == TBOX_FSX_E_ARGS);
    assert(c == NULL);
    assert(tbox_fsx_lock("", &c) == TBOX_FSX_E_ARGS);
    assert(tbox_fsx_lock(DIR "/prof1", NULL) == TBOX_FSX_E_ARGS);
    /* a profile directory that does not exist is IO trouble, not busy */
    assert(tbox_fsx_lock(DIR "/ghost-profile", &c) == TBOX_FSX_E_IO);
    assert(c == NULL);
    tbox_fsx_unlock(NULL); /* no-op */
}

int main(void) {
    test_mkdirs_is_recursive_and_tolerant();
    test_is_dir_branches();
    test_fopen_roundtrip();
    test_rename_and_remove();
    test_replace_overwrites_and_keeps_old_on_failure();
    test_lock_excludes_a_second_engine();
    test_lock_argument_rules();
    printf("test_tbox_fsx: ok\n");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * fsx.c — filesystem primitives (spec 9.2 durability, spec 9.3 lock
 * file). See fsx.h for the contract; notes that only matter here:
 *
 *   - Windows acts on a path through the ANSI code page unless told
 *     otherwise, so every wrapper converts UTF-8 to UTF-16 and calls
 *     the W form (coding rules). Invalid UTF-8 degrades to U+FFFD
 *     instead of failing: a substituted character in a path beats no
 *     operation at all.
 *   - The lock is verbatim spec 9.3 on both platforms. On POSIX the
 *     file itself may outlive a crashed engine, but the flock cannot -
 *     the kernel releases it with the fd - so a stale engine.lock never
 *     blocks the next start, and it is left in place rather than
 *     unlinked (unlinking a file another engine may be about to flock
 *     splits the exclusion).
 *   - tbox_fsx_rename replaces the destination on both platforms, the
 *     POSIX rename() contract; log rotation removes its target first,
 *     so both sides behave the same there too.
 */
#include "util/fsx.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h> /* flock */
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Same 600-byte path rule as the session and log code. */
#define TBOX_FSX_PATH 600
#define TBOX_FSX_WIDE (TBOX_FSX_PATH + 16)

/* ---- UTF-8 to the OS -------------------------------------------------- */

#ifdef _WIN32
static int fsx_wide(const char *utf8, wchar_t *out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap);
    return n > 0 ? 0 : -1;
}

static int fsx_is_dir(const char *utf8) {
    wchar_t w[TBOX_FSX_WIDE];
    DWORD attrs;
    if (fsx_wide(utf8, w, TBOX_FSX_WIDE) != 0) {
        return 0;
    }
    attrs = GetFileAttributesW(w);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

static int fsx_mkdir_one(const char *utf8) {
    wchar_t w[TBOX_FSX_WIDE];
    if (fsx_wide(utf8, w, TBOX_FSX_WIDE) != 0) {
        return -1;
    }
    return CreateDirectoryW(w, NULL) ? 0 : -1;
}
#else
static int fsx_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 0;
}

static int fsx_mkdir_one(const char *path) {
    return mkdir(path, 0755);
}
#endif

FILE *tbox_fsx_fopen(const char *path, const char *mode) {
    if (path == NULL || path[0] == '\0' || mode == NULL || mode[0] == '\0') {
        return NULL;
    }
#ifdef _WIN32
    wchar_t wp[TBOX_FSX_WIDE];
    wchar_t wm[8];
    FILE *f = NULL;
    if (fsx_wide(path, wp, TBOX_FSX_WIDE) != 0 ||
        fsx_wide(mode, wm, 8) != 0 ||
        _wfopen_s(&f, wp, wm) != 0) {
        return NULL;
    }
    return f;
#else
    return fopen(path, mode);
#endif
}

int tbox_fsx_is_dir(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    return fsx_is_dir(path);
}

/* ---- what is on disk --------------------------------------------------- */

TBoxFsxStatus tbox_fsx_file_info(const char *path, TBoxFileInfo *out) {
    if (path == NULL || path[0] == '\0' || out == NULL) {
        return TBOX_FSX_E_ARGS;
    }
    memset(out, 0, sizeof *out);
#ifdef _WIN32
    wchar_t w[TBOX_FSX_WIDE];
    if (fsx_wide(path, w, TBOX_FSX_WIDE) != 0) {
        return TBOX_FSX_E_IO;
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &data)) {
        return TBOX_FSX_OK; /* not there: exists stays 0 */
    }
    out->exists = 1;
    out->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    out->size = (long long)data.nFileSizeLow |
                ((long long)data.nFileSizeHigh << 32);
    /* FILETIME counts 100 ns ticks since 1601; Unix counts seconds since
     * 1970, so the two epochs are 11644473600 seconds apart. */
    ULARGE_INTEGER ticks;
    ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
    out->mtime = (long long)(ticks.QuadPart / 10000000ULL) - 11644473600LL;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return TBOX_FSX_OK; /* not there: exists stays 0 */
    }
    out->exists = 1;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->size = (long long)st.st_size;
    out->mtime = (long long)st.st_mtime;
#endif
    return TBOX_FSX_OK;
}

/* ---- reading a file ---------------------------------------------------- */

struct TBoxFsxFile {
#ifdef _WIN32
    HANDLE handle;
#else
    FILE *fp;
#endif
};

TBoxFsxStatus tbox_fsx_open_read(const char *path, TBoxFsxFile **out) {
    if (path == NULL || path[0] == '\0' || out == NULL) {
        return TBOX_FSX_E_ARGS;
    }
    *out = NULL;
    TBoxFsxFile *f = (TBoxFsxFile *)calloc(1, sizeof *f);
    if (f == NULL) {
        return TBOX_FSX_E_MEM;
    }
#ifdef _WIN32
    wchar_t w[TBOX_FSX_WIDE];
    if (fsx_wide(path, w, TBOX_FSX_WIDE) != 0) {
        free(f);
        return TBOX_FSX_E_IO;
    }
    /* FILE_SHARE_READ lets the file be read while Telegram is writing it;
     * the upload reads a file the user may still be editing. */
    f->handle = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f->handle == INVALID_HANDLE_VALUE) {
        free(f);
        return TBOX_FSX_E_IO;
    }
#else
    f->fp = fopen(path, "rb");
    if (f->fp == NULL) {
        free(f);
        return TBOX_FSX_E_IO;
    }
#endif
    *out = f;
    return TBOX_FSX_OK;
}

TBoxFsxStatus tbox_fsx_read(TBoxFsxFile *f, void *buf, size_t cap,
                             size_t *read) {
    if (f == NULL || buf == NULL || read == NULL) {
        return TBOX_FSX_E_ARGS;
    }
    *read = 0;
    if (cap == 0) {
        return TBOX_FSX_OK;
    }
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadFile(f->handle, buf, (DWORD)cap, &got, NULL)) {
        return TBOX_FSX_E_IO;
    }
    *read = (size_t)got;
#else
    size_t got = fread(buf, 1, cap, f->fp);
    if (got == 0 && ferror(f->fp)) {
        return TBOX_FSX_E_IO;
    }
    *read = got;
#endif
    return TBOX_FSX_OK;
}

void tbox_fsx_close_read(TBoxFsxFile *f) {
    if (f == NULL) {
        return;
    }
#ifdef _WIN32
    if (f->handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(f->handle);
    }
#else
    if (f->fp != NULL) {
        (void)fclose(f->fp);
    }
#endif
    free(f);
}

TBoxFsxStatus tbox_fsx_mkdirs(const char *path) {
    char tmp[TBOX_FSX_PATH];
    if (path == NULL || path[0] == '\0') {
        return TBOX_FSX_E_ARGS;
    }
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return TBOX_FSX_E_ARGS;
    }
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            (void)fsx_mkdir_one(tmp); /* intermediate components: best effort */
            *p = '/';
        }
    }
    if (fsx_mkdir_one(tmp) != 0 && !fsx_is_dir(tmp)) {
        return TBOX_FSX_E_IO;
    }
    return TBOX_FSX_OK;
}

TBoxFsxStatus tbox_fsx_remove(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return TBOX_FSX_E_ARGS;
    }
#ifdef _WIN32
    wchar_t w[TBOX_FSX_WIDE];
    if (fsx_wide(path, w, TBOX_FSX_WIDE) != 0) {
        return TBOX_FSX_E_IO;
    }
    return DeleteFileW(w) ? TBOX_FSX_OK : TBOX_FSX_E_IO;
#else
    return remove(path) == 0 ? TBOX_FSX_OK : TBOX_FSX_E_IO;
#endif
}

TBoxFsxStatus tbox_fsx_rename(const char *from, const char *to) {
    if (from == NULL || from[0] == '\0' || to == NULL || to[0] == '\0') {
        return TBOX_FSX_E_ARGS;
    }
#ifdef _WIN32
    wchar_t wf[TBOX_FSX_WIDE];
    wchar_t wt[TBOX_FSX_WIDE];
    if (fsx_wide(from, wf, TBOX_FSX_WIDE) != 0 ||
        fsx_wide(to, wt, TBOX_FSX_WIDE) != 0) {
        return TBOX_FSX_E_IO;
    }
    /* MOVEFILE_REPLACE_EXISTING gives POSIX rename() semantics; without
     * it Windows fails on an existing target and the two platforms
     * diverge. MSVC declares no _wrename, so kernel32 is the W path. */
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) ? TBOX_FSX_OK
                                                          : TBOX_FSX_E_IO;
#else
    return rename(from, to) == 0 ? TBOX_FSX_OK : TBOX_FSX_E_IO;
#endif
}

TBoxFsxStatus tbox_fsx_replace(const char *from, const char *to) {
    if (from == NULL || from[0] == '\0' || to == NULL || to[0] == '\0') {
        return TBOX_FSX_E_ARGS;
    }
#ifdef _WIN32
    wchar_t wf[TBOX_FSX_WIDE];
    wchar_t wt[TBOX_FSX_WIDE];
    if (fsx_wide(from, wf, TBOX_FSX_WIDE) != 0 ||
        fsx_wide(to, wt, TBOX_FSX_WIDE) != 0) {
        return TBOX_FSX_E_IO;
    }
    /* WRITE_THROUGH pushes the flush through to disk (spec 9.2). */
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? TBOX_FSX_OK
               : TBOX_FSX_E_IO;
#else
    int fd = open(from, O_RDONLY);
    if (fd < 0) {
        return TBOX_FSX_E_IO; /* cannot flush -> must not rename (spec 9.2) */
    }
    int r = fsync(fd);
    (void)close(fd);
    if (r != 0) {
        return TBOX_FSX_E_IO;
    }
    return rename(from, to) == 0 ? TBOX_FSX_OK : TBOX_FSX_E_IO;
#endif
}

/* ---- copying ----------------------------------------------------------- */

TBoxFsxStatus tbox_fsx_copy(const char *from, const char *to) {
    if (from == NULL || from[0] == '\0' || to == NULL || to[0] == '\0') {
        return TBOX_FSX_E_ARGS;
    }
    TBoxFsxFile *in = NULL;
    if (tbox_fsx_open_read(from, &in) != TBOX_FSX_OK) {
        return TBOX_FSX_E_IO;
    }
    FILE *out = tbox_fsx_fopen(to, "wb");
    if (out == NULL) {
        tbox_fsx_close_read(in);
        return TBOX_FSX_E_IO;
    }
    char buf[65536];
    TBoxFsxStatus st = TBOX_FSX_OK;
    for (;;) {
        size_t got = 0;
        if (tbox_fsx_read(in, buf, sizeof buf, &got) != TBOX_FSX_OK) {
            st = TBOX_FSX_E_IO;
            break;
        }
        if (got == 0) {
            break; /* the end of the file */
        }
        if (fwrite(buf, 1, got, out) != got) {
            st = TBOX_FSX_E_IO;
            break;
        }
    }
    if (fflush(out) != 0) {
        st = TBOX_FSX_E_IO;
    }
    (void)fclose(out);
    tbox_fsx_close_read(in);
    if (st != TBOX_FSX_OK) {
        /* a half-written copy is worse than none: it goes away */
        (void)tbox_fsx_remove(to);
    }
    return st;
}

TBoxFsxStatus tbox_fsx_rename_no_overwrite(const char *from, const char *to) {
    if (from == NULL || from[0] == '\0' || to == NULL || to[0] == '\0') {
        return TBOX_FSX_E_ARGS;
    }
#ifdef _WIN32
    wchar_t wf[TBOX_FSX_WIDE];
    wchar_t wt[TBOX_FSX_WIDE];
    if (fsx_wide(from, wf, TBOX_FSX_WIDE) != 0 ||
        fsx_wide(to, wt, TBOX_FSX_WIDE) != 0) {
        return TBOX_FSX_E_IO;
    }
    /* Without MOVEFILE_REPLACE_EXISTING Windows refuses an existing
     * destination, which is exactly what a download needs. */
    return MoveFileExW(wf, wt, MOVEFILE_WRITE_THROUGH) ? TBOX_FSX_OK
                                                       : TBOX_FSX_E_IO;
#else
    /* POSIX rename() replaces the destination, so the name is taken with
     * link() first: that fails when the destination is there. The bytes are
     * flushed before, like spec 9.2 wants. */
    int fd = open(from, O_RDONLY);
    if (fd < 0) {
        return TBOX_FSX_E_IO;
    }
    int r = fsync(fd);
    (void)close(fd);
    if (r != 0) {
        return TBOX_FSX_E_IO;
    }
    if (link(from, to) != 0) {
        return TBOX_FSX_E_IO;
    }
    if (unlink(from) != 0) {
        (void)unlink(to);
        return TBOX_FSX_E_IO;
    }
    return TBOX_FSX_OK;
#endif
}

/* ---- the single-instance lock (spec 9.3) ------------------------------ */

struct TBoxFsxLock {
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

static TBoxFsxStatus fsx_lock_path(const char *dir, char *dst, size_t cap) {
    if (dir == NULL || dir[0] == '\0') {
        return TBOX_FSX_E_ARGS;
    }
    int n = snprintf(dst, cap, "%s/engine.lock", dir);
    if (n < 0 || (size_t)n >= cap) {
        return TBOX_FSX_E_ARGS;
    }
    return TBOX_FSX_OK;
}

TBoxFsxStatus tbox_fsx_lock(const char *dir, TBoxFsxLock **out) {
    if (out == NULL) {
        return TBOX_FSX_E_ARGS;
    }
    *out = NULL;
    char path[TBOX_FSX_WIDE];
    TBoxFsxStatus st = fsx_lock_path(dir, path, sizeof path);
    if (st != TBOX_FSX_OK) {
        return st;
    }
#ifdef _WIN32
    wchar_t w[TBOX_FSX_WIDE];
    if (fsx_wide(path, w, TBOX_FSX_WIDE) != 0) {
        return TBOX_FSX_E_IO;
    }
    /* Verbatim spec 9.3: no sharing, OPEN_ALWAYS, DELETE_ON_CLOSE. A
     * second engine's CreateFileW collides with the first handle ->
     * sharing violation; anything else (missing directory, ACLs) is
     * honest IO trouble, not a busy profile. */
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION ||
                err == ERROR_ACCESS_DENIED)
                   ? TBOX_FSX_E_BUSY
                   : TBOX_FSX_E_IO;
    }
    TBoxFsxLock *lock = (TBoxFsxLock *)calloc(1, sizeof *lock);
    if (lock == NULL) {
        (void)CloseHandle(h);
        return TBOX_FSX_E_MEM;
    }
    lock->handle = h;
    *out = lock;
    return TBOX_FSX_OK;
#else
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return TBOX_FSX_E_IO;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        (void)close(fd);
        return (e == EWOULDBLOCK || e == EAGAIN) ? TBOX_FSX_E_BUSY
                                                 : TBOX_FSX_E_IO;
    }
    TBoxFsxLock *lock = (TBoxFsxLock *)calloc(1, sizeof *lock);
    if (lock == NULL) {
        (void)flock(fd, LOCK_UN);
        (void)close(fd);
        return TBOX_FSX_E_MEM;
    }
    lock->fd = fd;
    *out = lock;
    return TBOX_FSX_OK;
#endif
}

void tbox_fsx_unlock(TBoxFsxLock *lock) {
    if (lock == NULL) {
        return;
    }
#ifdef _WIN32
    (void)CloseHandle(lock->handle); /* DELETE_ON_CLOSE removes the file */
#else
    (void)flock(lock->fd, LOCK_UN);
    (void)close(lock->fd); /* the file stays; the lock is gone */
#endif
    free(lock);
}

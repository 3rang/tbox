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

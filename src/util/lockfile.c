/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * util/lockfile.c - the single-owner lock for one Telegram session.
 * One header, two native backends (#ifdef _WIN32), no TDLib involved.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: snprintf in fixed-size buffers */

#ifndef _WIN32
/* flock() (per open-file-description, so a second instance conflicts even
 * inside one process) and kill() under a strict -std=c17 compile: ask glibc
 * for the POSIX + BSD-visible set explicitly. Must precede every include;
 * macOS declares both unconditionally and needs no toggle. */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "lockfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/datadir.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/file.h>
#include <unistd.h>
#endif

int tbox_lockfile_path(char *out, size_t size, const char *root)
{
    return tbox_datadir_join(out, size, root, "session.lock");
}

long tbox_pid_self(void)
{
#ifdef _WIN32
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

int tbox_pid_alive(long pid)
{
    if (pid <= 0)
        return -1;

#ifdef _WIN32
    {
        HANDLE proc;
        DWORD code = 0;
        BOOL got;

        proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                          (DWORD)pid);
        if (proc == NULL) {
            /* access denied still means the process exists */
            return GetLastError() == ERROR_ACCESS_DENIED ? 1 : 0;
        }
        got = GetExitCodeProcess(proc, &code);
        CloseHandle(proc);

        if (!got)
            return 1;              /* exists, but we may not query it */
        return code == STILL_ACTIVE ? 1 : 0;
    }
#else
    if (kill((pid_t)pid, 0) == 0)
        return 1;
    /* EPERM means it exists but belongs to another user */
    return errno == EPERM ? 1 : 0;
#endif
}

/* Parse the first decimal number in `buf`; returns 0 on success. */
static int parse_pid(const char *buf, long *pid)
{
    char *end;
    long value;

    value = strtol(buf, &end, 10);
    if (end == buf || value <= 0)
        return -1;

    *pid = value;
    return 0;
}

#ifdef _WIN32

int tbox_lockfile_acquire(tbox_lockfile_t *lock, const char *path)
{
    HANDLE handle;
    char buf[32];
    DWORD written = 0;
    int len;

    if (lock == NULL || path == NULL)
        return -2;

    lock->handle = NULL;
    lock->pid = 0;
    lock->held = 0;

    /* FILE_SHARE_READ: a second *writer* open (any serve instance) is
     * refused with ERROR_SHARING_VIOLATION, while a reader may still peek
     * at the owner pid for the "already running (pid N)" message. */
    handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION)
            return -1;
        return -2;
    }

    len = snprintf(buf, sizeof buf, "%ld\n", tbox_pid_self());

    /* Overwrite our pid from offset 0. SetFilePointer *returns* the new
     * offset, so a 0 result is success at the start - SetFilePointerEx is
     * used instead of trusting that return value. */
    {
        LARGE_INTEGER zero;
        LARGE_INTEGER pos;

        zero.QuadPart = 0;
        if (!SetFilePointerEx(handle, zero, &pos, FILE_BEGIN)
            || !WriteFile(handle, buf, (DWORD)len, &written, NULL)
            || written != (DWORD)len
            || !FlushFileBuffers(handle)) {
            CloseHandle(handle);
            return -2;
        }
    }

    lock->handle = handle;
    lock->pid = tbox_pid_self();
    lock->held = 1;

    return 0;
}

void tbox_lockfile_release(tbox_lockfile_t *lock)
{
    if (lock == NULL || !lock->held)
        return;

    CloseHandle((HANDLE)lock->handle);
    lock->handle = NULL;
    lock->pid = 0;
    lock->held = 0;
}

int tbox_lockfile_read_pid(const char *path, long *pid)
{
    HANDLE handle;
    char buf[32];
    DWORD got = 0;

    if (path == NULL || pid == NULL)
        return -1;

    handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;

    if (!ReadFile(handle, buf, (DWORD)sizeof buf - 1, &got, NULL)) {
        CloseHandle(handle);
        return -1;
    }
    buf[got] = '\0';
    CloseHandle(handle);

    return parse_pid(buf, pid);
}

#else /* POSIX */

int tbox_lockfile_acquire(tbox_lockfile_t *lock, const char *path)
{
    int fd;
    char buf[32];
    int len;
    ssize_t wrote;

    if (lock == NULL || path == NULL)
        return -2;

    lock->handle = NULL;
    lock->pid = 0;
    lock->held = 0;

    fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return -2;

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int err = errno;

        close(fd);
        if (err == EWOULDBLOCK || err == EAGAIN)
            return -1;              /* held by a live process */
        return -2;
    }

    len = snprintf(buf, sizeof buf, "%ld\n", tbox_pid_self());
    wrote = len > 0 ? write(fd, buf, (size_t)len) : -1;
    if (wrote != len) {
        (void)flock(fd, LOCK_UN);
        close(fd);
        return -2;
    }

    lock->handle = (void *)(intptr_t)fd;
    lock->pid = tbox_pid_self();
    lock->held = 1;

    return 0;
}

void tbox_lockfile_release(tbox_lockfile_t *lock)
{
    int fd;

    if (lock == NULL || !lock->held)
        return;

    fd = (int)(intptr_t)lock->handle;
    (void)flock(fd, LOCK_UN);
    close(fd);

    lock->handle = NULL;
    lock->pid = 0;
    lock->held = 0;
}

int tbox_lockfile_read_pid(const char *path, long *pid)
{
    int fd;
    char buf[32];
    ssize_t got;

    if (path == NULL || pid == NULL)
        return -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    got = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (got <= 0)
        return -1;
    buf[got] = '\0';

    return parse_pid(buf, pid);
}

#endif /* _WIN32 */
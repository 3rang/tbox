/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_LOCKFILE_H
#define TBOX_LOCKFILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Single-owner lock for the Telegram session (util layer: no TDLib).
 *
 * Two processes opening one TDLib session make Telegram drop the auth key
 * (v0.3 hit this: AuthKeyUnregisteredError a few minutes after a second
 * `tbox` started). So `tbox serve` takes this lock and nobody else can.
 *
 * The lock is owned by the OS, not by the file contents:
 *   Windows  CreateFileA(..., dwShareMode = FILE_SHARE_READ) - a second
 *            writer open fails with ERROR_SHARING_VIOLATION, and the handle
 *            dies with the process, so there is no stale lock to clean up.
 *   POSIX    flock(fd, LOCK_EX | LOCK_NB) - also released on process exit.
 *
 * Because the lock lives in a live handle, a crash can never wedge the
 * archive: the next start simply succeeds.
 */

/* "<root>/session.lock" into `out`; returns 0 or -1 if it would not fit. */
int tbox_lockfile_path(char *out, size_t size, const char *root);

typedef struct
{
    void *handle;   /* OS handle / file descriptor, NULL when not held */
    long pid;       /* owner pid, 0 when not held */
    int held;

} tbox_lockfile_t;

/*
 * Take the lock at `path` for this process and record our pid inside.
 *
 * Returns:
 *   0  = lock held (release it with tbox_lockfile_release)
 *  -1  = another live process holds it (nothing was opened)
 *  -2  = the lock file could not be created/opened at all
 */
int tbox_lockfile_acquire(tbox_lockfile_t *lock, const char *path);

/* Release the lock; safe to call twice. */
void tbox_lockfile_release(tbox_lockfile_t *lock);

/*
 * Read the owner pid recorded in an existing lock file without taking it.
 * Returns 0 and fills *pid, or -1 when the file is absent or unreadable
 * (readers are allowed on both platforms by design).
 */
int tbox_lockfile_read_pid(const char *path, long *pid);

/* This process's id. */
long tbox_pid_self(void);

/*
 * Is `pid` a live process?
 * Returns 1 = alive, 0 = gone, -1 = pid 0 / not meaningful.
 */
int tbox_pid_alive(long pid);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_LOCKFILE_H */
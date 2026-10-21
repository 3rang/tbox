/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * fsx.h — the engine's filesystem primitives: the one doorway from a
 * UTF-8 path to the OS, the durable replace of spec 9.2, and the
 * single-instance engine.lock of spec 9.3.
 *
 * Everything here speaks UTF-8 paths and (on Windows) converts to
 * UTF-16 for the W functions, so a profile directory under a non-ASCII
 * user name behaves exactly like an ASCII one. Failures are status
 * codes, never fatal; the caller decides what they mean.
 */
#ifndef TBOX_FSX_H
#define TBOX_FSX_H

#include <stdio.h> /* FILE */

typedef enum TBoxFsxStatus {
    TBOX_FSX_OK = 0,
    TBOX_FSX_E_ARGS = -1, /* NULL or empty required argument, path too long */
    TBOX_FSX_E_BUSY = -2, /* engine.lock held by another engine (spec 9.3) */
    TBOX_FSX_E_IO = -3,   /* the OS refused the operation */
    TBOX_FSX_E_MEM = -4   /* out of memory */
} TBoxFsxStatus;

typedef struct TBoxFsxLock TBoxFsxLock;

/* fopen with a UTF-8 path; NULL on any failure (invalid args, path that
 * does not convert, OS refusal). mode is a short standard mode like
 * "rb" / "wb" / "ab". */
FILE *tbox_fsx_fopen(const char *path, const char *mode);

/* Creates every component of path; succeeds if it already exists, fails
 * if a component exists but is not a directory. */
TBoxFsxStatus tbox_fsx_mkdirs(const char *path);

/* True when path names an existing directory. A missing path, a file, or
 * a path that does not convert is simply not a directory (0), never an
 * error - callers use it to branch, not to fail. */
int tbox_fsx_is_dir(const char *path);

TBoxFsxStatus tbox_fsx_remove(const char *path);

/* Moves a name over a path, POSIX rename() semantics: the destination is
 * atomically replaced if it exists. No durability promise - use
 * tbox_fsx_replace where spec 9.2 needs one. */
TBoxFsxStatus tbox_fsx_rename(const char *from, const char *to);

/* Spec 9.2 (temp file + flush + atomic rename): pushes the bytes of
 * `from` to disk, then atomically puts them at `to`. If anything fails
 * mid-way `to` keeps its old content and the status is non-zero. */
TBoxFsxStatus tbox_fsx_replace(const char *from, const char *to);

/* One engine per profile (spec 9.3): acquires <dir>/engine.lock.
 * Another engine holding it -> TBOX_FSX_E_BUSY (the serve command maps
 * that to PROFILE_BUSY). The lock dies with the process - on Windows
 * because the handle is DELETE_ON_CLOSE, on POSIX because the kernel
 * drops flock with the fd. */
TBoxFsxStatus tbox_fsx_lock(const char *dir, TBoxFsxLock **out);
void tbox_fsx_unlock(TBoxFsxLock *lock); /* NULL is a no-op */

#endif /* TBOX_FSX_H */

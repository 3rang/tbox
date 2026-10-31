/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * platform.h - the few things that differ between Windows and POSIX:
 * home directory, mkdir, path of the running executable, loading a shared
 * library, and reading a line / a secret from the terminal.
 */
#ifndef TBOX_PLATFORM_H
#define TBOX_PLATFORM_H

#include <stddef.h>

/* Call once at startup (UTF-8 + ANSI console on Windows). */
void platform_init(void);

/* Monotonic seconds, for deadlines. */
double platform_now(void);

/* User's home directory, or NULL. */
const char *platform_home_dir(void);

/* Create a directory; 0 if created or already present. */
int platform_mkdir(const char *path);

/* 1 if a regular file exists at path. */
int platform_file_exists(const char *path);

/* Absolute form of a path (the file must exist). 0 on success. */
int platform_absolute_path(const char *path, char *buf, size_t n);

/* Directory containing the running executable (no trailing slash). */
int platform_exe_dir(char *buf, size_t n);

/* Shared library loading; err receives the loader's message on failure. */
void *platform_dlopen(const char *path, char *err, size_t errlen);
void *platform_dlsym(void *handle, const char *name);

/* Shared library file name for this OS, e.g. "libtdjson.so". */
const char *platform_tdjson_name(void);

/* Prompt and read one line from stdin, newline stripped.
 * Returns 0, or -1 on EOF. read_secret does not echo. */
int platform_read_line(const char *prompt, char *buf, size_t n);
int platform_read_secret(const char *prompt, char *buf, size_t n);

#endif

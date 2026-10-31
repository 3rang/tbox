/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * platform.h - the few things that differ between Windows and POSIX:
 * home directory, mkdir, absolute paths, a monotonic clock, and reading a
 * secret from the terminal.
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

/* Prompt and read one line from stdin without echo, newline stripped.
 * Returns 0, or -1 on EOF. */
int platform_read_secret(const char *prompt, char *buf, size_t n);

#endif

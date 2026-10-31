/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "core/platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <limits.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#endif

void platform_init(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    /* ANSI colours for the QR code (Windows 10+). */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

double platform_now(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

const char *platform_home_dir(void) {
    const char *h = getenv("HOME");
#ifdef _WIN32
    if (h == NULL || h[0] == '\0') {
        h = getenv("USERPROFILE");
    }
#endif
    return (h != NULL && h[0] != '\0') ? h : NULL;
}

int platform_mkdir(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0) {
        return 0;
    }
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? 0 : -1;
#else
    if (mkdir(path, 0700) == 0) {
        return 0;
    }
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
#endif
}

int platform_file_exists(const char *path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

int platform_absolute_path(const char *path, char *buf, size_t n) {
#ifdef _WIN32
    char full[MAX_PATH];
    if (_fullpath(full, path, sizeof full) == NULL) {
        return -1;
    }
#else
    char full[PATH_MAX];
    if (realpath(path, full) == NULL) {
        return -1;
    }
#endif
    if (strlen(full) >= n) {
        return -1;
    }
    strcpy(buf, full);
    return 0;
}

static int read_line(char *buf, size_t n) {
    if (fgets(buf, (int)n, stdin) == NULL) {
        buf[0] = '\0';
        return -1;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

int platform_read_secret(const char *prompt, char *buf, size_t n) {
    fputs(prompt, stdout);
    fflush(stdout);
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    int tty = GetConsoleMode(in, &mode);
    if (tty) {
        SetConsoleMode(in, mode & ~(DWORD)ENABLE_ECHO_INPUT);
    }
    int rc = read_line(buf, n);
    if (tty) {
        SetConsoleMode(in, mode);
    }
#else
    struct termios old, quiet;
    int tty = tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    int rc = read_line(buf, n);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    }
#endif
    fputc('\n', stdout);
    return rc;
}

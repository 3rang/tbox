/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * term.c - terminal helpers (UTF-8/ANSI mode, masked line input).
 * Ported from the proven flow in demo/qr_login.c, split into reusable
 * primitives so the TDLib layer stays protocol-only.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* termios/isatty visible under -std=c17 */
#endif

#include "term.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <conio.h>
#else
#include <unistd.h>
#include <termios.h>
#endif

int tbox_term_enable_vt(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;

    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return 0;
    if (!GetConsoleMode(h, &mode))
        return 0;
    if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return 0;
    if (SetConsoleOutputCP(65001) == 0)
        return 0;
    return 1;
#else
    return 1;   /* Linux/macOS terminals speak UTF-8 + ANSI natively */
#endif
}

#ifdef _WIN32

/*
 * _getch() reads the raw console. When stdin is redirected the CRT
 * swallows console-key escapes into the byte stream, so refuse that
 * case up front instead of mangling input.
 */
static int console_stdin(void)
{
    return _isatty(_fileno(stdin));
}

int tbox_term_read_masked_line(char *buf, size_t size)
{
    size_t len = 0;
    int c;

    if (buf == NULL || size == 0 || !console_stdin())
        return -1;

    while ((c = _getch()) != 13 && c != 10) {

        if (c == 3 || c == 27) {         /* Ctrl-C / Esc: cancel */
            fputs("\n", stdout);
            fflush(stdout);
            return -1;
        }

        if (c == 224 || c == 0) {        /* extended key prefix: eat code */
            (void)_getch();
            continue;
        }

        if ((c == 8 || c == 127) && len > 0) {
            len--;
            fputs("\b \b", stdout);
            fflush(stdout);
            continue;
        }

        if (c >= 32 && len + 1 < size) {
            buf[len++] = (char)c;
            fputc('*', stdout);
            fflush(stdout);
        }
    }

    buf[len] = '\0';
    printf("\n");

    return (int)len;
}

#else /* POSIX */

int tbox_term_read_masked_line(char *buf, size_t size)
{
    struct termios saved;
    struct termios no_echo;
    int cancelled = 0;
    int echo_off = 0;
    size_t len = 0;
    int c;

    if (buf == NULL || size == 0)
        return -1;

    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        no_echo = saved;
        no_echo.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &no_echo) == 0)
            echo_off = 1;
    }

    while ((c = getchar()) != EOF && c != '\n' && c != '\r') {

        if (c == 3) {                    /* Ctrl-C: cancel */
            cancelled = 1;
            break;
        }
        if ((c == 8 || c == 127) && len > 0) {
            len--;
            fputs("\b \b", stdout);
            fflush(stdout);
            continue;
        }
        if (c >= 32 && len + 1 < size) {
            buf[len++] = (char)c;
            fputc('*', stdout);
            fflush(stdout);
        }
    }

    if (echo_off)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);

    if (cancelled || (c == EOF && len == 0)) {
        buf[0] = '\0';
        fputc('\n', stdout);
        fflush(stdout);
        return -1;
    }

    buf[len] = '\0';
    printf("\n");

    return (int)len;
}

#endif

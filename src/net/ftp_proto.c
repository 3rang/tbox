/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * net/ftp_proto.c - the pure FTP text layer. No sockets, no archive, no
 * TDLib: this is the part that can be tested without a network.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: snprintf in fixed buffers */

#include "ftp_proto.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The synthetic owner/group every listing line claims. */
#define FTP_OWNER "tbox"

/* English month abbreviations: strftime's %b is locale-dependent, and a
 * client that parses listings must not be at the mercy of LC_TIME. */
static const char *const MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Six months, in seconds: older than this and LIST shows the year. */
#define SIX_MONTHS (15552000LL)

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* ---- tokenizer ------------------------------------------------------------ */

int tbox_ftp_parse(const char *line, tbox_ftp_cmd_t *out)
{
    size_t i = 0;
    size_t vlen = 0;

    if (line == NULL || out == NULL)
        return -1;

    memset(out, 0, sizeof *out);

    while (is_space(line[i]))
        i++;

    while (line[i] != '\0' && !is_space(line[i])) {
        if (vlen + 1 < sizeof out->verb)
            out->verb[vlen++] = upper(line[i]);
        i++;
    }
    out->verb[vlen] = '\0';

    if (vlen == 0)
        return -1;

    while (line[i] != '\0') {
        size_t alen = 0;

        while (is_space(line[i]))
            i++;
        if (line[i] == '\0')
            break;

        /* More arguments than we keep: drop the rest rather than the line. */
        if (out->argc >= TBOX_FTP_ARGS)
            break;

        while (line[i] != '\0' && !is_space(line[i])) {
            if (alen + 1 < TBOX_FTP_ARG_MAX)
                out->arg[out->argc][alen++] = line[i];
            i++;
        }
        out->arg[out->argc][alen] = '\0';
        out->argc++;
    }

    return 0;
}

int tbox_ftp_is(const tbox_ftp_cmd_t *cmd, const char *verb)
{
    size_t i;

    if (cmd == NULL || verb == NULL)
        return 0;
    for (i = 0; verb[i] != '\0'; i++) {
        if (cmd->verb[i] != upper(verb[i]))
            return 0;
    }
    return cmd->verb[i] == '\0';
}

/* ---- replies -------------------------------------------------------------- */

int tbox_ftp_reply(char *out, size_t size, int code, const char *text)
{
    int n;

    if (out == NULL || size == 0)
        return -1;
    if (text == NULL)
        text = "";

    n = snprintf(out, size, "%d %s\r\n", code, text);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

int tbox_ftp_multiline(char *out, size_t size, int code, const char *const *lines,
                       int count)
{
    size_t len = 0;
    int i;

    if (out == NULL || size == 0 || (lines == NULL && count > 0))
        return -1;

    /* RFC 959: the first line carries "code-", the rest are bare text, and the
     * reply is closed by "code End". */
    for (i = 0; i < count; i++) {
        char line[TBOX_FTP_LINE_MAX];
        int n;

        if (lines[i] == NULL)
            continue;
        n = snprintf(line, sizeof line, "%s\r\n", lines[i]);
        if (n < 0 || (size_t)n >= sizeof line)
            return -1;
        if (i == 0) {
            char head[32];
            int hn = snprintf(head, sizeof head, "%d-", code);

            if (hn < 0 || (size_t)hn >= sizeof head || len + (size_t)hn >= size)
                return -1;
            memcpy(out + len, head, (size_t)hn);
            len += (size_t)hn;
        }
        if (len + (size_t)n >= size)
            return -1;
        memcpy(out + len, line, (size_t)n);
        len += (size_t)n;
    }

    {
        int n = snprintf(out + len, size - len, "%d End\r\n", code);

        if (n < 0 || (size_t)n >= size - len)
            return -1;
        len += (size_t)n;
    }

    out[len] = '\0';
    return 0;
}

int tbox_ftp_feat_reply(char *out, size_t size)
{
    /* A leading space marks a continuation line as one feature name, and the
     * negative features tell the client not to probe for them. */
    static const char *const feats[] = {
        "Features:",
        " SIZE",
        " MDTM",
        " PASV",
        " TVFS",
        " UTF8",
        " EPSV",
        " REST STREAM",
        " MLST type*;size*;modify*;",
        " MLSD"
    };

    return tbox_ftp_multiline(out, size, 211, feats,
                              (int)(sizeof feats / sizeof feats[0]));
}

/* ---- LIST lines ----------------------------------------------------------- */

/* UTC broken-down time. gmtime() is not thread-safe, and every connection runs
 * on its own thread, so use the re-entrant spelling on both platforms. */
int tbox_ftp_utc(long long when, int *year, int *month, int *day, int *hour,
                 int *minute, int *second)
{
    time_t t = (time_t)when;
    struct tm tm;

#ifdef _WIN32
    if (gmtime_s(&tm, &t) != 0)
        return -1;
#else
    if (gmtime_r(&t, &tm) == NULL)
        return -1;
#endif

    if (year != NULL)
        *year = tm.tm_year + 1900;
    if (month != NULL)
        *month = tm.tm_mon + 1;
    if (day != NULL)
        *day = tm.tm_mday;
    if (hour != NULL)
        *hour = tm.tm_hour;
    if (minute != NULL)
        *minute = tm.tm_min;
    if (second != NULL)
        *second = tm.tm_sec;

    return 0;
}

static int stat_date(char *out, size_t size, long long mtime, long long now)
{
    int year = 0;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int n;

    if (mtime <= 0 || tbox_ftp_utc(mtime, &year, &month, &day, &hour, &minute,
                                   NULL) != 0) {
        n = snprintf(out, size, "%s 01  1970", MONTHS[0]);
        return (n < 0 || (size_t)n >= size) ? -1 : 0;
    }

    if (now > mtime && now - mtime <= SIX_MONTHS)
        n = snprintf(out, size, "%s %2d %02d:%02d", MONTHS[month - 1], day,
                     hour, minute);
    else
        n = snprintf(out, size, "%s %2d  %4d", MONTHS[month - 1], day, year);

    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

int tbox_ftp_stat(char *out, size_t size, int is_dir, long long fsize,
                  long long mtime, const char *name)
{
    char date[32];
    int n;

    if (out == NULL || size == 0 || name == NULL)
        return -1;

    if (stat_date(date, sizeof date, mtime, (long long)time(NULL)) != 0)
        return -1;

    n = snprintf(out, size, "%s %3d %-8s %-8s %11lld %s %s\r\n",
                 is_dir ? "drwxr-xr-x" : "-rw-r--r--", 1, FTP_OWNER, FTP_OWNER,
                 is_dir ? 0LL : fsize, date, name);

    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

int tbox_ftp_list_add(char *buf, size_t cap, size_t *len, int is_dir,
                      long long fsize, long long mtime, const char *name)
{
    char line[TBOX_FTP_LINE_MAX];
    size_t n;

    if (buf == NULL || len == NULL || *len >= cap)
        return -1;
    if (tbox_ftp_stat(line, sizeof line, is_dir, fsize, mtime, name) != 0)
        return -1;

    n = strlen(line);
    if (*len + n >= cap)
        return -1;

    memcpy(buf + *len, line, n);
    *len += n;
    buf[*len] = '\0';
    return 0;
}

/* ---- client paths --------------------------------------------------------- */

static int path_is_abs(const char *arg)
{
    return arg != NULL && arg[0] == '/';
}

/*
 * Normalize into `out`, which must start out empty.
 *
 * The result always starts with '/', so a partially built path is always a
 * valid client path: len == 1 means the root and nothing else. Segments are
 * appended with '/' between them; "." is dropped and ".." pops one segment.
 */
static int path_build(char *out, size_t size, const char *const *segs, int n)
{
    size_t len = 1;
    int i;

    if (size < 2)
        return -1;
    out[0] = '/';
    out[1] = '\0';

    for (i = 0; i < n; i++) {
        const char *seg = segs[i];
        size_t slen;

        if (seg == NULL)
            continue;
        slen = strlen(seg);
        if (slen == 0 || strcmp(seg, ".") == 0)
            continue;

        if (strcmp(seg, "..") == 0) {
            size_t k = len;

            while (k > 1 && out[k - 1] != '/')
                k--;
            /* k is the index of the '/' in front of the segment, so the
             * separator goes too - unless that separator is the root's. */
            len = (k > 2) ? k - 1 : 1;
            out[len] = '\0';
            continue;
        }

        if (len > 1) {
            if (len + 1 >= size)
                return -1;
            out[len++] = '/';
        }
        if (len + slen + 1 > size)
            return -1;
        memcpy(out + len, seg, slen);
        len += slen;
        out[len] = '\0';
    }

    return 0;
}

/* Split a path into segments; "." and empty segments are dropped. */
#define WORK_MAX (TBOX_FTP_ARG_MAX * 2 + 8)

/* Number of segment slots a WORK_MAX-long path could possibly need. */
#define WORK_SLOTS (WORK_MAX / 2 + 1)

/*
 * Split a path in place: every '/' becomes a NUL, and `segs` receives one
 * pointer per segment. Empty segments (from "//" or a trailing '/') are
 * skipped here; path_build drops "." and handles "..".
 * Returns the segment count, or -1 when there are more than `max`.
 */
static int split_path(char *path, const char **segs, int max)
{
    int n = 0;
    char *p = path;

    for (;;) {
        char *start;

        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        start = p;
        while (*p != '\0' && *p != '/')
            p++;
        if (*p == '/')
            *p++ = '\0';

        if (n >= max)
            return -1;
        segs[n++] = start;
    }

    return n;
}

/* Copy a path into a scratch buffer so split_path can patch it in place. */
static int copy_work(char *work, size_t cap, const char *path)
{
    int n = snprintf(work, cap, "%s", path);

    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int tbox_ftp_dir_join(const char *cwd, const char *arg, char *out, size_t size)
{
    const char *segs[WORK_SLOTS];
    char work[WORK_MAX];
    int n;

    if (out == NULL || size == 0 || arg == NULL)
        return -1;
    if (cwd == NULL)
        cwd = "/";

    if (path_is_abs(arg)) {
        if (copy_work(work, sizeof work, arg) != 0)
            return -1;
    } else {
        /* cwd "/" gives "//arg", which normalizes to "/arg" - no special case */
        int wn = snprintf(work, sizeof work, "%s/%s", cwd, arg);

        if (wn < 0 || (size_t)wn >= sizeof work)
            return -1;
    }

    n = split_path(work, segs, WORK_SLOTS);
    if (n < 0)
        return -1;

    return path_build(out, size, segs, n);
}

int tbox_ftp_dir_parent(const char *cwd, char *out, size_t size)
{
    const char *segs[WORK_SLOTS];
    char work[WORK_MAX];
    int n;

    if (out == NULL || size == 0)
        return -1;
    if (cwd == NULL)
        cwd = "/";

    if (copy_work(work, sizeof work, cwd) != 0)
        return -1;

    n = split_path(work, segs, WORK_SLOTS);
    if (n < 0)
        return -1;

    if (n > 0)
        n--;                      /* drop the last segment */

    return path_build(out, size, segs, n);
}

const char *tbox_ftp_dir_base(const char *cwd)
{
    const char *slash;

    if (cwd == NULL)
        return "";
    slash = strrchr(cwd, '/');
    return slash != NULL ? slash + 1 : cwd;
}

int tbox_ftp_pasv_reply(char *out, size_t size, const unsigned char addr[4],
                        int port)
{
    int n;

    if (out == NULL || size == 0 || addr == NULL)
        return -1;
    if (port < 0 || port > 65535)
        return -1;

    n = snprintf(out, size, "227 Entering Passive Mode (%u,%u,%u,%u,%d,%d)\r\n",
                 (unsigned)addr[0], (unsigned)addr[1], (unsigned)addr[2],
                 (unsigned)addr[3], port / 256, port % 256);

    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}
/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_FTP_PROTO_H
#define TBOX_FTP_PROTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ftp_proto - the pure half of the FTP transport: tokenizer, reply builder,
 * CWD/PWD path arithmetic, the PASV address, and the LIST line formatter.
 *
 * Pure by rule (no sockets, no archive, no clock beyond time()): every
 * function here is covered by tests/test_ftp_proto.c, which is how the
 * protocol gets verified without a network.
 *
 * All paths this module produces are *client* paths ("/a/b", always starting
 * with "/"). Turning one into an archive name is core/index's job
 * (tbox_archive_name), so the traversal rule lives in exactly one place.
 */

#define TBOX_FTP_LINE_MAX  1024   /* control-line buffer, incl. CRLF + NUL */
#define TBOX_FTP_ARG_MAX    512   /* one argument, incl. NUL */
#define TBOX_FTP_ARGS         4

typedef struct
{
    char verb[16];                        /* upper-cased, NUL-terminated */
    char arg[TBOX_FTP_ARGS][TBOX_FTP_ARG_MAX];
    int argc;

} tbox_ftp_cmd_t;

/*
 * Split one control line into a verb and up to TBOX_FTP_ARGS arguments.
 *
 * The verb is upper-cased (FTP verbs are case-insensitive); arguments keep
 * their case, since archive names are case-sensitive. Runs of spaces and tabs
 * separate arguments, and trailing whitespace is ignored.
 *
 * Returns 0 on success, -1 for a line with no verb. An argument longer than
 * TBOX_FTP_ARG_MAX is dropped (the server still sees the verb, and answers
 * what it can) - never a buffer overflow.
 */
int tbox_ftp_parse(const char *line, tbox_ftp_cmd_t *out);

/* Case-insensitive verb comparison against a parsed command. */
int tbox_ftp_is(const tbox_ftp_cmd_t *cmd, const char *verb);

/*
 * "220 text\r\n" into `out`. Returns 0, or -1 when it would not fit.
 * Always uses CRLF, which is what the protocol requires.
 */
int tbox_ftp_reply(char *out, size_t size, int code, const char *text);

/*
 * A multi-line reply: every line of `lines` becomes "code-text\r\n", and a
 * final "code End\r\n" closes it, as RFC 959 requires for codes like 211.
 * Returns 0, or -1 when it would not fit.
 */
int tbox_ftp_multiline(char *out, size_t size, int code, const char *const *lines,
                       int count);

/*
 * The FEAT reply: SIZE, MDTM, PASV and UTF8 are advertised, and "EPSV",
 * "REST", "APPE", "MLSD" and "MLST" are explicitly refused. Advertising what
 * we do not support is how clients end up guessing; this way they know.
 */
int tbox_ftp_feat_reply(char *out, size_t size);

/*
 * One Unix-style LIST line: "drwxr-xr-x 1 tbox tbox 0 Oct 04 09:00 name".
 *
 * Permissions and ownership are synthetic (0644/0755, tbox:tbox) - no FTP
 * client acts on them, but believable values keep GUI clients from showing
 * everything as 0600. Timestamps within the last six months are shown as
 * "MMM DD HH:MM", older ones as "MMM DD  YYYY", both in UTC.
 *
 * mtime <= 0 renders as the epoch. Returns 0, or -1 when it would not fit.
 */
int tbox_ftp_stat(char *out, size_t size, int is_dir, long long fsize,
                  long long mtime, const char *name);

/*
 * Append one LIST line to buf[*len] (a growable buffer in the caller).
 * Returns 0, or -1 when the line does not fit in `cap`.
 */
int tbox_ftp_list_add(char *buf, size_t cap, size_t *len, int is_dir,
                      long long fsize, long long mtime, const char *name);

/* ---- client paths ("/a/b") ---------------------------------------------- */

/*
 * Resolve `arg` against the working directory `cwd` and normalize the result.
 * A leading "/" makes `arg` absolute; otherwise it is relative to `cwd`.
 * "." is dropped and ".." pops one segment; ".." at the root stays at the
 * root (the archive is a chroot, so a client cannot climb out of it).
 *
 * Returns 0 with `out` = "/" or "/a/b", or -1 when it would not fit.
 */
int tbox_ftp_dir_join(const char *cwd, const char *arg, char *out, size_t size);

/* The parent of `cwd`: "/a/b" -> "/a", "/" -> "/" (never "/.."). */
int tbox_ftp_dir_parent(const char *cwd, char *out, size_t size);

/* The last segment of `cwd`: "/a/b" -> "b", "/" -> "". Never NULL. */
const char *tbox_ftp_dir_base(const char *cwd);

/*
 * "227 Entering Passive Mode (127,0,0,1,8,69)" from the bound address and
 * port, as PASV requires. Returns 0, or -1 when it would not fit.
 */
int tbox_ftp_pasv_reply(char *out, size_t size, const unsigned char addr[4],
                        int port);

/*
 * Split a Unix timestamp into UTC calendar fields (tm_year + 1900, tm_mon + 1,
 * day, hour, minute, second). MDTM renders 213 YYYYMMDDHHMMSS and LIST needs
 * the same breakdown, so both go through this one function rather than each
 * calling gmtime() (whose static buffer is not thread-safe across sessions).
 *
 * Returns 0, or -1 when the timestamp cannot be converted.
 */
int tbox_ftp_utc(long long when, int *year, int *month, int *day, int *hour,
                 int *minute, int *second);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_FTP_PROTO_H */
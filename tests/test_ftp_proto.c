/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * tests/test_ftp_proto.c - the pure FTP text layer (src/net/ftp_proto.c).
 *
 * Headless by rule: no sockets are opened, no archive is consulted and no
 * TDLib is loaded. Everything the server will later put on the wire is
 * produced here first and checked for shape.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: strcpy/sscanf deprecation */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net/ftp_proto.h"

static int checks;
static int failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        failures++;
        printf("FAIL %s\n", what);
    }
    fflush(stdout);
}

static int eq(const char *got, const char *want, const char *what)
{
    int same = strcmp(got, want) == 0;

    if (!same) {
        checks++;
        failures++;
        printf("FAIL %s (got \"%s\", want \"%s\")\n", what, got, want);
        fflush(stdout);
    } else {
        ok(1, what);
    }
    return same;
}

/* ---- tokenizer ------------------------------------------------------------ */

static void test_parse_basic(void)
{
    tbox_ftp_cmd_t c;

    memset(&c, 0, sizeof c);
    ok(tbox_ftp_parse("USER tbox", &c) == 0, "USER parses");
    ok(strcmp(c.verb, "USER") == 0, "verb is USER");
    ok(c.argc == 1, "USER has one argument");
    eq(c.arg[0], "tbox", "argument is the user name");

    memset(&c, 0, sizeof c);
    ok(tbox_ftp_parse("QUIT", &c) == 0, "a bare verb parses");
    ok(strcmp(c.verb, "QUIT") == 0 && c.argc == 0, "no arguments recorded");

    memset(&c, 0, sizeof c);
    ok(tbox_ftp_parse("SITE CHMOD 755 /a/b.txt", &c) == 0, "three arguments parse");
    ok(c.argc == 3, "all three arguments were kept");
    eq(c.arg[2], "/a/b.txt", "path argument survives");
}

static void test_parse_case_and_space(void)
{
    tbox_ftp_cmd_t c;

    memset(&c, 0, sizeof c);
    ok(tbox_ftp_parse("pass Secret123", &c) == 0, "a lower-case verb parses");
    eq(c.verb, "PASS", "the verb is upper-cased");
    eq(c.arg[0], "Secret123", "argument case is preserved (names are case-sensitive)");

    memset(&c, 0, sizeof c);
    ok(tbox_ftp_parse("  \t RETR   /docs/a b.txt \t ", &c) == 0,
       "leading, trailing and repeated whitespace is tolerated");
    ok(strcmp(c.verb, "RETR") == 0, "verb after whitespace");
    ok(c.argc == 2, "the space inside the name splits it, as the protocol does");
    eq(c.arg[0], "/docs/a", "first token");
    eq(c.arg[1], "b.txt", "second token");
}

static void test_parse_edge(void)
{
    tbox_ftp_cmd_t c;
    char longline[TBOX_FTP_LINE_MAX + 64];
    size_t i;

    ok(tbox_ftp_parse("", &c) == -1, "an empty line is rejected");
    ok(tbox_ftp_parse("   \t ", &c) == -1, "a whitespace-only line is rejected");
    ok(tbox_ftp_parse(NULL, &c) == -1, "a NULL line is rejected");

    memset(&c, 0, sizeof c);
    ok(tbox_ftp_parse("A B C D E F G", &c) == 0, "extra arguments do not fail the parse");
    ok(c.argc == TBOX_FTP_ARGS, "argument count is capped at the maximum");
    ok(c.arg[TBOX_FTP_ARGS - 1][0] != '\0', "the last slot was filled");

    /* an absurdly long verb must truncate, not overflow */
    for (i = 0; i < sizeof longline - 1; i++)
        longline[i] = (i == 60) ? ' ' : 'V';
    longline[sizeof longline - 1] = '\0';
    memset(&c, 0, sizeof c);
    ok(tbox_ftp_parse(longline, &c) == 0, "an over-long verb still parses");
    ok(strlen(c.verb) == sizeof c.verb - 1, "the verb was truncated to the buffer");

    /* an over-long argument must truncate too */
    strcpy(longline, "RETR ");
    for (i = 5; i < sizeof longline - 1; i++)
        longline[i] = 'x';
    longline[sizeof longline - 1] = '\0';
    memset(&c, 0, sizeof c);
    ok(tbox_ftp_parse(longline, &c) == 0, "an over-long argument still parses");
    ok(strlen(c.arg[0]) == TBOX_FTP_ARG_MAX - 1, "the argument was truncated");
    ok(c.argc == 1, "the truncated argument is still one argument");
}

static void test_is(void)
{
    tbox_ftp_cmd_t c;

    tbox_ftp_parse("retr /a", &c);
    ok(tbox_ftp_is(&c, "RETR") == 1, "verb match ignores the client's case");
    ok(tbox_ftp_is(&c, "retr") == 1, "and the caller's case too");
    ok(tbox_ftp_is(&c, "RETRX") == 0, "a longer verb does not match");
    ok(tbox_ftp_is(&c, "RET") == 0, "a shorter verb does not match");
    ok(tbox_ftp_is(&c, "") == 0, "an empty verb never matches");
    ok(tbox_ftp_is(NULL, "RETR") == 0, "NULL command is safe");
    ok(tbox_ftp_is(&c, NULL) == 0, "NULL verb is safe");
}

/* ---- replies -------------------------------------------------------------- */

static void test_reply(void)
{
    char buf[64];

    ok(tbox_ftp_reply(buf, sizeof buf, 220, "tbox FTP ready") == 0, "220 builds");
    eq(buf, "220 tbox FTP ready\r\n", "a single-line reply ends with CRLF");

    ok(tbox_ftp_reply(buf, sizeof buf, 530, NULL) == 0, "a NULL text is allowed");
    eq(buf, "530 \r\n", "a NULL text becomes an empty message");

    ok(tbox_ftp_reply(buf, 8, 220, "a long message") == -1,
       "a reply that does not fit is refused, not truncated");
    ok(tbox_ftp_reply(buf, 0, 220, "x") == -1, "a zero-size buffer is refused");
    ok(tbox_ftp_reply(NULL, 10, 220, "x") == -1, "a NULL buffer is refused");
}

static void test_multiline(void)
{
    static const char *const lines[] = { "Features:", " SIZE", " MDTM" };
    char buf[256];

    ok(tbox_ftp_multiline(buf, sizeof buf, 211, lines, 3) == 0, "multi-line builds");
    eq(buf, "211-Features:\r\n SIZE\r\n MDTM\r\n211 End\r\n",
       "RFC 959 shape: code- first, bare middle, code End last");

    ok(tbox_ftp_multiline(buf, sizeof buf, 200, NULL, 0) == 0,
       "zero lines is legal");
    eq(buf, "200 End\r\n", "an empty multi-line reply still terminates");
}

static void test_feat(void)
{
    char buf[512];

    ok(tbox_ftp_feat_reply(buf, sizeof buf) == 0, "the FEAT reply builds");
    ok(strncmp(buf, "211-Features:\r\n", 15) == 0, "FEAT starts with 211-");
    ok(strstr(buf, " SIZE\r\n") != NULL, "SIZE is advertised");
    ok(strstr(buf, " MDTM\r\n") != NULL, "MDTM is advertised");
    ok(strstr(buf, " PASV\r\n") != NULL, "PASV is advertised");
    ok(strstr(buf, " MLSD\r\n") != NULL, "MLSD is advertised");
    /* features we do not implement are named so clients do not probe them */
    ok(strstr(buf, " EPSV\r\n") != NULL, "EPSV is listed as unsupported");
    ok(strstr(buf, " APPE\r\n") == NULL, "APPE (write) is not advertised");
    ok(strstr(buf, " STOR\r\n") == NULL, "STOR (write) is not advertised");

    {
        size_t len = strlen(buf);

        ok(len > 0 && buf[len - 2] == '\r' && buf[len - 1] == '\n',
           "the FEAT reply ends with CRLF");
    }

    ok(tbox_ftp_feat_reply(buf, 32) == -1, "a short buffer is refused, not truncated");
}

/* ---- LIST lines ----------------------------------------------------------- */

static const char *const MONTH_NAMES[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static int is_month(const char *s)
{
    int i;

    for (i = 0; i < 12; i++) {
        if (strcmp(s, MONTH_NAMES[i]) == 0)
            return 1;
    }
    return 0;
}

/*
 * A LIST line has nine whitespace-separated fields:
 *   perms nlink owner group size <mon> <day> <time|year> name...
 * A name may contain spaces, so only the first eight are matched positionally
 * and the name is whatever is left.
 */
static int stat_fields(const char *line, char *perms, char *nlink, char *owner,
                       char *group, char *size, char *mon, char *day,
                       char *stamp, char *rest)
{
    return sscanf(line, "%15s %7s %15s %15s %31s %3s %2s %7s %255[^\r\n]",
                  perms, nlink, owner, group, size, mon, day, stamp, rest);
}

static void test_stat_shape(void)
{
    char line[256];
    char perms[16], nlink[16], owner[16], group[16], size[32];
    char mon[8], day[8], stamp[16], rest[256];

    ok(tbox_ftp_stat(line, sizeof line, 0, 1234, 1700000000LL, "data.csv") == 0,
       "a file line builds");
    ok(stat_fields(line, perms, nlink, owner, group, size, mon, day, stamp,
                   rest) == 9,
       "the file line has the expected field count");
    eq(perms, "-rw-r--r--", "file permissions are 0644-style");
    eq(nlink, "1", "link count");
    eq(owner, "tbox", "synthetic owner");
    eq(group, "tbox", "synthetic group");
    eq(size, "1234", "the size field holds the real size");
    eq(rest, "data.csv", "the name is last and intact");
    ok(is_month(mon) == 1, "the month is one of the twelve abbreviations");

    ok(tbox_ftp_stat(line, sizeof line, 1, 999, 1700000000LL, "docs") == 0,
       "a directory line builds");
    ok(stat_fields(line, perms, nlink, owner, group, size, mon, day, stamp,
                   rest) == 9,
       "the directory line has the same shape");
    eq(perms, "drwxr-xr-x", "directory permissions are 0755-style");
    eq(size, "0", "a directory reports size 0, whatever the caller passed");
    eq(rest, "docs", "the directory name is last and intact");

    {
        size_t len = strlen(line);

        ok(len > 1 && line[len - 2] == '\r' && line[len - 1] == '\n',
           "every LIST line ends with CRLF");
    }

    ok(tbox_ftp_stat(line, sizeof line, 0, 1, 1, NULL) == -1, "NULL name is refused");
    ok(tbox_ftp_stat(NULL, 100, 0, 1, 1, "x") == -1, "NULL buffer is refused");
    ok(tbox_ftp_stat(line, 24, 0, 1, 1, "a-fairly-long-name.txt") == -1,
       "a line that does not fit is refused, not truncated");
}

static void test_stat_dates(void)
{
    char line[256];
    char stamp[16];
    char mon[8], day[8], rest[256];
    char perms[16], nlink[16], owner[16], group[16], size[32];

    /* mtime <= 0 means "unknown", which must not crash or print garbage */
    ok(tbox_ftp_stat(line, sizeof line, 1, 0, 0, "empty") == 0,
       "an unknown mtime builds");
    ok(strstr(line, "1970") != NULL, "an unknown mtime renders as the epoch");

    ok(tbox_ftp_stat(line, sizeof line, 0, 0, -5, "neg") == 0,
       "a negative mtime builds");
    ok(strstr(line, "1970") != NULL, "a negative mtime also renders as the epoch");

    /* recent: "MMM DD HH:MM" */
    ok(tbox_ftp_stat(line, sizeof line, 0, 0, (long long)time(NULL) - 3600,
                     "recent") == 0, "a recent mtime builds");
    sscanf(line, "%15s %7s %15s %15s %31s %3s %2s %7s", perms, nlink, owner,
           group, size, mon, day, stamp);
    ok(strchr(stamp, ':') != NULL, "a recent timestamp shows a clock time");

    /* older than six months: "MMM DD  YYYY" */
    ok(tbox_ftp_stat(line, sizeof line, 0, 0, 1000000000LL, "ancient") == 0,
       "an old mtime builds");
    sscanf(line, "%15s %7s %15s %15s %31s %3s %2s %7s %255[^\r\n]", perms,
           nlink, owner, group, size, mon, day, stamp, rest);
    eq(stamp, "2001", "an old timestamp shows the year");
    eq(rest, "ancient", "the name survives the year form");
}

static void test_list_add(void)
{
    char buf[256];
    size_t len = 0;

    buf[0] = '\0';
    ok(tbox_ftp_list_add(buf, sizeof buf, &len, 1, 0, 0, "docs") == 0,
       "the first line appends");
    ok(len > 0 && strncmp(buf, "drwxr-xr-x", 10) == 0, "it is the directory line");

    ok(tbox_ftp_list_add(buf, sizeof buf, &len, 0, 7, 0, "a.txt") == 0,
       "the second line appends");
    ok(strstr(buf, "docs") != NULL && strstr(buf, "a.txt") != NULL,
       "both names are in the buffer");
    ok(buf[len - 2] == '\r' && buf[len - 1] == '\n', "the buffer ends with CRLF");

    ok(tbox_ftp_list_add(buf, len + 1, &len, 0, 1, 0, "overflow.txt") == -1,
       "a line that does not fit is refused, not half-written");
    ok(strstr(buf, "overflow.txt") == NULL, "the refused line was not appended");

    ok(tbox_ftp_list_add(NULL, 10, &len, 0, 1, 0, "x") == -1, "NULL buffer is safe");
    ok(tbox_ftp_list_add(buf, sizeof buf, NULL, 0, 1, 0, "x") == -1,
       "NULL length is safe");
    len = sizeof buf + 5;
    ok(tbox_ftp_list_add(buf, sizeof buf, &len, 0, 1, 0, "x") == -1,
       "a length past the capacity is refused");
}

/* ---- client paths --------------------------------------------------------- */

static void test_dir_join(void)
{
    char out[600];

    ok(tbox_ftp_dir_join("/", "/a/b", out, sizeof out) == 0, "absolute join builds");
    eq(out, "/a/b", "an absolute argument ignores the working directory");

    ok(tbox_ftp_dir_join("/a", "b", out, sizeof out) == 0, "relative join builds");
    eq(out, "/a/b", "a relative argument resolves against the working directory");

    ok(tbox_ftp_dir_join("/", "a", out, sizeof out) == 0, "join at the root builds");
    eq(out, "/a", "joining at the root does not double the separator");

    ok(tbox_ftp_dir_join("/a/b/", "c", out, sizeof out) == 0, "trailing slash input");
    eq(out, "/a/b/c", "a trailing slash does not create an empty segment");

    ok(tbox_ftp_dir_join("/a", "/", out, sizeof out) == 0, "absolute root builds");
    eq(out, "/", "the root stays \"/\"");

    ok(tbox_ftp_dir_join("/a/b", ".", out, sizeof out) == 0, "\".\" builds");
    eq(out, "/a/b", "\".\" is dropped");

    ok(tbox_ftp_dir_join("/a", "./b", out, sizeof out) == 0, "interior \".\" builds");
    eq(out, "/a/b", "an interior \".\" is dropped");

    ok(tbox_ftp_dir_join("/a/b", "..", out, sizeof out) == 0, "parent builds");
    eq(out, "/a", "\"..\" pops one segment");

    ok(tbox_ftp_dir_join("/a/b", "../..", out, sizeof out) == 0, "two pops build");
    eq(out, "/", "popping twice from two deep reaches the root");

    ok(tbox_ftp_dir_join("/", "..", out, sizeof out) == 0, "\"..\" at the root builds");
    eq(out, "/", "\"..\" cannot climb above the root");

    ok(tbox_ftp_dir_join("/", "../..", out, sizeof out) == 0, "repeated \"..\" builds");
    eq(out, "/", "repeated \"..\" at the root stays at the root");

    ok(tbox_ftp_dir_join("/a/b/c", "../../d", out, sizeof out) == 0, "mixed pops build");
    eq(out, "/a/d", "a pop followed by a name resolves correctly");

    ok(tbox_ftp_dir_join("/a", "b//c", out, sizeof out) == 0, "double slashes build");
    eq(out, "/a/b/c", "empty segments are dropped");

    ok(tbox_ftp_dir_join("/a", "//b", out, sizeof out) == 0,
       "an absolute argument with a leading // builds");
    eq(out, "/b", "a leading '/' wins over the working directory");

    ok(tbox_ftp_dir_join("/a/b", "/c/../d", out, sizeof out) == 0,
       "an absolute argument is normalized too");
    eq(out, "/d", "normalization applies to absolute arguments");

    ok(tbox_ftp_dir_join(NULL, "x", out, sizeof out) == 0, "a NULL cwd builds");
    eq(out, "/x", "a NULL cwd is treated as the root");

    ok(tbox_ftp_dir_join("/a", NULL, out, sizeof out) == -1, "a NULL arg is refused");
    ok(tbox_ftp_dir_join("/a", "b", NULL, 10) == -1, "a NULL out is refused");
    ok(tbox_ftp_dir_join("/a", "b", out, 0) == -1, "a zero-size out is refused");

    ok(tbox_ftp_dir_join("/a", "b", out, 4) == -1, "one byte short of room");
    ok(tbox_ftp_dir_join("/a", "b", out, 5) == 0, "exactly enough room works");
    eq(out, "/a/b", "the result is exactly what fits");

    {
        char huge[TBOX_FTP_ARG_MAX * 2];
        size_t i;

        for (i = 0; i < sizeof huge - 1; i++)
            huge[i] = 'x';
        huge[sizeof huge - 1] = '\0';
        ok(tbox_ftp_dir_join("/", huge, out, sizeof out) == -1,
           "an absurdly long path is refused");
    }
}

static void test_dir_parent_and_base(void)
{
    char out[600];

    ok(tbox_ftp_dir_parent("/a/b", out, sizeof out) == 0, "parent builds");
    eq(out, "/a", "parent of a two-segment path");

    ok(tbox_ftp_dir_parent("/a", out, sizeof out) == 0, "parent of a leaf builds");
    eq(out, "/", "parent of a top-level path is the root");

    ok(tbox_ftp_dir_parent("/", out, sizeof out) == 0, "parent of the root builds");
    eq(out, "/", "parent of the root is the root, never \"/..\"");

    eq(tbox_ftp_dir_base("/a/b"), "b", "base of a two-segment path");
    eq(tbox_ftp_dir_base("/a"), "a", "base of a top-level path");
    eq(tbox_ftp_dir_base("/"), "", "base of the root is empty");
    eq(tbox_ftp_dir_base(""), "", "base of an empty path is empty");
    eq(tbox_ftp_dir_base(NULL), "", "base of NULL is empty, never NULL");
    eq(tbox_ftp_dir_base("abc"), "abc", "base of a slashless path is itself");
}

/* ---- PASV ----------------------------------------------------------------- */

static void test_pasv(void)
{
    const unsigned char local[4] = { 127, 0, 0, 1 };
    const unsigned char other[4] = { 192, 168, 1, 20 };
    char buf[128];

    ok(tbox_ftp_pasv_reply(buf, sizeof buf, local, 2121) == 0, "PASV builds");
    eq(buf, "227 Entering Passive Mode (127,0,0,1,8,73)\r\n",
       "the port is split as high/low bytes (2121 = 8*256 + 73)");

    ok(tbox_ftp_pasv_reply(buf, sizeof buf, other, 65535) == 0, "the top port builds");
    ok(strstr(buf, "(192,168,1,20,255,255)") != NULL, "the address is dotted decimal");

    ok(tbox_ftp_pasv_reply(buf, sizeof buf, local, 0) == 0, "port 0 builds");
    ok(strstr(buf, ",0,0)") != NULL, "port 0 renders as two zero bytes");

    ok(tbox_ftp_pasv_reply(buf, sizeof buf, local, 65536) == -1, "a port > 65535 is refused");
    ok(tbox_ftp_pasv_reply(buf, sizeof buf, local, -1) == -1, "a negative port is refused");
    ok(tbox_ftp_pasv_reply(buf, sizeof buf, NULL, 21) == -1, "a NULL address is refused");
    ok(tbox_ftp_pasv_reply(NULL, 64, local, 21) == -1, "a NULL buffer is refused");
    ok(tbox_ftp_pasv_reply(buf, 24, local, 21) == -1,
       "a short buffer is refused, not truncated");
}

int main(void)
{
    printf("== test_ftp_proto ==\n");

    test_parse_basic();
    test_parse_case_and_space();
    test_parse_edge();
    test_is();
    test_reply();
    test_multiline();
    test_feat();
    test_stat_shape();
    test_stat_dates();
    test_list_add();
    test_dir_join();
    test_dir_parent_and_base();
    test_pasv();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

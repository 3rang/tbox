/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * tests/test_ftpd.c - the FTP transport end to end (src/net/ftpd.c).
 *
 * A real server on a real loopback socket, a real FTP conversation, and the
 * real archive + cache underneath - only Telegram is missing, because the
 * fetcher is the injected fake from the cache layer.
 *
 * Headless by rule: the port is 0 (the OS picks it), no Telegram, no secrets,
 * and the scratch data root is under TEMP/tmp and removed again at the end.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: the usual POSIX-spelled names */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "core/index.h"
#include "core/read.h"
#include "net/ftpd.h"
#include "util/datadir.h"

/* ---- tiny test harness ---------------------------------------------------- */

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

/* Check that a reply line starts with `code`; print it when it does not. */
static int reply_is(const char *line, int code, const char *what)
{
    char want[8];
    int n = snprintf(want, sizeof want, "%3d", code);

    if (n < 0)
        return 0;

    if (strncmp(line, want, 3) == 0)
        return 1;

    checks++;
    failures++;
    printf("FAIL %s (got \"%s\", wanted %d)\n", what, line, code);
    fflush(stdout);

    return 0;
}

/* ---- a very small FTP client --------------------------------------------- */

#ifdef _WIN32

typedef SOCKET cfd_t;

#define CFG_INVALID  INVALID_SOCKET
#define cfg_close(s) closesocket(s)

#else

typedef int cfd_t;

#define CFG_INVALID  (-1)
#define cfg_close(s) close(s)

#endif

static int cfg_write(cfd_t fd, const char *text)
{
    size_t len = strlen(text);
    size_t off = 0;

    while (off < len) {
        long sent = send(fd, text + off, (int)(len - off), 0);

        if (sent <= 0)
            return -1;
        off += (size_t)sent;
    }

    return 0;
}

/* Read one complete reply (following RFC 959 multi-line rules): every line but
 * the last is "NNN-text", and the last is "NNN text". */
static int cli_reply(cfd_t fd, char *buf, size_t size)
{
    size_t len = 0;
    size_t line_start = 0;

    for (;;) {
        char c;

        if (recv(fd, &c, 1, 0) <= 0)
            return -1;
        if (c == '\n') {
            buf[len] = '\0';
            if (len - line_start >= 4 && buf[line_start + 3] == ' ')
                return 0;
            line_start = len;
            continue;
        }
        if (c == '\r')
            continue;
        if (len + 1 < size)
            buf[len++] = c;
    }
}

static cfd_t cli_connect(int port)
{
    struct sockaddr_in addr;
    cfd_t fd;

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == CFG_INVALID)
        return CFG_INVALID;

    if (connect(fd, (struct sockaddr *)&addr, (int)sizeof addr) != 0) {
        cfg_close(fd);
        return CFG_INVALID;
    }

    return fd;
}

/* Send a command and read its reply into `buf`, returning the reply code. */
static int cli_cmd(cfd_t fd, const char *cmd, char *buf, size_t size)
{
    if (cfg_write(fd, cmd) != 0 || cfg_write(fd, "\r\n") != 0)
        return -1;
    if (buf == NULL || size == 0)
        return -1;
    if (cli_reply(fd, buf, size) != 0)
        return -1;

    return atoi(buf);
}

/* Send a command, require a reply code, and name the reply when it differs. */
static int cmd_ok(cfd_t fd, const char *cmd, int code, const char *what)
{
    char line[2048];
    int got = cli_cmd(fd, cmd, line, sizeof line);

    if (got == code) {
        ok(1, what);
        return 1;
    }

    checks++;
    failures++;
    printf("FAIL %s (got %d \"%s\", wanted %d)\n", what, got, line, code);
    fflush(stdout);

    return 0;
}

/* Log in: greeting, USER, PASS. Returns 0 on success. */
static int cli_login(cfd_t fd, const char *token)
{
    char line[512];
    char cmd[256];

    if (cli_reply(fd, line, sizeof line) < 0)
        return -1;
    if (!reply_is(line, 220, "server greets with 220"))
        return -1;
    if (cli_cmd(fd, "USER tbox", line, sizeof line) < 0)
        return -1;
    if (!reply_is(line, 331, "USER tbox asks for a password"))
        return -1;

    snprintf(cmd, sizeof cmd, "PASS %s", token);

    return cli_cmd(fd, cmd, line, sizeof line);
}

/*
 * PASV: reply, parse the port, connect the data socket.
 * Returns the data socket, or CFG_INVALID; `data_port` gets the port.
 */
static cfd_t cli_pasv(cfd_t fd, char *buf, size_t size, int *data_port)
{
    int a = 0, b = 0, c = 0, d = 0, hi = 0, lo = 0;

    if (buf == NULL || size == 0)
        return CFG_INVALID;
    if (cli_cmd(fd, "PASV", buf, size) != 227
        || !reply_is(buf, 227, "PASV answers 227"))
        return CFG_INVALID;

    if (sscanf(buf, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)", &a, &b,
               &c, &d, &hi, &lo) != 6)
        return CFG_INVALID;

    *data_port = hi * 256 + lo;

    /* loopback only: the server must never hand out a routable address */
    if (a != 127) {
        checks++;
        failures++;
        printf("FAIL the data address is loopback (got %d.%d.%d.%d)\n", a, b, c, d);
        fflush(stdout);
        return CFG_INVALID;
    }

    return cli_connect(*data_port);
}

/* Read a data connection to EOF into buf. Returns the byte count, or -1. */
static int cli_data(cfd_t data, char *buf, size_t size)
{
    size_t len = 0;

    for (;;) {
        long n;

        if (len + 1 >= size)
            return -1;
        n = recv(data, buf + len, (int)(size - len - 1), 0);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        len += (size_t)n;
    }

    buf[len] = '\0';

    return (int)len;
}

/* ---- the fixture ---------------------------------------------------------- */

static char g_root[512];
static char g_cache[640];
static tbox_archive_t g_archive;
static tbox_cache_t g_cache_state;
static tbox_ftpd_t *g_server;
static char g_token[TBOX_FTPD_TOKEN_LEN];

static const char BODY_A[] = "hello!";
static const char BODY_B[] = "binary";
static const char BODY_C[] = "";          /* a genuinely empty file */

/* The cache's injected fetcher: the only stand-in for Telegram here. */
static int fake_fetch(void *user, long long file_id, const char *dest)
{
    const char *body;
    FILE *f;

    (void)user;

    switch (file_id) {
    case 11:  body = BODY_A; break;
    case 22:  body = BODY_B; break;
    case 33:  body = BODY_C; break;
    default:  return -1;
    }

    f = fopen(dest, "wb");
    if (f == NULL)
        return -1;
    if (fwrite(body, 1, strlen(body), f) != strlen(body)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    return 0;
}

static void add_entry(const char *name, int is_dir, long long file_id,
                      long long size, long long mtime)
{
    tbox_archive_entry_t e;

    memset(&e, 0, sizeof e);
    strcpy(e.name, name);
    e.is_dir = is_dir;
    e.file_id = file_id;
    e.message_id = 1000 + file_id;
    e.size = size;
    e.mtime = mtime;
    e.is_tbox = (!is_dir && mtime > 0) ? 1 : 0;
    if (!is_dir)
        strcpy(e.sha256, "0123456789abcdef0123456789abcdef"
                         "0123456789abcdef0123456789abcdef");

    ok(tbox_archive_add(&g_archive, &e) == 0, "fixture: added entry");
}

/* ---- setup / teardown ----------------------------------------------------- */

static int file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static void empty_dir(const char *dir)
{
#ifdef _WIN32
    char pattern[700];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (tbox_datadir_join(pattern, sizeof pattern, dir, "*") != 0)
        return;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char victim[800];

        if (tbox_datadir_join(victim, sizeof victim, dir, fd.cFileName) == 0
            && strcmp(fd.cFileName, ".") != 0
            && strcmp(fd.cFileName, "..") != 0)
            remove(victim);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *de;

    if (d == NULL)
        return;
    while ((de = readdir(d)) != NULL) {
        char victim[800];

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (tbox_datadir_join(victim, sizeof victim, dir, de->d_name) == 0)
            remove(victim);
    }
    closedir(d);
#endif
}

static void scratch_drop(void)
{
    if (g_cache[0] != '\0') {
        empty_dir(g_cache);
#ifdef _WIN32
        _rmdir(g_cache);
#else
        rmdir(g_cache);
#endif
    }
    if (g_root[0] != '\0') {
#ifdef _WIN32
        _rmdir(g_root);
#else
        rmdir(g_root);
#endif
    }
}

static void scratch_make(void)
{
    const char *tmp = getenv("TEMP");

    if (tmp == NULL || tmp[0] == '\0')
        tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0')
        tmp = ".";

    if (tbox_datadir_join(g_root, sizeof g_root, tmp, "tbox_test_ftpd") != 0
        || tbox_datadir_cache(g_cache, sizeof g_cache, g_root) != 0) {
        printf("FAIL scratch path too long\n");
        failures++;
        return;
    }

    scratch_drop();
    ok(tbox_datadir_ensure(g_root) == 0, "scratch data root created");
}

static void fixture_build(void)
{
    tbox_archive_init(&g_archive);

    add_entry("docs", 1, 0, 0, 0);
    add_entry("docs/a.txt", 0, 11, (long long)strlen(BODY_A), 1700000000LL);
    add_entry("docs/sub/b.bin", 0, 22, (long long)strlen(BODY_B), 1700000000LL);
    add_entry("readme.md", 0, 33, 0, -1);          /* foreign document */
    add_entry("empty", 1, 0, 0, 0);

    ok(tbox_cache_open(&g_cache_state, g_root, 65536, fake_fetch, NULL) == 0,
       "fixture: cache opened");
}

static void server_start(void)
{
    tbox_ftpd_opts_t opts;

    memset(&opts, 0, sizeof opts);
    opts.bind_addr = "127.0.0.1";
    opts.port = 0;                                 /* ephemeral */
    opts.archive = &g_archive;
    opts.cache = &g_cache_state;

    g_server = tbox_ftpd_start(&opts, g_token, sizeof g_token);
    ok(g_server != NULL, "server starts on an ephemeral loopback port");
    ok(tbox_ftpd_port(g_server) > 0, "the bound port was reported");
    ok(strlen(g_token) == TBOX_FTPD_TOKEN_LEN - 1, "a 32-character token");
    ok(strcmp(tbox_ftpd_token(g_server), g_token) == 0,
       "the reported token matches the one handed out");
}

/* ---- tests ---------------------------------------------------------------- */

static void test_login(void)
{
    cfd_t fd = cli_connect(tbox_ftpd_port(g_server));
    char line[512];

    ok(fd != CFG_INVALID, "client connects");
    if (fd == CFG_INVALID)
        return;

    /* the greeting arrives unprompted, before anything is sent */
    ok(cli_reply(fd, line, sizeof line) == 0, "the greeting arrived");
    reply_is(line, 220, "server greets with 220");

    /* anything that touches the archive is refused before login */
    cmd_ok(fd, "PWD", 530, "PWD before login is 530");
    cmd_ok(fd, "LIST", 530, "LIST before login is 530");
    cmd_ok(fd, "RETR x", 530, "RETR before login is 530");

    cmd_ok(fd, "USER admin", 530, "only USER tbox is accepted");
    cmd_ok(fd, "PASS whatever", 503, "PASS before USER is a bad sequence");

    cmd_ok(fd, "USER tbox", 331, "USER tbox asks for the token");
    cmd_ok(fd, "PASS deadbeef", 530, "a wrong token is rejected");
    cmd_ok(fd, "USER tbox", 331, "USER may be repeated");
    cmd_ok(fd, "PASS 0123", 530, "a short token is rejected");
    cmd_ok(fd, "PWD", 530, "a failed login leaves the session closed");

    cfg_close(fd);

    /* and now the real one */
    fd = cli_connect(tbox_ftpd_port(g_server));
    if (fd == CFG_INVALID) {
        ok(0, "second connection");
        return;
    }
    ok(cli_login(fd, g_token) == 230, "USER tbox + the token logs in");
    cmd_ok(fd, "PWD", 257, "the session works after logging in");
    cmd_ok(fd, "QUIT", 221, "QUIT answers 221");
    cfg_close(fd);
}

static void test_negotiation(void)
{
    cfd_t fd = cli_connect(tbox_ftpd_port(g_server));
    char line[2048];

    if (fd == CFG_INVALID) {
        ok(0, "negotiation connection");
        return;
    }
    if (cli_login(fd, g_token) != 230) {
        ok(0, "negotiation login");
        cfg_close(fd);
        return;
    }

    ok(cli_cmd(fd, "SYST", line, sizeof line) == 215, "SYST answers 215");
    ok(strstr(line, "UNIX") != NULL, "SYST claims a UNIX server");

    ok(cli_cmd(fd, "TYPE I", line, sizeof line) == 200, "TYPE I is accepted");
    ok(cli_cmd(fd, "TYPE A", line, sizeof line) == 200, "TYPE A is accepted");
    ok(cli_cmd(fd, "TYPE X", line, sizeof line) == 504, "TYPE X is refused");
    ok(cli_cmd(fd, "MODE S", line, sizeof line) == 200, "MODE S is accepted");
    ok(cli_cmd(fd, "MODE B", line, sizeof line) == 504, "MODE B is refused");
    ok(cli_cmd(fd, "STRU F", line, sizeof line) == 200, "STRU F is accepted");
    ok(cli_cmd(fd, "NOOP", line, sizeof line) == 200, "NOOP is 200");
    ok(cli_cmd(fd, "OPTS UTF8 ON", line, sizeof line) == 200,
       "OPTS UTF8 ON is a harmless 200");
    ok(cli_cmd(fd, "CLNT WinSCP", line, sizeof line) == 200, "CLNT is 200");
    ok(cli_cmd(fd, "AUTH TLS", line, sizeof line) == 534,
       "AUTH TLS is refused, not silently ignored");
    ok(cli_cmd(fd, "FLOOP", line, sizeof line) == 500, "an unknown verb is 500");
    ok(cli_cmd(fd, "STOR /x", line, sizeof line) == 502, "STOR is refused");
    ok(cli_cmd(fd, "DELE /x", line, sizeof line) == 502, "DELE is refused");
    ok(cli_cmd(fd, "MKD /x", line, sizeof line) == 502, "MKD is refused");
    ok(cli_cmd(fd, "EPSV", line, sizeof line) == 502, "EPSV is refused");

    ok(cli_cmd(fd, "FEAT", line, sizeof line) == 211, "FEAT answers 211");
    ok(strstr(line, "211-Features:") != NULL, "FEAT starts with 211-");
    ok(strstr(line, " SIZE") != NULL, "FEAT advertises SIZE");
    ok(strstr(line, " STOR") == NULL, "FEAT does not advertise STOR");
    ok(strstr(line, "211 End") != NULL, "FEAT ends with 211 End");

    cfg_close(fd);
}

static void test_pwd_cwd(void)
{
    cfd_t fd = cli_connect(tbox_ftpd_port(g_server));
    char line[512];

    if (fd == CFG_INVALID || cli_login(fd, g_token) != 230) {
        ok(0, "pwd/cwd login");
        if (fd != CFG_INVALID)
            cfg_close(fd);
        return;
    }

    ok(cli_cmd(fd, "PWD", line, sizeof line) == 257, "PWD at the root is 257");
    ok(strstr(line, "\"/\"") != NULL, "the root is quoted \"/\"");

    ok(cli_cmd(fd, "CWD /docs", line, sizeof line) == 257, "CWD /docs is 257");
    ok(cli_cmd(fd, "PWD", line, sizeof line) == 257, "PWD after CWD");
    ok(strstr(line, "\"/docs\"") != NULL, "the working directory followed CWD");

    ok(cli_cmd(fd, "CWD sub", line, sizeof line) == 257,
       "a relative CWD resolves against the working directory");
    ok(strstr(line, "\"/docs/sub\"") != NULL, "the relative CWD landed correctly");

    ok(cli_cmd(fd, "CDUP", line, sizeof line) == 257, "CDUP is 257");
    ok(strstr(line, "\"/docs\"") != NULL, "CDUP went up one level");

    ok(cli_cmd(fd, "CWD ..", line, sizeof line) == 257, "CWD .. is 257");
    ok(strstr(line, "\"/\"") != NULL, "\"..\" from the top level is the root");

    ok(cli_cmd(fd, "CWD ../../..", line, sizeof line) == 257,
       "\"..\" cannot climb above the root");
    ok(strstr(line, "\"/\"") != NULL, "the archive root is the floor");

    ok(cli_cmd(fd, "CWD /docs/a.txt", line, sizeof line) == 550,
       "CWD onto a file is 550");
    ok(cli_cmd(fd, "CWD /nope", line, sizeof line) == 550,
       "CWD into a missing directory is 550");
    ok(cli_cmd(fd, "CWD", line, sizeof line) == 501, "CWD with no argument is 501");

    ok(cli_cmd(fd, "QUIT", line, sizeof line) == 221, "done");
    cfg_close(fd);
}

static void test_list(void)
{
    cfd_t fd = cli_connect(tbox_ftpd_port(g_server));
    char line[512];
    char data[8192];
    char names[8192];
    cfd_t d;
    int port = 0;
    int n;

    if (fd == CFG_INVALID || cli_login(fd, g_token) != 230) {
        ok(0, "list login");
        if (fd != CFG_INVALID)
            cfg_close(fd);
        return;
    }

    ok(cli_cmd(fd, "LIST", line, sizeof line) == 425,
       "LIST without PASV is 425");

    /* root listing */
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(d != CFG_INVALID, "PASV yields a connectable loopback port");
    ok(cli_cmd(fd, "LIST", line, sizeof line) == 150, "LIST answers 150");
    n = cli_data(d, data, sizeof data);
    ok(n > 0, "the root listing has content");
    cfg_close(d);
    ok(cli_reply(fd, line, sizeof line) == 0, "the completion reply arrived");
    reply_is(line, 226, "LIST ends with 226");

    ok(strstr(data, "docs") != NULL, "the root listing contains docs");
    ok(strstr(data, "readme.md") != NULL, "the root listing contains readme.md");
    ok(strstr(data, "empty") != NULL, "the root listing contains empty");
    ok(strstr(data, "drwxr-xr-x") != NULL, "directories get the d prefix");
    ok(strstr(data, "-rw-r--r--") != NULL, "files get the - prefix");
    ok(strstr(data, "\r\n") != NULL, "listing lines end with CRLF");

    /* NLST: names only */
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(d != CFG_INVALID, "PASV again for NLST");
    ok(cli_cmd(fd, "NLST", line, sizeof line) == 150, "NLST answers 150");
    n = cli_data(d, names, sizeof names);
    ok(n > 0, "NLST has content");
    cfg_close(d);
    (void)cli_reply(fd, line, sizeof line);
    ok(strchr(names, ' ') == NULL, "NLST lines carry names only");
    ok(strstr(names, "docs\r\n") != NULL, "NLST lists docs");
    ok(strstr(names, "readme.md\r\n") != NULL, "NLST lists readme.md");

    /* a subdirectory */
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(cli_cmd(fd, "LIST /docs", line, sizeof line) == 150, "LIST /docs is 150");
    n = cli_data(d, data, sizeof data);
    cfg_close(d);
    (void)cli_reply(fd, line, sizeof line);
    ok(strstr(data, "a.txt") != NULL, "the docs listing contains a.txt");
    ok(strstr(data, "sub") != NULL, "the docs listing contains sub");
    ok(strstr(data, "docs/") == NULL,
       "listing names are relative, as WinSCP expects");

    /* an empty directory still lists something */
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(cli_cmd(fd, "LIST /empty", line, sizeof line) == 150,
       "LIST on the empty directory is 150");
    n = cli_data(d, data, sizeof data);
    cfg_close(d);
    (void)cli_reply(fd, line, sizeof line);
    ok(strstr(data, ".\r\n") != NULL, "an empty directory lists \".\"");
    ok(strstr(data, "..\r\n") != NULL, "an empty directory lists \"..\"");

    /* a missing path is 550 and opens no data connection */
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(cli_cmd(fd, "LIST /nope", line, sizeof line) == 550,
       "LIST of a missing path is 550");
    cfg_close(d);

    ok(cli_cmd(fd, "QUIT", line, sizeof line) == 221, "done");
    cfg_close(fd);
}

static void test_retr(void)
{
    cfd_t fd = cli_connect(tbox_ftpd_port(g_server));
    char line[512];
    char data[8192];
    char cached[800];
    cfd_t d;
    int port = 0;

    if (fd == CFG_INVALID || cli_login(fd, g_token) != 230) {
        ok(0, "retr login");
        if (fd != CFG_INVALID)
            cfg_close(fd);
        return;
    }

    ok(cli_cmd(fd, "RETR /docs/a.txt", line, sizeof line) == 425,
       "RETR without PASV is 425");

    /* the body has to be fetched from the injected fetcher, then streamed */
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(d != CFG_INVALID, "PASV for RETR");
    ok(cli_cmd(fd, "RETR /docs/a.txt", line, sizeof line) == 150,
       "RETR answers 150");
    ok(cli_data(d, data, sizeof data) == (int)strlen(BODY_A),
       "the transferred length is exactly the body size");
    cfg_close(d);
    {
        char end[512];

        /* the completion reply follows the data connection closing */
        if (cli_reply(fd, end, sizeof end) == 0)
            reply_is(end, 226, "RETR ends with 226");
        else
            ok(0, "RETR completion reply");
    }
    ok(strcmp(data, BODY_A) == 0, "the body arrived intact");

    /* and the transfer left the body in the on-disk cache */
    if (tbox_datadir_join(cached, sizeof cached, g_cache, "11.bin") == 0) {
        ok(file_exists(cached), "the fetched body is cached as <file_id>.bin");
    } else {
        ok(0, "cache path built");
    }

    /* a relative RETR from a working directory */
    ok(cli_cmd(fd, "CWD /docs", line, sizeof line) == 257, "CWD /docs");
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(cli_cmd(fd, "RETR sub/b.bin", line, sizeof line) == 150,
       "a relative RETR works");
    ok(cli_data(d, data, sizeof data) == (int)strlen(BODY_B), "body B length");
    cfg_close(d);
    {
        char end[512];

        (void)cli_reply(fd, end, sizeof end);
    }
    ok(strcmp(data, BODY_B) == 0, "the second body arrived intact");
    ok(cli_cmd(fd, "CUP", line, sizeof line) == 500, "CUP is not a command");

    /* a zero-byte file transfers as zero bytes, not as an error */
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(cli_cmd(fd, "RETR /readme.md", line, sizeof line) == 150,
       "an empty file still answers 150");
    ok(cli_data(d, data, sizeof data) == 0, "an empty file transfers nothing");
    cfg_close(d);
    {
        char end[512];

        ok(cli_reply(fd, end, sizeof end) == 0
               && reply_is(end, 226, "an empty transfer completes"),
           "an empty transfer completes with 226");
    }

    /* error cases */
    d = cli_pasv(fd, line, sizeof line, &port);
    ok(cli_cmd(fd, "RETR /docs", line, sizeof line) == 550,
       "RETR on a directory is 550");
    cfg_close(d);

    d = cli_pasv(fd, line, sizeof line, &port);
    ok(cli_cmd(fd, "RETR /missing.txt", line, sizeof line) == 550,
       "RETR of a missing file is 550");
    cfg_close(d);

    d = cli_pasv(fd, line, sizeof line, &port);
    ok(cli_cmd(fd, "RETR", line, sizeof line) == 501, "RETR with no path is 501");
    cfg_close(d);

    ok(cli_cmd(fd, "QUIT", line, sizeof line) == 221, "done");
    cfg_close(fd);
}

static void test_size_mdtm(void)
{
    cfd_t fd = cli_connect(tbox_ftpd_port(g_server));
    char line[512];

    if (fd == CFG_INVALID || cli_login(fd, g_token) != 230) {
        ok(0, "size/mdtm login");
        if (fd != CFG_INVALID)
            cfg_close(fd);
        return;
    }

    ok(cli_cmd(fd, "SIZE /docs/a.txt", line, sizeof line) == 213, "SIZE is 213");
    ok(strstr(line, "6") != NULL, "SIZE reports the real size");

    ok(cli_cmd(fd, "SIZE /docs", line, sizeof line) == 213,
       "SIZE on a directory is answered, not refused");
    ok(cli_cmd(fd, "SIZE /nope", line, sizeof line) == 550,
       "SIZE of a missing path is 550");
    ok(cli_cmd(fd, "SIZE", line, sizeof line) == 501, "SIZE with no path is 501");

    ok(cli_cmd(fd, "MDTM /docs/a.txt", line, sizeof line) == 213, "MDTM is 213");
    ok(strncmp(line, "213 2023", 8) == 0,
       "MDTM renders YYYYMMDD from the stored mtime");

    ok(cli_cmd(fd, "MDTM /readme.md", line, sizeof line) == 550,
       "MDTM on an entry with no mtime is 550, not an invented date");
    ok(cli_cmd(fd, "MDTM /docs", line, sizeof line) == 550,
       "MDTM on a directory marker is 550 too");

    ok(cli_cmd(fd, "QUIT", line, sizeof line) == 221, "done");
    cfg_close(fd);
}

static void test_two_sessions(void)
{
    cfd_t a = cli_connect(tbox_ftpd_port(g_server));
    cfd_t b = cli_connect(tbox_ftpd_port(g_server));
    char line[512];
    char data[8192];
    char other[8192];
    cfd_t da, db;
    int pa = 0, pb = 0;

    if (a == CFG_INVALID || b == CFG_INVALID) {
        ok(0, "two sessions connect");
        if (a != CFG_INVALID)
            cfg_close(a);
        if (b != CFG_INVALID)
            cfg_close(b);
        return;
    }

    ok(cli_login(a, g_token) == 230, "session A logs in");
    ok(cli_login(b, g_token) == 230, "session B logs in independently");

    /* interleave: each session keeps its own working directory */
    ok(cli_cmd(a, "CWD /docs", line, sizeof line) == 257, "A changes directory");
    ok(cli_cmd(b, "PWD", line, sizeof line) == 257, "B asks for its own PWD");
    ok(strstr(line, "\"/\"") != NULL, "B is still at the root");

    da = cli_pasv(a, line, sizeof line, &pa);
    db = cli_pasv(b, line, sizeof line, &pb);
    ok(pa != pb, "each session got its own data port");

    ok(cli_cmd(a, "LIST", line, sizeof line) == 150, "A lists");
    ok(cli_data(da, data, sizeof data) > 0, "A got data");
    cfg_close(da);

    ok(cli_cmd(b, "LIST", line, sizeof line) == 150, "B lists");
    ok(cli_data(db, other, sizeof other) > 0, "B got data");
    cfg_close(db);

    ok(strstr(data, "a.txt") != NULL, "A sees the docs listing");
    ok(strstr(other, "readme.md") != NULL, "B sees the root listing");
    ok(strstr(other, "a.txt") == NULL, "B's listing is the root, not docs");

    cfg_close(a);
    cfg_close(b);
}

static void test_token_and_binding(void)
{
    char a[TBOX_FTPD_TOKEN_LEN];
    char b[TBOX_FTPD_TOKEN_LEN];
    size_t i;
    int hex_ok = 1;
    int differing = 0;

    ok(tbox_ftpd_token_generate(a, sizeof a) == 0,
       "the token generator reports success");
    ok(tbox_ftpd_token_generate(b, sizeof b) == 0,
       "the token generator reports success again");

    ok(strlen(a) == TBOX_FTPD_TOKEN_LEN - 1, "a generated token is 32 chars");
    for (i = 0; i < TBOX_FTPD_TOKEN_LEN - 1; i++) {
        if (!strchr("0123456789abcdef", a[i]))
            hex_ok = 0;
    }
    ok(hex_ok, "the token is lowercase hex");
    ok(strcmp(a, b) != 0, "two tokens differ");
    if (strcmp(a, b) != 0)
        differing = 1;
    ok(differing, "the token is not a constant");

    /*
     * Regression: the server used to treat a token starting "00" as its
     * all-zero failure sentinel and refuse to start. A real 128-bit token
     * looks like that about once every 256 runs, so `serve` used to fail to
     * start at random. Drawing until one appears is cheap - a few thousand
     * OS calls take microseconds - and it pins the behaviour directly.
     */
    {
        char t[TBOX_FTPD_TOKEN_LEN];
        int seen = 0;
        int draw;

        for (draw = 0; draw < 4000 && !seen; draw++) {
            /* returning 0 is the assertion: a "00..." token must be accepted,
             * not inferred to be a failure. */
            if (tbox_ftpd_token_generate(t, sizeof t) != 0)
                continue;
            if (t[0] == '0' && t[1] == '0')
                seen = 1;
        }
        ok(seen, "a token beginning \"00\" is accepted, not taken for a failure");
        ok(!seen || strlen(t) == TBOX_FTPD_TOKEN_LEN - 1,
           "that token is still full length");
        /* Same length check for a token that does not begin "00", so the
         * terminator is pinned by both paths. */
        ok(tbox_ftpd_token_generate(a, sizeof a) == 0
               && strlen(a) == TBOX_FTPD_TOKEN_LEN - 1,
           "an ordinary token is 32 chars and terminated");
    }

    {
        tbox_ftpd_opts_t opts;
        tbox_ftpd_t *s;

        memset(&opts, 0, sizeof opts);
        opts.bind_addr = "0.0.0.0";
        opts.port = 0;
        opts.archive = &g_archive;
        s = tbox_ftpd_start(&opts, NULL, 0);
        ok(s == NULL, "binding 0.0.0.0 is refused");
        if (s != NULL)
            tbox_ftpd_stop(s);

        opts.bind_addr = "192.168.1.10";
        s = tbox_ftpd_start(&opts, NULL, 0);
        ok(s == NULL, "binding a LAN address is refused");
        if (s != NULL)
            tbox_ftpd_stop(s);

        opts.bind_addr = "127.0.0.1";
        opts.archive = NULL;
        s = tbox_ftpd_start(&opts, NULL, 0);
        ok(s == NULL, "starting without an archive is refused");
        if (s != NULL)
            tbox_ftpd_stop(s);
    }

    /* a NULL handle is a no-op, not a crash */
    tbox_ftpd_stop(NULL);
    ok(1, "stop(NULL) is safe");
}

/* Stop must not hang even with a session parked mid-conversation. */
static void test_stop_with_open_session(void)
{
    int port = tbox_ftpd_port(g_server);
    cfd_t fd = cli_connect(port);
    char line[512];

    if (fd == CFG_INVALID) {
        ok(0, "stop-test connection");
        return;
    }
    ok(cli_login(fd, g_token) == 230, "the parked session logs in");
    ok(cli_cmd(fd, "NOOP", line, sizeof line) == 200, "and is mid-session");

    tbox_ftpd_stop(g_server);
    g_server = NULL;

    ok(1, "stop returned with a session still attached");

    /* the listening socket is gone, so nothing answers on that port any more */
    fd = cli_connect(port);
    ok(fd == CFG_INVALID, "the port no longer accepts connections");
    if (fd != CFG_INVALID)
        cfg_close(fd);
}

int main(void)
{
    printf("== test_ftpd ==\n");

    scratch_make();
    fixture_build();
    server_start();

    if (g_server != NULL) {
        test_login();
        test_negotiation();
        test_pwd_cwd();
        test_list();
        test_retr();
        test_size_mdtm();
        test_two_sessions();
        test_token_and_binding();
        test_stop_with_open_session();
    }

    tbox_cache_close(&g_cache_state);
    tbox_archive_free(&g_archive);
    scratch_drop();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

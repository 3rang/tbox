/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * net/ftpd.c - a read-only FTP server over the archive.
 *
 * Layer 3 of the plan: the only per-transport code. It speaks FTP over a
 * loopback socket and reads the archive through core/index + core/read, so it
 * has no idea that Telegram exists. All protocol *text* lives in
 * net/ftp_proto.c, which is tested without a network.
 *
 * Threading: one acceptor thread plus one thread per session. Nothing blocks
 * forever - every wait is a select() with a 200 ms tick that re-checks the stop
 * flags, and the data socket carries a send timeout - so tbox_ftpd_stop()
 * never has to reach into another thread's socket and never hangs.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: the usual POSIX-spelled names */

#ifdef _WIN32
#include <winsock2.h>            /* before windows.h, always */
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "ftpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/ftp_proto.h"
#include "util/tbox_thread.h"

/* ---- platform socket shims ------------------------------------------------ */

#ifdef _WIN32

typedef SOCKET tbox_fd_t;

#define TBOX_FD_INVALID       INVALID_SOCKET
#define tbox_fd_close(s)      closesocket(s)
#define tbox_sock_errno()     WSAGetLastError()
#define TBOX_SOCK_EINTR       WSAEINTR

#else

typedef int tbox_fd_t;

#define TBOX_FD_INVALID       (-1)
#define tbox_fd_close(s)      close(s)
#define tbox_sock_errno()     errno
#define TBOX_SOCK_EINTR       EINTR

#endif

/* How long a client has to open the data connection after a PASV. */
#define DATA_TIMEOUT_TICKS 15          /* x 1 s */

/* Select tick on every blocking wait, so stop is noticed within this long. */
#define TICK_USEC 200000L

/* A data send that stalls this long is treated as a dead client. */
#define DATA_SEND_TIMEOUT_SEC 10

/* Longest client path we keep around (an archive name is at most 512). */
#define CPATH_MAX 640

/* Output buffer for LIST/NLST, flushed whenever it fills. */
#define LIST_BUF 8192

/* Stream chunk for RETR. */
#define RETR_BUF 65536

/* Result codes shared by the transfer helpers. */
#define XFER_OK        0
#define XFER_FAILED  (-1)
#define XFER_CANCELLED (-2)    /* the server is shutting down */
#define XFER_NOBODY   (-3)    /* no local body for this entry */

/* ---- types ---------------------------------------------------------------- */

struct ftpd_session
{
    tbox_ftpd_t *server;
    tbox_fd_t control;
    tbox_fd_t data;               /* PASV listener, INVALID when none */
    int data_port;
    int data_addr[4];             /* the address handed back in 227 */
    char cwd[CPATH_MAX];
    int user_ok;                  /* USER tbox seen */
    int authed;                   /* PASS <token> accepted */
    volatile int stop;
    volatile int in_use;
    tbox_thread_t thread;
};

struct tbox_ftpd
{
    tbox_fd_t listen_fd;
    int port;
    int addr[4];
    char token[TBOX_FTPD_TOKEN_LEN];
    tbox_archive_t *archive;      /* borrowed */
    tbox_cache_t *cache;          /* borrowed */
    tbox_mutex_t lock;            /* guards the archive */
    volatile int stop;
    volatile int live;            /* open session count */
    tbox_thread_t acceptor;
    struct ftpd_session sessions[TBOX_FTPD_MAX_SESSIONS];
};

/* ---- small socket helpers ------------------------------------------------- */

/* 1 = readable (or hung up), 0 = nothing yet, -1 = error. */
static int fd_ready(tbox_fd_t fd, long usec)
{
    fd_set set;
    struct timeval tv;
    int r;

    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = (long)(usec / 1000000L);
    tv.tv_usec = (int)(usec % 1000000L);

    do {
        r = select((int)fd + 1, &set, NULL, NULL, &tv);
    } while (r < 0 && tbox_sock_errno() == TBOX_SOCK_EINTR);

    if (r < 0)
        return -1;

    return r > 0 ? 1 : 0;
}

static int write_all(tbox_fd_t fd, const char *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        size_t want = len - off;
        long sent;

        if (want > 65536)
            want = 65536;

        sent = send(fd, data + off, (int)want, 0);
        if (sent > 0) {
            off += (size_t)sent;
            continue;
        }
        if (sent < 0 && tbox_sock_errno() == TBOX_SOCK_EINTR)
            continue;
        return -1;
    }

    return 0;
}

static void reply_raw(struct ftpd_session *ss, const char *text)
{
    (void)write_all(ss->control, text, strlen(text));
}

static void reply(struct ftpd_session *ss, int code, const char *text)
{
    char buf[TBOX_FTP_LINE_MAX];

    if (tbox_ftp_reply(buf, sizeof buf, code, text) != 0)
        return;

    reply_raw(ss, buf);
}

static void session_stopped(struct ftpd_session *ss)
{
    ss->server->live--;
    ss->in_use = 0;               /* last: the slot is free again */
}

/* Constant-time compare, so a token guess cannot be timed. The length is not
 * secret (it is printed by `tbox serve`), so it is checked up front. */
static int token_matches(const char *got, const char *want)
{
    size_t i;
    size_t len;
    unsigned char diff = 0;

    if (got == NULL || want == NULL)
        return 0;

    len = strlen(want);
    if (strlen(got) != len)
        return 0;

    for (i = 0; i < len; i++)
        diff |= (unsigned char)(got[i] ^ want[i]);

    return diff == 0 ? 1 : 0;
}

/* Basename of an archive name ("a/b.txt" -> "b.txt"). */
static const char *archive_base(const char *name)
{
    const char *slash = strrchr(name, '/');

    return slash != NULL ? slash + 1 : name;
}

/* ---- data connection ------------------------------------------------------ */

static void data_close_listener(struct ftpd_session *ss)
{
    if (ss->data != TBOX_FD_INVALID) {
        tbox_fd_close(ss->data);
        ss->data = TBOX_FD_INVALID;
    }
    ss->data_port = 0;
}

static void data_close(struct ftpd_session *ss, tbox_fd_t fd)
{
    data_close_listener(ss);
    if (fd != TBOX_FD_INVALID)
        tbox_fd_close(fd);
}

/* Open a loopback listener on an ephemeral port for the next transfer. */
static int data_listen(struct ftpd_session *ss)
{
    struct sockaddr_in addr;
    tbox_fd_t fd;
    socklen_t len = (socklen_t)sizeof addr;
    int on = 1;

    data_close_listener(ss);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == TBOX_FD_INVALID)
        return -1;

    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on,
                     (int)sizeof on);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = 0;                            /* ephemeral */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0
        || listen(fd, 1) != 0
        || getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        tbox_fd_close(fd);
        return -1;
    }

    ss->data = fd;
    ss->data_port = ntohs(addr.sin_port);
    ss->data_addr[0] = (int)((ntohl(addr.sin_addr.s_addr) >> 24) & 0xffu);
    ss->data_addr[1] = (int)((ntohl(addr.sin_addr.s_addr) >> 16) & 0xffu);
    ss->data_addr[2] = (int)((ntohl(addr.sin_addr.s_addr) >> 8) & 0xffu);
    ss->data_addr[3] = (int)(ntohl(addr.sin_addr.s_addr) & 0xffu);

    return 0;
}

/* Give an accepted socket a send timeout, so a client that stops reading
 * cannot pin a session thread forever (stop() has to be able to join). */
static void set_send_timeout(tbox_fd_t fd, int seconds)
{
#ifdef _WIN32
    DWORD tv = (DWORD)seconds * 1000;
#else
    struct timeval tv;
#endif

#ifdef _WIN32
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv,
                     (int)sizeof tv);
#else
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv,
                     (int)sizeof tv);
#endif
}

/* Wait for the client to open the data connection. */
static tbox_fd_t data_accept(struct ftpd_session *ss)
{
    int ticks = 0;

    if (ss->data == TBOX_FD_INVALID)
        return TBOX_FD_INVALID;

    for (;;) {
        tbox_fd_t conn;
        int ready;

        if (ss->stop || ss->server->stop)
            return TBOX_FD_INVALID;

        ready = fd_ready(ss->data, 1000000L);
        if (ready < 0)
            return TBOX_FD_INVALID;
        if (ready == 0) {
            if (++ticks >= DATA_TIMEOUT_TICKS)
                return TBOX_FD_INVALID;
            continue;
        }

        conn = accept(ss->data, NULL, NULL);
        data_close_listener(ss);
        if (conn == TBOX_FD_INVALID)
            return TBOX_FD_INVALID;

        set_send_timeout(conn, DATA_SEND_TIMEOUT_SEC);

        return conn;
    }
}

/* ---- archive access (always under the server lock) ------------------------ */

/* stat one client path into `out`. Returns 0, or -1 when it does not exist. */
static int archive_stat(struct ftpd_session *ss, const char *path,
                        tbox_archive_entry_t *out)
{
    tbox_ftpd_t *s = ss->server;
    int rc;

    memset(out, 0, sizeof *out);

    tbox_mutex_lock(&s->lock);
    rc = tbox_archive_stat(s->archive, path, out);
    tbox_mutex_unlock(&s->lock);

    return rc;
}

static int archive_exists(struct ftpd_session *ss, const char *path)
{
    tbox_archive_entry_t tmp;

    return archive_stat(ss, path, &tmp) == 0;
}

/* ---- LIST / NLST ---------------------------------------------------------- */

/*
 * Buffered writer over the data socket: a listing can be far larger than one
 * send(), and the protocol has no length field to pre-compute.
 */
struct sink
{
    tbox_fd_t fd;
    char buf[LIST_BUF];
    size_t len;
    int failed;
};

static int sink_flush(struct sink *sk)
{
    if (sk->len == 0)
        return sk->failed ? XFER_FAILED : XFER_OK;

    if (!sk->failed && write_all(sk->fd, sk->buf, sk->len) != 0)
        sk->failed = 1;
    sk->len = 0;

    return sk->failed ? XFER_FAILED : XFER_OK;
}

static int sink_raw(struct sink *sk, const char *text)
{
    size_t len = strlen(text);

    if (sk->failed)
        return XFER_FAILED;
    if (len + 1 > sizeof sk->buf) {
        sk->failed = 1;                  /* longer than the whole buffer */
        return XFER_FAILED;
    }
    if (sk->len + len >= sizeof sk->buf && sink_flush(sk) != XFER_OK)
        return XFER_FAILED;

    memcpy(sk->buf + sk->len, text, len);
    sk->len += len;

    return XFER_OK;
}

static int sink_line(struct sink *sk, int is_dir, long long size,
                     long long mtime, const char *name)
{
    size_t len = sk->len;

    if (sk->failed)
        return XFER_FAILED;

    if (tbox_ftp_list_add(sk->buf, sizeof sk->buf, &len, is_dir, size, mtime,
                          name) != 0) {
        /* the line does not fit in what is left: flush and try once more */
        if (sink_flush(sk) != XFER_OK)
            return XFER_FAILED;
        len = 0;
        if (tbox_ftp_list_add(sk->buf, sizeof sk->buf, &len, is_dir, size,
                              mtime, name) != 0) {
            sk->failed = 1;
            return XFER_FAILED;
        }
    }

    sk->len = len;

    return XFER_OK;
}

/*
 * The body of LIST / NLST: one entry when the path is a file, otherwise the
 * direct children of the directory. Called with no lock held.
 */
static int list_send(struct ftpd_session *ss, tbox_fd_t fd, const char *path,
                     int names_only)
{
    tbox_ftpd_t *s = ss->server;
    struct sink sk;
    tbox_archive_entry_t self;
    tbox_archive_entry_t *kids = NULL;
    size_t cap = 0;
    size_t i;
    int rc;

    sk.fd = fd;
    sk.len = 0;
    sk.failed = 0;

    if (archive_stat(ss, path, &self) != 0)
        return XFER_FAILED;

    if (!self.is_dir) {
        if (names_only) {
            (void)sink_raw(&sk, archive_base(path));
            (void)sink_raw(&sk, "\r\n");
        } else {
            (void)sink_line(&sk, 0, self.size, self.mtime,
                            archive_base(path));
        }
        return sink_flush(&sk);
    }

    tbox_mutex_lock(&s->lock);
    rc = tbox_archive_list(s->archive, path, &kids, &cap);
    tbox_mutex_unlock(&s->lock);

    if (rc != 0 || kids == NULL)
        return XFER_FAILED;

    for (i = 0; i < cap; i++) {
        if (names_only) {
            if (sink_raw(&sk, archive_base(kids[i].name)) == XFER_OK)
                (void)sink_raw(&sk, "\r\n");
        } else {
            (void)sink_line(&sk, kids[i].is_dir, kids[i].size, kids[i].mtime,
                            archive_base(kids[i].name));
        }

        if (sk.failed) {
            free(kids);
            return XFER_FAILED;
        }
        if ((i & 0x3fu) == 0 && (ss->stop || s->stop)) {
            free(kids);
            return XFER_CANCELLED;
        }
    }

    free(kids);

    return sink_flush(&sk);
}

/* ---- RETR ----------------------------------------------------------------- */

static int retr_send(struct ftpd_session *ss, tbox_fd_t fd,
                     const tbox_archive_entry_t *ent)
{
    tbox_cache_t *cache = ss->server->cache;
    tbox_cfile_t file;
    char buf[RETR_BUF];
    long long left;
    int rc = XFER_OK;

    if (cache == NULL || ent->file_id == 0)
        return XFER_NOBODY;

    /* The download happens with no lock held: it can take seconds. */
    if (tbox_cfile_open(cache, ent->file_id, &file) != 0)
        return XFER_FAILED;

    left = tbox_cfile_size(&file);
    while (left > 0) {
        size_t want = (left < (long long)sizeof buf) ? (size_t)left
                                                     : sizeof buf;
        long long got = tbox_cfile_read(&file, buf, want);

        if (got <= 0) {
            rc = XFER_FAILED;
            break;
        }
        if (write_all(fd, buf, (size_t)got) != 0) {
            rc = XFER_FAILED;
            break;
        }
        left -= got;

        if (ss->stop || ss->server->stop) {
            rc = XFER_CANCELLED;
            break;
        }
    }

    tbox_cfile_close(&file);

    return rc;
}

/* ---- shared reply shapes -------------------------------------------------- */

static void reply_transfer_end(struct ftpd_session *ss, int rc, const char *what)
{
    if (rc == XFER_CANCELLED)
        reply(ss, 426, "Transfer aborted.");
    else if (rc == XFER_NOBODY)
        reply(ss, 550, "That file has no bytes available yet.");
    else if (rc != XFER_OK)
        reply(ss, 451, what);
    else
        reply(ss, 226, "Transfer complete.");
}

/* ---- command handlers ----------------------------------------------------- */

static void do_pwd(struct ftpd_session *ss)
{
    char buf[TBOX_FTP_LINE_MAX];

    /* 257 takes the path in quotes, as every real server does. */
    if (snprintf(buf, sizeof buf, "257 \"%s\"\r\n", ss->cwd) < 0)
        return;

    reply_raw(ss, buf);
}

static void do_cwd(struct ftpd_session *ss, const char *arg)
{
    char path[CPATH_MAX];
    tbox_archive_entry_t self;

    if (arg == NULL || arg[0] == '\0') {
        reply(ss, 501, "CWD needs a path.");
        return;
    }
    if (tbox_ftp_dir_join(ss->cwd, arg, path, sizeof path) != 0) {
        reply(ss, 501, "That path is too long.");
        return;
    }
    if (archive_stat(ss, path, &self) != 0 || !self.is_dir) {
        reply(ss, 550, "No such directory.");
        return;
    }

    memcpy(ss->cwd, path, strlen(path) + 1);
    do_pwd(ss);
}

static void do_pasv(struct ftpd_session *ss)
{
    char buf[TBOX_FTP_LINE_MAX];
    unsigned char addr[4];

    if (data_listen(ss) != 0) {
        reply(ss, 425, "Cannot open a data connection.");
        return;
    }

    addr[0] = (unsigned char)ss->data_addr[0];
    addr[1] = (unsigned char)ss->data_addr[1];
    addr[2] = (unsigned char)ss->data_addr[2];
    addr[3] = (unsigned char)ss->data_addr[3];

    if (tbox_ftp_pasv_reply(buf, sizeof buf, addr, ss->data_port) != 0) {
        data_close_listener(ss);
        reply(ss, 425, "Cannot open a data connection.");
        return;
    }

    reply_raw(ss, buf);
}

/* Resolve the argument of a data command into a client path. */
static int transfer_path(struct ftpd_session *ss, const char *arg, char *path,
                         size_t size)
{
    if (arg != NULL && arg[0] != '\0') {
        if (tbox_ftp_dir_join(ss->cwd, arg, path, size) != 0) {
            reply(ss, 501, "That path is too long.");
            return -1;
        }
    } else {
        size_t len = strlen(ss->cwd);

        if (len + 1 > size) {
            reply(ss, 501, "That path is too long.");
            return -1;
        }
        memcpy(path, ss->cwd, len + 1);
    }

    return 0;
}

static void do_list(struct ftpd_session *ss, const char *arg, int names_only)
{
    char path[CPATH_MAX];
    tbox_fd_t fd;
    int rc;

    if (ss->data == TBOX_FD_INVALID) {
        reply(ss, 425, "Use PASV first.");
        return;
    }
    if (transfer_path(ss, arg, path, sizeof path) != 0)
        return;

    /* Answer 550 before opening the data connection: a client that has
     * already connected would otherwise wait for a reply that never comes. */
    if (!archive_exists(ss, path)) {
        reply(ss, 550, "No such path.");
        return;
    }

    fd = data_accept(ss);
    if (fd == TBOX_FD_INVALID) {
        reply(ss, 425, "No data connection was opened.");
        return;
    }

    reply(ss, 150, names_only ? "Opening the data connection for names."
                              : "Opening the data connection for the list.");

    rc = list_send(ss, fd, path, names_only);

    data_close(ss, fd);

    if (rc == XFER_CANCELLED)
        reply(ss, 426, "Transfer aborted.");
    else if (rc != XFER_OK)
        reply(ss, 451, "Cannot produce the listing.");
    else
        reply(ss, 226, "Transfer complete.");
}

static void do_retr(struct ftpd_session *ss, const char *arg)
{
    char path[CPATH_MAX];
    tbox_archive_entry_t ent;
    tbox_fd_t fd;
    int rc;

    if (ss->data == TBOX_FD_INVALID) {
        reply(ss, 425, "Use PASV first.");
        return;
    }
    if (arg == NULL || arg[0] == '\0') {
        reply(ss, 501, "RETR needs a path.");
        return;
    }
    if (tbox_ftp_dir_join(ss->cwd, arg, path, sizeof path) != 0) {
        reply(ss, 501, "That path is too long.");
        return;
    }
    if (archive_stat(ss, path, &ent) != 0) {
        reply(ss, 550, "No such file.");
        return;
    }
    if (ent.is_dir) {
        reply(ss, 550, "That is a directory.");
        return;
    }

    fd = data_accept(ss);
    if (fd == TBOX_FD_INVALID) {
        reply(ss, 425, "No data connection was opened.");
        return;
    }

    reply(ss, 150, "Opening the data connection for the file.");

    rc = retr_send(ss, fd, &ent);

    data_close(ss, fd);

    reply_transfer_end(ss, rc, "Cannot send the file.");
}

static void do_size(struct ftpd_session *ss, const char *arg)
{
    char path[CPATH_MAX];
    tbox_archive_entry_t ent;
    char buf[TBOX_FTP_LINE_MAX];

    if (arg == NULL || arg[0] == '\0') {
        reply(ss, 501, "SIZE needs a path.");
        return;
    }
    if (tbox_ftp_dir_join(ss->cwd, arg, path, sizeof path) != 0) {
        reply(ss, 501, "That path is too long.");
        return;
    }
    if (archive_stat(ss, path, &ent) != 0) {
        reply(ss, 550, "No such file.");
        return;
    }

    if (ent.is_dir || ent.size <= 0) {
        reply(ss, 213, "0");
        return;
    }
    if (snprintf(buf, sizeof buf, "213 %lld\r\n", ent.size) < 0)
        return;

    reply_raw(ss, buf);
}

static void do_mdtm(struct ftpd_session *ss, const char *arg)
{
    char path[CPATH_MAX];
    tbox_archive_entry_t ent;
    char buf[TBOX_FTP_LINE_MAX];
    int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0;

    if (arg == NULL || arg[0] == '\0') {
        reply(ss, 501, "MDTM needs a path.");
        return;
    }
    if (tbox_ftp_dir_join(ss->cwd, arg, path, sizeof path) != 0) {
        reply(ss, 501, "That path is too long.");
        return;
    }
    if (archive_stat(ss, path, &ent) != 0) {
        reply(ss, 550, "No such path.");
        return;
    }

    /* MDTM has no way to say "no timestamp", so answer 550 rather than invent
     * the epoch and show the client a date the archive never had. */
    if (ent.mtime <= 0
        || tbox_ftp_utc(ent.mtime, &year, &month, &day, &hour, &minute, &second)
           != 0) {
        reply(ss, 550, "That entry has no modification time.");
        return;
    }

    if (snprintf(buf, sizeof buf, "213 %04d%02d%02d%02d%02d%02d\r\n", year,
                 month, day, hour, minute, second) < 0)
        return;

    reply_raw(ss, buf);
}

/*
 * Dispatch one parsed command. Returns 1 when the session should end.
 */
static int dispatch(struct ftpd_session *ss, const tbox_ftp_cmd_t *cmd)
{
    tbox_ftpd_t *s = ss->server;

    /* --- allowed before a session is established --- */
    if (tbox_ftp_is(cmd, "USER")) {
        if (cmd->argc < 1 || strcmp(cmd->arg[0], "tbox") != 0) {
            ss->user_ok = 0;
            ss->authed = 0;
            reply(ss, 530, "This server only accepts USER tbox.");
            return 0;
        }
        ss->user_ok = 1;
        reply(ss, 331, "Password required: the token tbox serve printed.");
        return 0;
    }

    if (tbox_ftp_is(cmd, "PASS")) {
        if (!ss->user_ok) {
            reply(ss, 503, "Send USER first.");
            return 0;
        }
        if (cmd->argc < 1 || !token_matches(cmd->arg[0], s->token)) {
            ss->authed = 0;
            reply(ss, 530, "Login incorrect.");
            return 0;
        }
        ss->authed = 1;
        reply(ss, 230, "Logged in. This archive is read-only.");
        return 0;
    }

    if (tbox_ftp_is(cmd, "QUIT")) {
        reply(ss, 221, "Goodbye.");
        return 1;
    }

    if (tbox_ftp_is(cmd, "NOOP")) {
        reply(ss, 200, "OK.");
        return 0;
    }

    if (tbox_ftp_is(cmd, "FEAT")) {
        char buf[512];

        if (tbox_ftp_feat_reply(buf, sizeof buf) != 0) {
            reply(ss, 500, "Cannot build the feature list.");
            return 0;
        }
        reply_raw(ss, buf);
        return 0;
    }

    if (tbox_ftp_is(cmd, "SYST")) {
        reply(ss, 215, "UNIX Type: L8");
        return 0;
    }

    if (tbox_ftp_is(cmd, "TYPE")) {
        if (cmd->argc < 1) {
            reply(ss, 501, "TYPE needs a code.");
        } else if (cmd->arg[0][0] == 'A' || cmd->arg[0][0] == 'I'
                   || cmd->arg[0][0] == 'L') {
            reply(ss, 200, "Type accepted.");
        } else {
            reply(ss, 504, "Only A, I and L are understood.");
        }
        return 0;
    }

    if (tbox_ftp_is(cmd, "MODE")) {
        if (cmd->argc >= 1 && cmd->arg[0][0] == 'S')
            reply(ss, 200, "Stream mode only.");
        else
            reply(ss, 504, "Only stream mode is supported.");
        return 0;
    }

    if (tbox_ftp_is(cmd, "STRU")) {
        if (cmd->argc >= 1 && cmd->arg[0][0] == 'F')
            reply(ss, 200, "File structure only.");
        else
            reply(ss, 504, "Only file structure is supported.");
        return 0;
    }

    if (tbox_ftp_is(cmd, "OPTS")) {
        reply(ss, 200, "OK.");
        return 0;
    }

    if (tbox_ftp_is(cmd, "CLNT")) {
        /* WinSCP and FileZilla announce themselves here; accepting it keeps
         * them from falling back to a different dialect. */
        reply(ss, 200, "tbox");
        return 0;
    }

    if (tbox_ftp_is(cmd, "PBSZ")) {
        reply(ss, 200, "PBSZ=0");
        return 0;
    }

    if (tbox_ftp_is(cmd, "AUTH") || tbox_ftp_is(cmd, "PROT")) {
        reply(ss, 534, "This server is plain FTP on loopback only.");
        return 0;
    }

    /* --- everything below needs a session --- */
    if (!ss->authed) {
        reply(ss, 530, "Not logged in.");
        return 0;
    }

    if (tbox_ftp_is(cmd, "PWD") || tbox_ftp_is(cmd, "XPWD")) {
        do_pwd(ss);
        return 0;
    }

    if (tbox_ftp_is(cmd, "CWD") || tbox_ftp_is(cmd, "XCWD")) {
        do_cwd(ss, cmd->argc >= 1 ? cmd->arg[0] : NULL);
        return 0;
    }

    if (tbox_ftp_is(cmd, "CDUP") || tbox_ftp_is(cmd, "XCUP")) {
        do_cwd(ss, "..");
        return 0;
    }

    if (tbox_ftp_is(cmd, "PASV")) {
        do_pasv(ss);
        return 0;
    }

    if (tbox_ftp_is(cmd, "EPSV")) {
        reply(ss, 502, "EPSV is not implemented - use PASV.");
        return 0;
    }

    if (tbox_ftp_is(cmd, "LIST") || tbox_ftp_is(cmd, "NLST")) {
        int names_only = tbox_ftp_is(cmd, "NLST");
        const char *arg = cmd->argc >= 1 ? cmd->arg[0] : NULL;

        /* clients that pass flags ("LIST -la /dir"): ignore them */
        if (arg != NULL && arg[0] == '-')
            arg = (cmd->argc >= 2) ? cmd->arg[1] : NULL;

        do_list(ss, arg, names_only);
        return 0;
    }

    if (tbox_ftp_is(cmd, "RETR")) {
        do_retr(ss, cmd->argc >= 1 ? cmd->arg[0] : NULL);
        return 0;
    }

    if (tbox_ftp_is(cmd, "SIZE")) {
        do_size(ss, cmd->argc >= 1 ? cmd->arg[0] : NULL);
        return 0;
    }

    if (tbox_ftp_is(cmd, "MDTM")) {
        do_mdtm(ss, cmd->argc >= 1 ? cmd->arg[0] : NULL);
        return 0;
    }

    /* --- deliberately not implemented: every write, plus the extras --- */
    if (tbox_ftp_is(cmd, "STOR") || tbox_ftp_is(cmd, "APPE")
        || tbox_ftp_is(cmd, "DELE") || tbox_ftp_is(cmd, "MKD")
        || tbox_ftp_is(cmd, "RMD") || tbox_ftp_is(cmd, "RNFR")
        || tbox_ftp_is(cmd, "RNTO") || tbox_ftp_is(cmd, "SITE")
        || tbox_ftp_is(cmd, "ALLO") || tbox_ftp_is(cmd, "REST")
        || tbox_ftp_is(cmd, "MLSD") || tbox_ftp_is(cmd, "MLST")
        || tbox_ftp_is(cmd, "STAT") || tbox_ftp_is(cmd, "HOST")) {
        reply(ss, 502, "Not implemented - this server is read-only.");
        return 0;
    }

    reply(ss, 500, "Unknown command.");
    return 0;
}

/* ---- session thread ------------------------------------------------------- */

/*
 * Read one CRLF-terminated control line. Returns 0 on success, -1 on EOF,
 * error or a stop request. A line longer than `size` is drained and cut short
 * rather than split into two bogus commands.
 */
static int read_line(struct ftpd_session *ss, char *buf, size_t size)
{
    size_t len = 0;
    tbox_fd_t fd = ss->control;

    for (;;) {
        char c;
        long n;
        int ready;

        if (ss->stop || ss->server->stop)
            return -1;

        ready = fd_ready(fd, TICK_USEC);
        if (ready < 0)
            return -1;
        if (ready == 0)
            continue;

        n = recv(fd, &c, 1, 0);
        if (n <= 0)
            return -1;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (len + 1 < size)
            buf[len++] = c;
    }

    buf[len] = '\0';

    return 0;
}

static void *session_main(void *arg)
{
    struct ftpd_session *ss = (struct ftpd_session *)arg;
    char line[TBOX_FTP_LINE_MAX];

    ss->cwd[0] = '/';
    ss->cwd[1] = '\0';

    reply(ss, 220, "tbox read-only FTP - log in with USER tbox");

    while (!ss->stop && !ss->server->stop) {
        tbox_ftp_cmd_t cmd;

        if (read_line(ss, line, sizeof line) != 0)
            break;

        if (tbox_ftp_parse(line, &cmd) != 0) {
            reply(ss, 500, "Empty command.");
            continue;
        }

        if (dispatch(ss, &cmd) != 0)
            break;
    }

    /* A client that vanished mid-transfer still gets its sockets closed. */
    data_close_listener(ss);
    if (ss->control != TBOX_FD_INVALID) {
        tbox_fd_close(ss->control);
        ss->control = TBOX_FD_INVALID;
    }

    session_stopped(ss);

    return NULL;
}

/* ---- acceptor ------------------------------------------------------------- */

static void *acceptor_main(void *arg)
{
    tbox_ftpd_t *s = (tbox_ftpd_t *)arg;

    while (!s->stop) {
        struct ftpd_session *ss = NULL;
        tbox_fd_t conn;
        int i;

        if (fd_ready(s->listen_fd, TICK_USEC) <= 0)
            continue;

        conn = accept(s->listen_fd, NULL, NULL);
        if (conn == TBOX_FD_INVALID)
            continue;

        for (i = 0; i < TBOX_FTPD_MAX_SESSIONS; i++) {
            if (!s->sessions[i].in_use) {
                ss = &s->sessions[i];
                break;
            }
        }

        if (ss == NULL) {
            /* Full house: say so and close, rather than leave the client
             * waiting for a greeting that will never arrive. */
            static const char busy[] = "421 Too many connections.\r\n";

            (void)write_all(conn, busy, sizeof busy - 1);
            tbox_fd_close(conn);
            continue;
        }

        memset(ss, 0, sizeof *ss);
        ss->server = s;
        ss->control = conn;
        ss->data = TBOX_FD_INVALID;
        ss->in_use = 1;
        s->live++;
        set_send_timeout(conn, DATA_SEND_TIMEOUT_SEC);

        if (tbox_thread_start(&ss->thread, session_main, ss) != 0) {
            tbox_fd_close(conn);
            ss->in_use = 0;
            s->live--;
            continue;
        }
    }

    return NULL;
}

/* ---- public API ----------------------------------------------------------- */

/* True only for loopback: the archive must never be reachable off-host. */
static int addr_is_loopback(const char *addr)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;

    if (addr == NULL || addr[0] == '\0')
        return 0;
    if (strcmp(addr, "::1") == 0 || strcmp(addr, "localhost") == 0)
        return 1;
    if (sscanf(addr, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;

    return a == 127 && b < 256 && c < 256 && d < 256;
}

int tbox_ftpd_token_generate(char *out, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    int ok = 0;

    if (out == NULL || size < TBOX_FTPD_TOKEN_LEN)
        return -1;

    /* Empty rather than zeroed: a caller that ignores the return value still
     * cannot end up with a token it will accept. */
    out[0] = '\0';
    if (size > TBOX_FTPD_TOKEN_LEN)
        out[TBOX_FTPD_TOKEN_LEN] = '\0';

#ifdef _WIN32
    ok = BCryptGenRandom(NULL, raw, (ULONG)sizeof raw,
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    {
        FILE *f = fopen("/dev/urandom", "rb");

        if (f != NULL) {
            ok = fread(raw, 1, sizeof raw, f) == sizeof raw;
            fclose(f);
        }
    }
#endif

    if (!ok)
        return -1;                /* the caller must not serve this */

    {
        size_t i;

        for (i = 0; i < sizeof raw; i++) {
            out[i * 2] = hex[(raw[i] >> 4) & 0xfu];
            out[i * 2 + 1] = hex[raw[i] & 0xfu];
        }
    }

    /* 32 hex characters, then the terminator: TBOX_FTPD_TOKEN_LEN - 1 of them. */
    out[TBOX_FTPD_TOKEN_LEN - 1] = '\0';

    return 0;
}

tbox_ftpd_t *tbox_ftpd_start(const tbox_ftpd_opts_t *opts, char *token,
                              size_t token_size)
{
    tbox_ftpd_t *s;
    struct sockaddr_in addr;
    socklen_t len = (socklen_t)sizeof addr;
    const char *bind_addr;
    int on = 1;
    int i;

    if (opts == NULL || opts->archive == NULL) {
        fprintf(stderr, "ftpd: no archive to serve.\n");
        return NULL;
    }

    bind_addr = (opts->bind_addr != NULL) ? opts->bind_addr : "127.0.0.1";
    if (!addr_is_loopback(bind_addr)) {
        fprintf(stderr, "ftpd: %s is not a loopback address; this server "
                        "refuses to leave the machine.\n", bind_addr);
        return NULL;
    }

#ifdef _WIN32
    {
        WSADATA wsa;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "ftpd: cannot initialise Winsock.\n");
            return NULL;
        }
    }
#endif

    s = (tbox_ftpd_t *)calloc(1, sizeof *s);
    if (s == NULL) {
        fprintf(stderr, "ftpd: out of memory.\n");
        return NULL;
    }

    s->listen_fd = TBOX_FD_INVALID;
    s->archive = opts->archive;
    s->cache = opts->cache;
    s->port = opts->port;
    s->addr[0] = 127;
    s->addr[3] = 1;
    for (i = 0; i < TBOX_FTPD_MAX_SESSIONS; i++)
        s->sessions[i].data = TBOX_FD_INVALID;

    if (tbox_ftpd_token_generate(s->token, sizeof s->token) != 0) {
        fprintf(stderr, "ftpd: no system entropy source; refusing to serve "
                        "with a guessable token.\n");
        free(s);
        return NULL;
    }
    if (token != NULL && token_size >= TBOX_FTPD_TOKEN_LEN)
        memcpy(token, s->token, sizeof s->token);

    if (tbox_mutex_init(&s->lock) != 0) {
        fprintf(stderr, "ftpd: cannot create the archive lock.\n");
        free(s);
        return NULL;
    }

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd == TBOX_FD_INVALID) {
        fprintf(stderr, "ftpd: cannot create a socket.\n");
        tbox_mutex_free(&s->lock);
        free(s);
        return NULL;
    }

    (void)setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on,
                     (int)sizeof on);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)opts->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(s->listen_fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0) {
        fprintf(stderr, "ftpd: cannot bind 127.0.0.1:%d.\n", opts->port);
        tbox_fd_close(s->listen_fd);
        tbox_mutex_free(&s->lock);
        free(s);
        return NULL;
    }
    if (listen(s->listen_fd, 8) != 0) {
        fprintf(stderr, "ftpd: cannot listen on 127.0.0.1:%d.\n", opts->port);
        tbox_fd_close(s->listen_fd);
        tbox_mutex_free(&s->lock);
        free(s);
        return NULL;
    }

    /* Learn the port actually bound - this is what makes port 0 usable. */
    if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &len) == 0) {
        s->port = ntohs(addr.sin_port);
        s->addr[0] = (int)((ntohl(addr.sin_addr.s_addr) >> 24) & 0xffu);
        s->addr[1] = (int)((ntohl(addr.sin_addr.s_addr) >> 16) & 0xffu);
        s->addr[2] = (int)((ntohl(addr.sin_addr.s_addr) >> 8) & 0xffu);
        s->addr[3] = (int)(ntohl(addr.sin_addr.s_addr) & 0xffu);
    }

    if (tbox_thread_start(&s->acceptor, acceptor_main, s) != 0) {
        fprintf(stderr, "ftpd: cannot start the acceptor thread.\n");
        tbox_fd_close(s->listen_fd);
        tbox_mutex_free(&s->lock);
        free(s);
        return NULL;
    }

    return s;
}

void tbox_ftpd_stop(tbox_ftpd_t *server)
{
    int i;
    int spins;

    if (server == NULL)
        return;

    server->stop = 1;
    for (i = 0; i < TBOX_FTPD_MAX_SESSIONS; i++)
        server->sessions[i].stop = 1;

    /* The acceptor leaves within one tick, then every session notices the flag
     * and closes its own sockets. */
    (void)tbox_thread_join(&server->acceptor);

    for (spins = 0; spins < 300; spins++) {
        int busy = 0;

        for (i = 0; i < TBOX_FTPD_MAX_SESSIONS; i++) {
            if (server->sessions[i].in_use)
                busy = 1;
        }
        if (!busy)
            break;
        tbox_sleep_ms(10);
    }

    for (i = 0; i < TBOX_FTPD_MAX_SESSIONS; i++) {
        if (server->sessions[i].in_use) {
            (void)tbox_thread_join(&server->sessions[i].thread);
            server->sessions[i].in_use = 0;
        }
    }

    if (server->listen_fd != TBOX_FD_INVALID)
        tbox_fd_close(server->listen_fd);
    server->listen_fd = TBOX_FD_INVALID;

    tbox_mutex_free(&server->lock);

#ifdef _WIN32
    WSACleanup();
#endif

    free(server);
}

int tbox_ftpd_port(const tbox_ftpd_t *server)
{
    return server != NULL ? server->port : 0;
}

const char *tbox_ftpd_token(const tbox_ftpd_t *server)
{
    return server != NULL ? server->token : "";
}

int tbox_ftpd_sessions(const tbox_ftpd_t *server)
{
    return server != NULL ? server->live : 0;
}

void tbox_ftpd_archive_lock(tbox_ftpd_t *server)
{
    if (server != NULL)
        tbox_mutex_lock(&server->lock);
}

void tbox_ftpd_archive_unlock(tbox_ftpd_t *server)
{
    if (server != NULL)
        tbox_mutex_unlock(&server->lock);
}

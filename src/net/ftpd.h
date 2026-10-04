/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_FTPD_H
#define TBOX_FTPD_H

#include <stddef.h>

#include "core/index.h"
#include "core/read.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ftpd - the FTP transport: a read-only FTP server over the archive.
 *
 * This is the only per-transport code in the app (the plan's "layer 3"), and
 * it knows nothing about Telegram: it reads the archive index (core/index) and
 * the file bodies (core/read), and the protocol text lives in net/ftp_proto.
 *
 * Security model, both parts enforced in code rather than in a comment:
 *   - loopback only. The bind address is rejected unless it is a loopback
 *     address, so the archive is never reachable from the network.
 *   - USER tbox with the session token as the password. The token is 16 bytes
 *     from the OS RNG, printed by `tbox serve`, and compared in constant time,
 *     so another local process cannot guess its way in.
 *
 * Sessions: one thread per connection, capped at TBOX_FTPD_MAX_SESSIONS. Every
 * archive read happens under the server's own mutex, so a rescan can rebuild
 * the index between commands without a session ever seeing a half-built list
 * (use tbox_ftpd_archive_lock/unlock for that).
 *
 * Supported: USER PASS SYST FEAT TYPE MODE STRU OPTS NOOP PWD CWD CDUP PASV
 * LIST NLST RETR SIZE MDTM QUIT, plus harmless replies for CLNT/PBSZ/AUTH.
 * Refused on purpose: writes (STOR DELE MKD RMD RNFR RNTO APPE), EPSV and REST.
 */

#define TBOX_FTPD_MAX_SESSIONS 8

/* 16 random bytes as hex, plus NUL. */
#define TBOX_FTPD_TOKEN_LEN 33

typedef struct tbox_ftpd tbox_ftpd_t;

typedef struct
{
    const char *bind_addr;        /* NULL = 127.0.0.1; must be loopback */
    int port;                     /* 0 = let the OS pick (used by tests) */
    tbox_archive_t *archive;      /* borrowed; must outlive the server */
    tbox_cache_t *cache;          /* borrowed; NULL serves names only */

} tbox_ftpd_opts_t;

/*
 * Start listening. Returns NULL on failure with the reason printed; on success
 * `token` receives the session token that clients must send as the password.
 */
tbox_ftpd_t *tbox_ftpd_start(const tbox_ftpd_opts_t *opts, char *token,
                              size_t token_size);

/*
 * Stop accepting, close every session and release everything. Blocks until all
 * session threads have finished. Safe to call with NULL.
 */
void tbox_ftpd_stop(tbox_ftpd_t *server);

/* The port actually bound (meaningful after port 0 was requested). */
int tbox_ftpd_port(const tbox_ftpd_t *server);

/* The session token; same value handed to tbox_ftpd_start(). */
const char *tbox_ftpd_token(const tbox_ftpd_t *server);

/* How many sessions are open right now (for status.json). */
int tbox_ftpd_sessions(const tbox_ftpd_t *server);

/*
 * Hold the archive lock across a rebuild. Every index read the server does is
 * taken under the same lock, so this is all a caller needs to swap the index
 * contents without a session tripping over it. Recursive: calling these from a
 * helper that is itself called under the lock is fine.
 */
void tbox_ftpd_archive_lock(tbox_ftpd_t *server);
void tbox_ftpd_archive_unlock(tbox_ftpd_t *server);

/*
 * Fill `out` with TBOX_FTPD_TOKEN_LEN-1 random characters from the OS entropy
 * source (BCryptGenRandom on Windows, /dev/urandom elsewhere), NUL-terminated.
 *
 * Returns 0, or -1 when the entropy source is unavailable - in which case
 * `out` is left empty rather than filled with something guessable. The failure
 * is reported rather than inferred from the value: a real 128-bit token starts
 * with "00" about once every 256 runs, so no caller can spot one.
 */
int tbox_ftpd_token_generate(char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_FTPD_H */

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CMD_H
#define TBOX_CMD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exit codes (deliberately small):
 *  0 = success
 *  1 = runtime error (also: command exists but is not available in build)
 *  2 = unknown command / bad usage
 */
#define TBOX_EXIT_OK          0
#define TBOX_ERROR            1
#define TBOX_UNKNOWN_COMMAND  2

/*
 * The auth runner (td/tdhandler.c's tbox_td_auth_run) is injected by
 * main() instead of linked from tboxcore, so the core library and the
 * headless unit tests never touch TDLib.
 *
 * Contract: run the QR login flow, store the session under data_dir
 * (NULL = per-OS default), and abort cooperatively whenever
 * *should_stop becomes true. Returns 0 on success, non-zero otherwise.
 */
typedef int (*tbox_auth_runner_t)(const char *data_dir,
                                  volatile bool *should_stop);

/*
 * Wire the auth command. runner = TDLib login entry point,
 * should_stop = the shared flag the signal listener flips on Ctrl+C etc.
 * Until this is called, `tbox auth` reports itself unavailable.
 */
void tbox_cmd_set_auth(tbox_auth_runner_t runner,
                       volatile bool *should_stop);

/*
 * The serve runner (td/tdserve.c's tbox_td_serve_run) is injected the same
 * way as the auth runner, for the same reason: tboxcore and the headless
 * unit tests must not link TDLib.
 *
 * Contract: take exclusive ownership of the session in `root` (NULL = the
 * per-OS default), keep it open, publish status.json as the archive
 * becomes servable, and return 0 when asked to stop. It must abort
 * cooperatively whenever *should_stop becomes true.
 */
typedef int (*tbox_serve_runner_t)(const char *root,
                                   volatile bool *should_stop);

/*
 * Wire the serve command. Until this is called, `tbox serve` still takes the
 * session lock and writes status.json - it just has nothing to serve yet.
 */
void tbox_cmd_set_serve(tbox_serve_runner_t runner,
                        volatile bool *should_stop);

/*
 * Tell the command layer whether TDLib is compiled into this binary. Only
 * main() knows this (tboxcore is TDLib-free by rule), and `tbox serve` needs
 * it to keep two very different situations apart:
 *
 *   present   - Telegram support is right there, but the archive scanner is
 *               not written yet. Nothing the user can do about it.
 *   absent    - the build has no Telegram support at all, and the fix is to
 *               run scripts/fetch-tdjson.* . Nothing the user can do about it
 *               either, but a completely different thing.
 *
 * Without this, `serve` blames TDLib for a TDLib-less binary's missing runner,
 * which is the wrong story whenever TDLib is present.
 */
void tbox_cmd_set_tdlib_present(bool present);

/*
 * Shared argument handling (cmd/args.c): every command takes the same
 * optional `--data <dir>`, and auth / serve / status must agree on where
 * the session lives, so the parsing and root resolution happen once.
 */

/* Parse `<cmd> [--data <dir>]`; *data_dir is NULL when the flag is absent.
 * Returns TBOX_EXIT_OK, or TBOX_UNKNOWN_COMMAND with a message printed. */
int tbox_cmd_parse_data(int argc, char *argv[], const char **data_dir);

/* Resolve the data root into `out`. `create` makes the directory (serve);
 * pass 0 for read-only commands so asking changes nothing on disk.
 * Returns 0, or -1 with a message printed. */
int tbox_cmd_resolve_root(const char *cmd, const char *data_dir, char *out,
                          size_t size, int create);

void tbox_cmd_version(void);
void tbox_cmd_help(void);
int tbox_cmd_auth(int argc, char *argv[]);
int tbox_cmd_serve(int argc, char *argv[]);
int tbox_cmd_status(int argc, char *argv[]);
int tbox_cmd_selftest(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_CMD_H */

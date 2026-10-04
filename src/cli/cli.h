/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CLI_H
#define TBOX_CLI_H

/*
 * CLI entry point: parse argv and dispatch to the command implementations
 * (see cmd/cmd.h). Returns an exit code (TBOX_EXIT_* / TBOX_ERROR).
 *
 * Surface:
 *   auth [--data <dir>]     log in via QR code (QR-only)
 *   serve [--data <dir>]    own the session and serve the archive
 *   status [--data <dir>]   show serve state (reads status.json only)
 *   selftest                check the core rules offline
 *   help | -v | -h
 *
 * QR-only by design - there is no --qr switch; auth is QR by default.
 */
int tbox_cli(int argc, char *argv[]);

#endif /* TBOX_CLI_H */
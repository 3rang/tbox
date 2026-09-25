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
 * Surface: auth | status | help | selftest | -v | -h.
 * QR-only by design - there is no --qr switch; auth is QR by default.
 */
int tbox_cli(int argc, char *argv[]);

#endif /* TBOX_CLI_H */
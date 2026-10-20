/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * cli.h — argv parsing and dispatch to cmd/ implementations.
 */
#ifndef TBOX_CLI_H
#define TBOX_CLI_H

/* Parses argv and dispatches; returns a TBOX_EXIT_* code. */
int tbox_cli(int argc, char *argv[]);

#endif /* TBOX_CLI_H */

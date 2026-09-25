/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CMD_H
#define TBOX_CMD_H

/* Exit codes (deliberately small):
 *  0 = success
 *  1 = runtime error (for now also: command exists but is not implemented)
 *  2 = unknown command / bad usage
 */
#define TBOX_EXIT_OK          0
#define TBOX_ERROR            1
#define TBOX_UNKNOWN_COMMAND  2

void tbox_cmd_version(void);
void tbox_cmd_help(void);
int tbox_cmd_auth(int argc, char *argv[]);
int tbox_cmd_status(int argc, char *argv[]);
int tbox_cmd_selftest(int argc, char *argv[]);

#endif /* TBOX_CMD_H */
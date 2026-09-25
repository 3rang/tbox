/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CMD_H
#define TBOX_CMD_H

#define TBOX_EXIT_OK 0       /* success */
#define TBOX_ERROR   1       /* generic error */
#define TBOX_UNKNOWN_COMMAND 2   /* unknown command */

void tbox_cmd_version(void);
void tbox_cmd_help(void);

#endif /* TBOX_CMD_H */
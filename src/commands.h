/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * commands.h - one function per sub-command. argv[0] is the first
 * argument after the command name. Each returns an exit code.
 */
#ifndef TBOX_COMMANDS_H
#define TBOX_COMMANDS_H

#include "td.h"

int cmd_login(int argc, char **argv);
int cmd_logout(int argc, char **argv);
int cmd_me(int argc, char **argv);
int cmd_search(int argc, char **argv);
int cmd_send(int argc, char **argv);

/* Print the logged-in account (shared by login and me). */
int print_me(Td *td);

#endif

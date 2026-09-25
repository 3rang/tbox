/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include "cli/cli.h"
#include "cmd/cmd.h"

int main(int argc, char *argv[])
{
    return tbox_cli(argc, argv);
}
/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include "cli/cli.h"

int main(int argc, char *argv[])
{
    /* Everything flows through the CLI dispatcher -> command table. */
    return tbox_cli(argc, argv);
}
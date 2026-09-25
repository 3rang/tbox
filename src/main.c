/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include "cli/cli.h"
#include "cmd/cmd.h"
#include "util/tbox_signal.h"

int main(int argc, char *argv[])
{
  
    int rc = tbox_cli(argc, argv);

    return rc;
}
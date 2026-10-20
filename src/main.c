/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * main.c — argv in, exit code out. All real work lives in cli/ and cmd/.
 */
#include "cli/cli.h"

int main(int argc, char **argv) {
    return tbox_cli(argc, argv);
}

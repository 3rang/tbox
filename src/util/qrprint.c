/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * qrprint.c - render a string as a QR code in the terminal.
 * Ported verbatim from demo/qr_login.c print_qr(); the only change is
 * that the caller passes the vt flag instead of a file-static.
 */

#include "qrprint.h"

#include <stdio.h>
#include <stdint.h>

#include <qrcodegen.h>

void tbox_qr_print(const char *text, int vt)
{
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    int size, x, y;
    const int margin = 4;

    printf("\n  Scan this QR code with your Telegram app\n"
           "  (Settings > Devices > Link Desktop Device):\n\n");

    if (!qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, 1)) {
        printf("  (QR too large to render - open the link directly:\n"
               "  %s)\n\n", text);
        return;
    }
    size = qrcodegen_getSize(qr);

    if (vt) {
        const char *blk = "\033[40m  \033[0m"; /* black module */
        const char *wht = "\033[47m  \033[0m"; /* white module */
        for (y = -margin; y < size + margin; y++) {
            fputs("  ", stdout);
            for (x = -margin; x < size + margin; x++) {
                int dark = 0;
                if (x >= 0 && x < size && y >= 0 && y < size)
                    dark = qrcodegen_getModule(qr, x, y);
                fputs(dark ? blk : wht, stdout);
            }
            putchar('\n');
        }
    } else {
        for (y = 0; y < size; y++) {
            fputs("  ", stdout);
            for (x = 0; x < size; x++)
                fputs(qrcodegen_getModule(qr, x, y) ? "##" : "  ", stdout);
            putchar('\n');
        }
    }
    printf("\n  ...or open directly: %s\n\n", text);
}

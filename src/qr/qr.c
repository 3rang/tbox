/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * qr.c — QR painting in the terminal (ported from demo/qr_login.c where
 * it survived real phone scans; keep the geometry unless proven broken).
 */
#include "qr/qr.h"

#include <stdio.h>

#include "qrcodegen.h"

#ifdef _WIN32
#include <windows.h>

static int g_vt = 0; /* ANSI/UTF-8 console support enabled */

int tbox_qr_console_setup(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return 0;
    }
    if (!GetConsoleMode(h, &mode)) {
        return 0;
    }
    if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        return 0;
    }
    if (SetConsoleOutputCP(65001) == 0) {
        return 0;
    }
    g_vt = 1;
    return 1;
}
#else
int tbox_qr_console_setup(void) { return 1; }
#endif

void tbox_qr_render(const char *link) {
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    int size, x, y;
    const int margin = 4;

    printf("\n  Scan this QR code with your Telegram app\n"
           "  (Settings > Devices > Link Desktop Device):\n\n");
    if (!qrcodegen_encodeText(link, temp, qr, qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, 1)) {
        printf("  (QR too large to render - open the link directly:\n"
               "  %s)\n\n", link);
        return;
    }
    size = qrcodegen_getSize(qr);

#ifdef _WIN32
    if (g_vt) {
        const char *blk = "\033[40m  \033[0m"; /* black module */
        const char *wht = "\033[47m  \033[0m"; /* white module */
        for (y = -margin; y < size + margin; y++) {
            fputs("  ", stdout);
            for (x = -margin; x < size + margin; x++) {
                int dark = 0;
                if (x >= 0 && x < size && y >= 0 && y < size) {
                    dark = qrcodegen_getModule(qr, x, y);
                }
                fputs(dark ? blk : wht, stdout);
            }
            putchar('\n');
        }
    } else
#endif
    {
        for (y = 0; y < size; y++) {
            fputs("  ", stdout);
            for (x = 0; x < size; x++) {
                fputs(qrcodegen_getModule(qr, x, y) ? "##" : "  ", stdout);
            }
            putchar('\n');
        }
    }
    printf("\n  ...or open directly: %s\n\n", link);
    fflush(stdout);
}

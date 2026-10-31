/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "tg/qr.h"

#include <stdbool.h>
#include <stdint.h>

#include "qrcodegen.h"

#define QUIET_ZONE 2 /* light modules around the code; the spec says 4, phones accept 2 */

static bool module_at(const uint8_t *qr, int size, int x, int y) {
    if (x < 0 || y < 0 || x >= size || y >= size) {
        return false; /* quiet zone is light */
    }
    return qrcodegen_getModule(qr, x, y);
}

int qr_render(const char *text, FILE *out) {
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];

    if (text == NULL || text[0] == '\0' ||
        !qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return -1;
    }
    int size = qrcodegen_getSize(qr);

    /* White foreground on black background; each character shows two
     * vertically stacked modules: upper half, lower half, both or none. */
    for (int y = -QUIET_ZONE; y < size + QUIET_ZONE; y += 2) {
        fputs("\033[97;40m", out);
        for (int x = -QUIET_ZONE; x < size + QUIET_ZONE; x++) {
            bool top_dark = module_at(qr, size, x, y);
            bool bottom_dark = module_at(qr, size, x, y + 1);
            if (y + 1 >= size + QUIET_ZONE) {
                bottom_dark = false;
            }
            if (!top_dark && !bottom_dark) {
                fputs("\xE2\x96\x88", out); /* █ both light */
            } else if (!top_dark) {
                fputs("\xE2\x96\x80", out); /* ▀ top light */
            } else if (!bottom_dark) {
                fputs("\xE2\x96\x84", out); /* ▄ bottom light */
            } else {
                fputc(' ', out); /*   both dark */
            }
        }
        fputs("\033[0m\n", out);
    }
    fflush(out);
    return 0;
}

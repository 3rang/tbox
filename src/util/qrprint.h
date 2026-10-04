/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_QRPRINT_H
#define TBOX_QRPRINT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Terminal QR rendering (util layer: knows qrcodegen + ANSI, nothing
 * about Telegram). Used by the auth flow to show the login link.
 */

/*
 * Render text as a QR code on stdout.
 *
 *   vt != 0  -> ANSI black/white blocks (needs tbox_term_enable_vt)
 *   vt == 0  -> plain "##" character grid
 *
 * If the payload is too large to encode, prints the text itself as a
 * clickable fallback. Always also prints the raw text under the code.
 */
void tbox_qr_print(const char *text, int vt);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_QRPRINT_H */

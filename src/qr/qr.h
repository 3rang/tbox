/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * qr.h — terminal rendering of the tdlib QR-login link.
 *
 * The link cannot be typed or clicked: the phone's Telegram app must scan
 * it as a QR code, so painting it in the terminal IS the login UI. Ported
 * from the proven demo: real black/white blocks via ANSI VT on Windows,
 * ## grid fallback otherwise. TDLib rotates the token ~every 30 s; call
 * again whenever the link changes.
 */
#ifndef TBOX_QR_H
#define TBOX_QR_H

/* Switches the Windows console to ANSI mode + UTF-8 so blocks paint
 * correctly; returns non-0 when enabled (non-Windows: always 1). */
int tbox_qr_console_setup(void);

/* Paints the link as a QR code plus the raw text as fallback. */
void tbox_qr_render(const char *link);

#endif /* TBOX_QR_H */

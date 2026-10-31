/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * qr.h - draw a QR code in the terminal, for the login link.
 */
#ifndef TBOX_QR_H
#define TBOX_QR_H

#include <stdio.h>

/* Print `text` as a QR code to out. Two modules per text line (Unicode
 * half blocks) with explicit colours, so it scans on light and dark
 * terminals alike. Returns 0, or -1 if the text does not fit. */
int qr_render(const char *text, FILE *out);

#endif

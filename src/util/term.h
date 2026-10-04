/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_TERM_H
#define TBOX_TERM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Console helpers shared by the auth flow (util layer: no TDLib, no
 * Telegram knowledge - just terminal I/O).
 */

/*
 * Switch the terminal to UTF-8 + ANSI-escape mode so the QR can be drawn
 * as real black/white blocks (Windows only; POSIX terminals already do).
 *
 * Returns:
 *   1 = virtual-terminal mode is usable
 *   0 = not available (caller should use the ASCII fallback)
 */
int tbox_term_enable_vt(void);


/*
 * Read one line from stdin without echoing it (masked with '*').
 * Handles backspace, Ctrl-C/Esc (cancel) and platform differences
 * (_getch on Windows, fgets elsewhere).
 *
 * On Windows the read happens only when echo is disabled; the terminal
 * state is always restored before returning.
 *
 * Returns:
 *   >=0 = number of characters stored (line, without newline)
 *   -1  = cancelled (Ctrl-C / Esc) or read error / buffer too small
 */
int tbox_term_read_masked_line(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_TERM_H */

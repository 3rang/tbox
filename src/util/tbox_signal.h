/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_SIGNAL_H
#define TBOX_SIGNAL_H

/*
 * Interrupt listener - one API, two native backends.
 *
 *   signal_win.c   (MSVC / Windows)   SetConsoleCtrlHandler + event + thread
 *   signal_posix.c (Linux / macOS)    self-pipe for SIGINT/SIGTERM/SIGHUP + thread
 *
 * CMake picks the backend; this header and tests/test_signal.c are identical
 * on every platform. "Ctrl+C" on Linux means SIGINT; anything that wants the
 * program to stop (terminal ^C, kill -INT, kill -TERM) funnels into the same
 * sticky flags.
 *
 * Install this at the very top of main(), BEFORE any other activity, so an
 * early interrupt is never lost. Neither backend touches TDLib (threading
 * law - the bridge worker is the only td_receive caller). The main/bridge
 * loop just polls tbox_signal_pending() between td_receive calls.
 *
 * Semantics (Step 17 of the ladder, pulled forward):
 *   - first interrupt  -> graceful stop requested (exit 130)
 *   - second interrupt -> force stop (hard 130, no further clean-up promised)
 *
 * Thread-safe. Idempotent install.
 */

/* Start the listener. Call once, first thing in main(). */
void tbox_signal_install(void);

/* Stop the listener (join thread, restore default signal behaviour).
 * Optional; main calls it before returning so tests can re-install cleanly. */
void tbox_signal_shutdown(void);

/* 1 if any interrupt has been received (sticky until shutdown). */
int tbox_signal_pending(void);

/* 1 if a second interrupt arrived (force path). Sticky. */
int tbox_signal_force(void);

/* Block up to ms for an interrupt; returns 1 immediately if one is already
 * pending. Usable by a worker between td_receive calls. */
int tbox_signal_wait(unsigned ms);

/* Internal/test hook: behaves exactly like an OS interrupt (used by ctest
 * to exercise the flags without a real console event / kill). */
void tbox_signal_fire(void);

#endif /* TBOX_SIGNAL_H */
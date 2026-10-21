/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * rand.h — the engine's randomness: object ids and op ids (spec 7.2)
 * and the TDLib database key (S5) all come from here, and all of them
 * come from the OS cryptographic generator (spec 3.2). rand() is never
 * used, anywhere.
 */
#ifndef TBOX_RAND_H
#define TBOX_RAND_H

#include <stddef.h>

typedef enum TBoxRandStatus {
    TBOX_RAND_OK = 0,
    TBOX_RAND_E_ARGS = -1, /* NULL buffer, zero or oversized request,
                            * output buffer too small */
    TBOX_RAND_E_IO = -2    /* the OS generator failed */
} TBoxRandStatus;

/* The largest single draw accepted (bytes): covers the 32-byte database
 * key (S5) and the 16-byte object id (spec 7.2) with room to spare. */
#define TBOX_RAND_MAX 64

/* nbytes fresh CSPRNG bytes into buf. */
TBoxRandStatus tbox_rand_fill(void *buf, size_t nbytes);

/* nbytes fresh CSPRNG bytes rendered as 2 * nbytes lowercase hex chars
 * plus a NUL - 32 chars for an object id (nbytes 16), 16 for an op id
 * (nbytes 8). out_size must hold 2 * nbytes + 1. */
TBoxRandStatus tbox_rand_hex(char *out, size_t out_size, size_t nbytes);

#endif /* TBOX_RAND_H */

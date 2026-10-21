/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * rand.c — OS CSPRNG (spec 3.2: BCryptGenRandom on Windows,
 * getrandom//dev/urandom on POSIX; never rand()). Two rules that only
 * matter here:
 *
 *   - A generator failure is an error code, never a weaker fallback:
 *     a predictable object id would break idempotency (spec 7.2), so
 *     there is no "seed from the clock" escape hatch.
 *   - POSIX keeps a /dev/urandom fallback for kernels without
 *     getrandom; it is still the kernel CSPRNG, only the entry point
 *     differs.
 */
#include "util/rand.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h> /* BCryptGenRandom */
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h> /* getrandom */
#endif
#endif

TBoxRandStatus tbox_rand_fill(void *buf, size_t nbytes) {
    if (buf == NULL || nbytes == 0 || nbytes > TBOX_RAND_MAX) {
        return TBOX_RAND_E_ARGS;
    }
#ifdef _WIN32
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)nbytes,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(st) ? TBOX_RAND_OK : TBOX_RAND_E_IO;
#else
    size_t got = 0;
#if defined(__linux__)
    while (got < nbytes) {
        ssize_t r = getrandom((unsigned char *)buf + got, nbytes - got, 0);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* older kernel: the urandom fallback below */
        }
        got += (size_t)r;
    }
    if (got == nbytes) {
        return TBOX_RAND_OK;
    }
#endif
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return TBOX_RAND_E_IO;
    }
    while (got < nbytes) {
        ssize_t r = read(fd, (unsigned char *)buf + got, nbytes - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(fd);
            return TBOX_RAND_E_IO;
        }
        if (r == 0) { /* urandom should never hit EOF; refuse partial data */
            (void)close(fd);
            return TBOX_RAND_E_IO;
        }
        got += (size_t)r;
    }
    (void)close(fd);
    return TBOX_RAND_OK;
#endif
}

TBoxRandStatus tbox_rand_hex(char *out, size_t out_size, size_t nbytes) {
    static const char HEX[] = "0123456789abcdef";
    if (out == NULL || nbytes == 0 || nbytes > TBOX_RAND_MAX ||
        out_size < 2 * nbytes + 1) {
        return TBOX_RAND_E_ARGS;
    }
    unsigned char raw[TBOX_RAND_MAX];
    TBoxRandStatus st = tbox_rand_fill(raw, nbytes);
    if (st != TBOX_RAND_OK) {
        return st;
    }
    for (size_t i = 0; i < nbytes; i++) {
        out[2 * i] = HEX[raw[i] >> 4];
        out[2 * i + 1] = HEX[raw[i] & 0x0F];
    }
    out[2 * nbytes] = '\0';
    return TBOX_RAND_OK;
}

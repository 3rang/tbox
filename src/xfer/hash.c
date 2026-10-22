/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * hash.c — the content hash of a file (spec 7.2, 3.2).
 *
 * The hash is the vendored SHA-256 (third_party/sha256), fed from the file
 * in fixed chunks. Nothing here keeps the file in memory: the buffer is one
 * chunk, and it is the same size whatever the file is.
 */
#include "xfer/hash.h"

#include <string.h>

#include "sha256.h"
#include "util/fsx.h"

/* How much is read at a time. Big enough that the read overhead does not
 * matter, small enough that the buffer is nothing. */
#define TBOX_HASH_CHUNK 65536

int tbox_hash_file(const char *path, char out[TBOX_SHA256_HEX_LEN + 1],
                   long long *size_out) {
    if (out == NULL) {
        return -1;
    }
    out[0] = '\0';
    if (size_out != NULL) {
        *size_out = 0;
    }
    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    TBoxFsxFile *file = NULL;
    if (tbox_fsx_open_read(path, &file) != TBOX_FSX_OK) {
        return -1;
    }

    SHA256_CTX ctx;
    sha256_init(&ctx);
    unsigned char buf[TBOX_HASH_CHUNK];
    long long total = 0;
    int failed = 0;
    for (;;) {
        size_t got = 0;
        if (tbox_fsx_read(file, buf, sizeof buf, &got) != TBOX_FSX_OK) {
            failed = 1;
            break;
        }
        if (got == 0) {
            break; /* the end of the file */
        }
        sha256_update(&ctx, buf, got);
        total += (long long)got;
    }
    tbox_fsx_close_read(file);
    if (failed) {
        return -1;
    }

    unsigned char digest[SHA256_BLOCK_SIZE];
    sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        out[i * 2] = hex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[SHA256_BLOCK_SIZE * 2] = '\0';
    if (size_out != NULL) {
        *size_out = total;
    }
    return 0;
}

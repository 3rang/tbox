/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * hash.h — the content hash of a file (spec 7.2, 3.2).
 *
 * The catalog remembers the SHA-256 of every managed file, so a file that
 * comes back can be checked against the one that went out (spec 8.6). The
 * file is read in chunks, so a file of any size is hashed without holding
 * it in memory, and the path goes through the UTF-8 doorway of util/fsx.h
 * so a name in any language works (coding rules).
 */
#ifndef TBOX_XFER_HASH_H
#define TBOX_XFER_HASH_H

#include <stddef.h>

#include "catalog/record.h"

/* The SHA-256 of the file at path, as 64 lowercase hex characters.
 *
 * Returns 0 on success. On failure the hex buffer is empty and size_out
 * (when it is not NULL) holds 0. */
int tbox_hash_file(const char *path, char out[TBOX_SHA256_HEX_LEN + 1],
                   long long *size_out);

#endif /* TBOX_XFER_HASH_H */

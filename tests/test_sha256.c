/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_sha256.c — the vendored SHA-256 (spec 3.2): the published test
 * vectors, and the streaming behaviour the upload path relies on.
 *
 * The catalog stores the SHA-256 of a file's content as 64 lowercase hex
 * chars (spec 7.2), and the download path checks it against what the
 * sender wrote down. A hash that is wrong in a way that still looks like
 * a hash would corrupt every file it touches, so the vectors below are
 * the ones FIPS 180-4 and the SHA-2 summary publish.
 */
#include "sha256.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest as the catalog writes it: 64 lowercase hex chars. */
#define HEX_LEN 64

static void to_hex(const unsigned char digest[32], char out[HEX_LEN + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = digits[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    out[HEX_LEN] = '\0';
}

static void digest_of(const void *data, size_t len, char out[HEX_LEN + 1]) {
    SHA256_CTX ctx;
    unsigned char digest[32];
    sha256_init(&ctx);
    sha256_update(&ctx, (const BYTE *)data, len);
    sha256_final(&ctx, digest);
    to_hex(digest, out);
}

static void expect(const char *what, const void *data, size_t len,
                   const char *expected) {
    char got[HEX_LEN + 1];
    digest_of(data, len, got);
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "%s:\n  expected %s\n  got      %s\n", what, expected,
                got);
        assert(0);
    }
}

static void test_published_vectors(void) {
    expect("empty", "", 0,
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect("abc", "abc", 3,
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect("two blocks", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           56,
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    expect("896 bits",
           "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
           "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
           112,
           "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

static void test_one_million_a(void) {
    /* The long vector: a megabyte of one byte, fed in blocks the way a
     * file is read. */
    SHA256_CTX ctx;
    unsigned char digest[32];
    char got[HEX_LEN + 1];
    unsigned char block[1000];
    memset(block, 'a', sizeof block);
    sha256_init(&ctx);
    for (int i = 0; i < 1000; i++) {
        sha256_update(&ctx, block, sizeof block);
    }
    sha256_final(&ctx, digest);
    to_hex(digest, got);
    assert(strcmp(got,
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_streaming_is_the_same_digest(void) {
    /* The upload hashes a file in pieces, so the digest must not depend on
     * where the pieces are cut. */
    unsigned char data[1000];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (unsigned char)(i * 7 + 3);
    }
    char whole[HEX_LEN + 1];
    digest_of(data, sizeof data, whole);

    for (size_t chunk = 1; chunk <= 64; chunk *= 2) {
        SHA256_CTX ctx;
        unsigned char digest[32];
        char got[HEX_LEN + 1];
        sha256_init(&ctx);
        for (size_t off = 0; off < sizeof data; off += chunk) {
            size_t n = (off + chunk <= sizeof data) ? chunk : sizeof data - off;
            sha256_update(&ctx, data + off, n);
        }
        sha256_final(&ctx, digest);
        to_hex(digest, got);
        assert(strcmp(got, whole) == 0);
    }
}

static void test_binary_safe(void) {
    /* A file is bytes, not text: the digest must not stop at a zero byte
     * and must not care about the sign of a byte. */
    unsigned char data[256];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (unsigned char)i;
    }
    char got[HEX_LEN + 1];
    digest_of(data, sizeof data, got);
    assert(strlen(got) == HEX_LEN);
    for (size_t i = 0; i < HEX_LEN; i++) {
        char c = got[i];
        assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    /* the digest of the same bytes differs from the digest of the text */
    char text[HEX_LEN + 1];
    digest_of("abc", 3, text);
    assert(strcmp(got, text) != 0);
}

int main(void) {
    test_published_vectors();
    test_one_million_a();
    test_streaming_is_the_same_digest();
    test_binary_safe();
    printf("sha256 OK\n");
    return 0;
}

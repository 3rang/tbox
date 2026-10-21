/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_rand.c — the CSPRNG (spec 3.2) that mints object ids and op ids
 * (spec 7.2): freshness, exact lowercase-hex shape, argument rules, and
 * a byte-coverage smoke that would catch a generator stuck on a corner
 * of its output space.
 */
#include "util/rand.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static void test_fill_draws_fresh_bytes(void) {
    unsigned char a[TBOX_RAND_MAX];
    unsigned char b[TBOX_RAND_MAX];
    assert(tbox_rand_fill(a, sizeof a) == TBOX_RAND_OK);
    assert(tbox_rand_fill(b, sizeof b) == TBOX_RAND_OK);
    /* two 64-byte draws colliding is 2^-512 territory */
    assert(memcmp(a, b, sizeof a) != 0);
    int nonzero = 0;
    for (size_t i = 0; i < sizeof a; i++) {
        if (a[i] != 0) {
            nonzero = 1;
        }
    }
    assert(nonzero);
    /* argument rules */
    assert(tbox_rand_fill(NULL, 16) == TBOX_RAND_E_ARGS);
    assert(tbox_rand_fill(a, 0) == TBOX_RAND_E_ARGS);
    assert(tbox_rand_fill(a, TBOX_RAND_MAX + 1) == TBOX_RAND_E_ARGS);
}

static void test_hex_is_lowercase_and_two_n_chars(void) {
    char out[2 * TBOX_RAND_MAX + 1];
    char again[2 * TBOX_RAND_MAX + 1];
    /* object id: 16 bytes -> 32 lowercase hex chars (spec 7.2) */
    assert(tbox_rand_hex(out, sizeof out, 16) == TBOX_RAND_OK);
    assert(strlen(out) == 32);
    for (size_t i = 0; i < 32; i++) {
        assert(is_lower_hex(out[i]));
    }
    /* op id: 8 bytes -> 16 hex chars (spec 7.2) */
    assert(tbox_rand_hex(out, sizeof out, 8) == TBOX_RAND_OK);
    assert(strlen(out) == 16);
    /* two ids never collide */
    assert(tbox_rand_hex(again, sizeof again, 8) == TBOX_RAND_OK);
    assert(strcmp(out, again) != 0);
}

static void test_hex_argument_rules(void) {
    char out[80];
    assert(tbox_rand_hex(NULL, sizeof out, 16) == TBOX_RAND_E_ARGS);
    assert(tbox_rand_hex(out, sizeof out, 0) == TBOX_RAND_E_ARGS);
    assert(tbox_rand_hex(out, sizeof out, TBOX_RAND_MAX + 1) == TBOX_RAND_E_ARGS);
    /* 16 bytes -> 32 chars + NUL: one byte short of room is refused */
    assert(tbox_rand_hex(out, 32, 16) == TBOX_RAND_E_ARGS);
    assert(tbox_rand_hex(out, 33, 16) == TBOX_RAND_OK);
}

static void test_every_byte_value_appears(void) {
    /* 128 draws of 64 bytes = 8192 samples over 256 values; a correct
     * CSPRNG misses some value with odds ~1e-11, so a failure here means
     * a broken generator, not bad luck. */
    unsigned char seen[256];
    unsigned char buf[TBOX_RAND_MAX];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < 128; i++) {
        assert(tbox_rand_fill(buf, sizeof buf) == TBOX_RAND_OK);
        for (size_t j = 0; j < sizeof buf; j++) {
            seen[buf[j]] = 1;
        }
    }
    for (int v = 0; v < 256; v++) {
        assert(seen[v] == 1);
    }
}

int main(void) {
    test_fill_draws_fresh_bytes();
    test_hex_is_lowercase_and_two_n_chars();
    test_hex_argument_rules();
    test_every_byte_value_appears();
    printf("test_tbox_rand: ok\n");
    return 0;
}

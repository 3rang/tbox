/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_qr.c - the terminal QR renderer.
 */
#include "check.h"
#include "tg/qr.h"

static void test_renders_login_link(void) {
    FILE *f = tmpfile();
    CHECK(f != NULL);
    CHECK(qr_render("tg://login?token=AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA", f) == 0);
    long len = ftell(f);
    CHECK(len > 500); /* a few dozen lines of blocks */
    rewind(f);
    int lines = 0, c;
    while ((c = fgetc(f)) != EOF) {
        lines += c == '\n';
    }
    /* version 3-4 code (29-33 modules) + 4 quiet rows, two rows per line */
    CHECK(lines >= 16 && lines <= 20);
    fclose(f);
}

static void test_rejects_bad_input(void) {
    FILE *f = tmpfile();
    CHECK(qr_render("", f) == -1);
    CHECK(qr_render(NULL, f) == -1);
    char huge[8000];
    memset(huge, 'x', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';
    CHECK(qr_render(huge, f) == -1);
    fclose(f);
}

int main(void) {
    test_renders_login_link();
    test_rejects_bad_input();
    TEST_MAIN_END();
}

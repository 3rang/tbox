/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_record.c â€” the tbx1 record (spec 7.2) and the S8 name rules.
 *
 * Table-driven: valid records parse to the right fields, invalid ones are
 * refused for the right reason, unknown keys are ignored, serialization is
 * byte-for-byte deterministic and round-trips, and every name S8 forbids is
 * rejected (while a few lookalikes are allowed).
 */
#include "catalog/record.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define O1 "0123456789abcdef0123456789abcdef"
#define O2 "fedcba9876543210fedcba9876543210"
#define Q1 "0123456789abcdef"
#define H1 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* ---- helpers --------------------------------------------------------- */

/* Copies a short literal into a field. Bounded, so it is the safe stand-in
 * for strcpy (which MSVC deprecates under /W4). */
static void set_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src);
}

static TBoxRecord parse_ok(const char *text) {
    TBoxRecord rec;
    TBoxRecordStatus st = tbox_record_parse(text, &rec);
    if (st != TBOX_REC_OK) {
        printf("expected OK, got %d for: %s\n", st, text);
    }
    assert(st == TBOX_REC_OK);
    return rec;
}

static void expect_status(const char *text, TBoxRecordStatus want) {
    TBoxRecord rec;
    TBoxRecordStatus got = tbox_record_parse(text, &rec);
    if (got != want) {
        printf("expected %d, got %d for: %s\n", want, got, text);
    }
    assert(got == want);
}

/* ---- parsing: valid -------------------------------------------------- */

static void test_parse_minimal_file(void) {
    TBoxRecord rec = parse_ok("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1
                              "\",\"p\":\"root\",\"n\":\"a.txt\",\"r\":1,\"s\":10}");
    assert(rec.version == 1);
    assert(rec.kind == 'f');
    assert(strcmp(rec.object_id, O1) == 0);
    assert(strcmp(rec.parent, "root") == 0);
    assert(strcmp(rec.name, "a.txt") == 0);
    assert(rec.rev == 1);
    assert(rec.has_size && rec.size == 10);
    assert(!rec.has_sha256 && !rec.has_mtime && !rec.has_trashed);
    assert(!rec.has_op && !rec.has_src);
    printf("record: minimal file parses\n");
}

static void test_parse_folder_without_size(void) {
    TBoxRecord rec = parse_ok("tbx1 {\"v\":1,\"k\":\"d\",\"o\":\"" O1
                              "\",\"p\":\"root\",\"n\":\"docs\",\"r\":1}");
    assert(rec.kind == 'd');
    assert(!rec.has_size);
    printf("record: a folder needs no size\n");
}

static void test_parse_all_optional_fields(void) {
    TBoxRecord rec = parse_ok("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1
                              "\",\"p\":\"" O2 "\",\"n\":\"x.bin\",\"r\":3,\"s\":42,"
                              "\"h\":\"" H1 "\",\"m\":1760000000,\"t\":1760000001,"
                              "\"q\":\"" Q1 "\",\"src\":\"123456789\"}");
    assert(rec.rev == 3);
    assert(rec.has_sha256 && strcmp(rec.sha256, H1) == 0);
    assert(rec.has_mtime && rec.mtime == 1760000000);
    assert(rec.has_trashed && rec.trashed_at == 1760000001);
    assert(rec.has_op && strcmp(rec.op_id, Q1) == 0);
    assert(rec.has_src && strcmp(rec.src, "123456789") == 0);
    assert(strcmp(rec.parent, O2) == 0);
    printf("record: every optional field parses\n");
}

static void test_unknown_keys_are_ignored(void) {
    /* forward compatibility: a future key must not break an old reader */
    TBoxRecord rec = parse_ok("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1
                              "\",\"p\":\"root\",\"n\":\"a.txt\",\"r\":1,\"s\":1,"
                              "\"future\":\"whatever\",\"zeta\":[1,2,3]}");
    assert(strcmp(rec.name, "a.txt") == 0);
    printf("record: unknown keys ignored\n");
}

/* ---- parsing: invalid ------------------------------------------------ */

static void test_not_a_record(void) {
    expect_status("hello world", TBOX_REC_E_NOPREFIX);
    expect_status("tbx2 {\"v\":1}", TBOX_REC_E_NOPREFIX);
    expect_status("", TBOX_REC_E_NOPREFIX);
    printf("record: a plain message is not a record\n");
}

static void test_bad_syntax(void) {
    expect_status("tbx1 not json", TBOX_REC_E_SYNTAX);
    expect_status("tbx1 {}", TBOX_REC_E_VERSION); /* parses, but no v */
    expect_status("tbx1 [1,2,3]", TBOX_REC_E_SYNTAX);
    printf("record: malformed bodies are refused\n");
}

static void test_bad_version(void) {
    expect_status("tbx1 {\"v\":2,\"k\":\"f\"}", TBOX_REC_E_VERSION);
    expect_status("tbx1 {\"v\":\"1\",\"k\":\"f\"}", TBOX_REC_E_TYPE);
    printf("record: only version 1 is accepted\n");
}

static void test_missing_required(void) {
    expect_status("tbx1 {\"v\":1,\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":1}",
                  TBOX_REC_E_MISSING); /* no k */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"p\":\"root\",\"n\":\"a\",\"r\":1,\"s\":1}",
                  TBOX_REC_E_MISSING); /* no o */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"n\":\"a\",\"r\":1,\"s\":1}",
                  TBOX_REC_E_MISSING); /* no p */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"root\",\"r\":1,\"s\":1}",
                  TBOX_REC_E_MISSING); /* no n */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"s\":1}",
                  TBOX_REC_E_MISSING); /* no r */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":1}",
                  TBOX_REC_E_MISSING); /* file without s */
    printf("record: a missing required key is refused\n");
}

static void test_wrong_types(void) {
    expect_status("tbx1 {\"v\":1,\"k\":5,\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":1,\"s\":1}",
                  TBOX_REC_E_TYPE);
    expect_status("tbx1 {\"v\":1,\"k\":\"x\",\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":1,\"s\":1}",
                  TBOX_REC_E_TYPE);
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":\"1\",\"s\":1}",
                  TBOX_REC_E_TYPE);
    printf("record: wrong JSON types are refused\n");
}

static void test_bad_ids(void) {
    /* o must be 32 lowercase hex */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"short\",\"p\":\"root\",\"n\":\"a\",\"r\":1,\"s\":1}",
                  TBOX_REC_E_ID);
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" /* uppercase */
                  "0123456789ABCDEF0123456789ABCDEF\",\"p\":\"root\",\"n\":\"a\",\"r\":1,\"s\":1}",
                  TBOX_REC_E_ID);
    /* p must be root or 32 hex */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"nope\",\"n\":\"a\",\"r\":1,\"s\":1}",
                  TBOX_REC_E_ID);
    /* h must be 64 hex */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":1,\"s\":1,\"h\":\"abc\"}",
                  TBOX_REC_E_ID);
    /* q must be 16 hex */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":1,\"s\":1,\"q\":\"zz\"}",
                  TBOX_REC_E_ID);
    printf("record: bad ids are refused\n");
}

static void test_ranges(void) {
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":0,\"s\":1}",
                  TBOX_REC_E_RANGE); /* r < 1 */
    expect_status("tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"root\",\"n\":\"a\",\"r\":1,\"s\":-5}",
                  TBOX_REC_E_RANGE); /* negative size */
    printf("record: out-of-range numbers are refused\n");
}

/* ---- names (S8) ------------------------------------------------------ */

static void expect_name_ok(const char *name) {
    assert(tbox_name_validate(name) == TBOX_REC_OK);
}

static void expect_name_bad(const char *name) {
    TBoxRecordStatus st = tbox_name_validate(name);
    if (st != TBOX_REC_E_NAME) {
        printf("expected the name to be refused: %s\n", name);
    }
    assert(st == TBOX_REC_E_NAME);
}

static void test_good_names(void) {
    expect_name_ok("report.pdf");
    expect_name_ok("my file.txt");
    expect_name_ok("2026-01-01 backup.tar.gz");
    expect_name_ok("cafe-restaurant.txt");
    /* Non-ASCII names are ordinary; the bytes are spelled out so this source
     * file stays pure ASCII (MSVC would otherwise warn on its code page). */
    expect_name_ok("caf\xc3\xa9.txt");       /* cafe with an acute accent */
    expect_name_ok("\xe4\xb8\xad\xe6\x96\x87.txt"); /* two CJK ideographs */
    expect_name_ok("CONX");                  /* not the reserved CON */
    expect_name_ok("a.very.long.ext.name");
    printf("record: ordinary names are accepted\n");
}

static void test_forbidden_characters(void) {
    expect_name_bad("a/b");
    expect_name_bad("a\\b");
    expect_name_bad("a:b");
    expect_name_bad("a*b");
    expect_name_bad("a?b");
    expect_name_bad("a\"b");
    expect_name_bad("a<b");
    expect_name_bad("a>b");
    expect_name_bad("a|b");
    expect_name_bad("a\tb");   /* control char */
    expect_name_bad("a\nb");   /* control char */
    printf("record: forbidden characters are refused\n");
}

static void test_path_tricks(void) {
    expect_name_bad("..");
    expect_name_bad(".");
    expect_name_bad("../etc/passwd");
    expect_name_bad("a/../b");
    expect_name_bad("/absolute");
    expect_name_bad("\\absolute");
    printf("record: path tricks are refused\n");
}

static void test_trailing_and_reserved(void) {
    expect_name_bad("trailing.");
    expect_name_bad("trailing ");
    expect_name_bad("CON");
    expect_name_bad("con");
    expect_name_bad("NUL");
    expect_name_bad("COM1");
    expect_name_bad("lpt9.txt");   /* reserved stem, with an extension */
    expect_name_bad("");           /* empty */
    printf("record: trailing dots/spaces and reserved devices are refused\n");
}

static void test_name_too_long(void) {
    char big[400];
    memset(big, 'a', sizeof big);
    big[TBOX_NAME_MAX_BYTES] = '\0';
    expect_name_ok(big);                    /* exactly 255 is fine */
    big[TBOX_NAME_MAX_BYTES] = 'a';
    big[TBOX_NAME_MAX_BYTES + 1] = '\0';
    expect_name_bad(big);                   /* 256 is not */
    printf("record: the 255-byte limit is enforced\n");
}

static void test_sanitize_salvages(void) {
    char out[512];
    tbox_name_sanitize("a/b:c*?.txt", out, sizeof out);
    assert(strcmp(out, "a_b_c__.txt") == 0);

    tbox_name_sanitize("  name with spaces  ", out, sizeof out);
    assert(strcmp(out, "  name with spaces") == 0); /* trailing trimmed */

    tbox_name_sanitize("...", out, sizeof out);
    assert(strcmp(out, "unnamed") == 0);

    tbox_name_sanitize("CON", out, sizeof out);
    assert(strcmp(out, "unnamed") == 0);

    tbox_name_sanitize("normal.txt", out, sizeof out);
    assert(strcmp(out, "normal.txt") == 0);

    /* a name whose bytes exceed the budget is cut on a codepoint boundary */
    char big[TBOX_NAME_MAX_BYTES + 8];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    tbox_name_sanitize(big, out, sizeof out);
    assert(strlen(out) <= TBOX_NAME_MAX_BYTES);
    printf("record: sanitize makes a bad name writable\n");
}

/* ---- building -------------------------------------------------------- */

static void test_build_is_deterministic(void) {
    TBoxRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.version = 1;
    rec.kind = 'f';
    set_str(rec.object_id, sizeof rec.object_id, O1);
    set_str(rec.parent, sizeof rec.parent, "root");
    set_str(rec.name, sizeof rec.name, "report.pdf");
    rec.rev = 1;
    rec.has_size = 1;
    rec.size = 12345;

    char a[512];
    char b[512];
    assert(tbox_record_build(&rec, a, sizeof a, 0) == TBOX_REC_OK);
    assert(tbox_record_build(&rec, b, sizeof b, 0) == TBOX_REC_OK);
    assert(strcmp(a, b) == 0); /* byte-for-byte stable */
    assert(strcmp(a, "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1
                    "\",\"p\":\"root\",\"n\":\"report.pdf\",\"r\":1,\"s\":12345}") == 0);
    printf("record: build is deterministic and exact\n");
}

static void test_build_key_order_with_optionals(void) {
    TBoxRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.version = 1;
    rec.kind = 'f';
    set_str(rec.object_id, sizeof rec.object_id, O1);
    set_str(rec.parent, sizeof rec.parent, O2);
    set_str(rec.name, sizeof rec.name, "x.bin");
    rec.rev = 3;
    rec.has_size = 1;
    rec.size = 42;
    rec.has_sha256 = 1;
    set_str(rec.sha256, sizeof rec.sha256, H1);
    rec.has_mtime = 1;
    rec.mtime = 1760000000;
    rec.has_trashed = 1;
    rec.trashed_at = 1760000001;
    rec.has_op = 1;
    set_str(rec.op_id, sizeof rec.op_id, Q1);
    rec.has_src = 1;
    set_str(rec.src, sizeof rec.src, "123456789");

    char out[1024];
    assert(tbox_record_build(&rec, out, sizeof out, 0) == TBOX_REC_OK);
    /* the spec fixes the order: v,k,o,p,n,r,s,h,m,t,q,src */
    assert(strcmp(out, "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"" O1 "\",\"p\":\"" O2
                       "\",\"n\":\"x.bin\",\"r\":3,\"s\":42,\"h\":\"" H1
                       "\",\"m\":1760000000,\"t\":1760000001,\"q\":\"" Q1
                       "\",\"src\":\"123456789\"}") == 0);
    printf("record: optional keys come out in the spec order\n");
}

static void test_roundtrip(void) {
    TBoxRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.version = 1;
    rec.kind = 'd';
    set_str(rec.object_id, sizeof rec.object_id, O1);
    set_str(rec.parent, sizeof rec.parent, "root");
    set_str(rec.name, sizeof rec.name, "docs");
    rec.rev = 7;

    char text[512];
    assert(tbox_record_build(&rec, text, sizeof text, 0) == TBOX_REC_OK);
    TBoxRecord back = parse_ok(text);
    assert(back.kind == 'd');
    assert(strcmp(back.object_id, O1) == 0);
    assert(strcmp(back.name, "docs") == 0);
    assert(back.rev == 7);
    printf("record: build then parse round-trips\n");
}

static void test_build_refuses_bad_input(void) {
    TBoxRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.version = 1;
    rec.kind = 'f';
    set_str(rec.object_id, sizeof rec.object_id, O1);
    set_str(rec.parent, sizeof rec.parent, "root");
    set_str(rec.name, sizeof rec.name, "bad/name");
    rec.rev = 1;
    rec.has_size = 1;
    char out[512];
    assert(tbox_record_build(&rec, out, sizeof out, 0) == TBOX_REC_E_NAME);

    /* a name that would not fit the caption limit is refused up front */
    memset(&rec, 0, sizeof rec);
    rec.version = 1;
    rec.kind = 'd';
    set_str(rec.object_id, sizeof rec.object_id, O1);
    set_str(rec.parent, sizeof rec.parent, "root");
    set_str(rec.name, sizeof rec.name, "folder");
    rec.rev = 1;
    assert(tbox_record_build(&rec, out, sizeof out, 10) == TBOX_REC_E_TOO_LONG);
    printf("record: build refuses bad names and oversized records\n");
}

int main(void) {
    test_parse_minimal_file();
    test_parse_folder_without_size();
    test_parse_all_optional_fields();
    test_unknown_keys_are_ignored();

    test_not_a_record();
    test_bad_syntax();
    test_bad_version();
    test_missing_required();
    test_wrong_types();
    test_bad_ids();
    test_ranges();

    test_good_names();
    test_forbidden_characters();
    test_path_tricks();
    test_trailing_and_reserved();
    test_name_too_long();
    test_sanitize_salvages();

    test_build_is_deterministic();
    test_build_key_order_with_optionals();
    test_roundtrip();
    test_build_refuses_bad_input();

    printf("record OK\n");
    return 0;
}

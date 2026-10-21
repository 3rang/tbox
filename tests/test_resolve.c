/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_resolve.c — the resolver of spec 7.4, table-driven.
 *
 * Each case builds a small set of messages in memory, resolves them, and
 * checks the resulting objects, Inbox and findings. The properties that must
 * hold are the spec's invariants: highest revision wins, ties become conflict
 * items, a broken tree lands in Lost & Found, trash hides children, and the
 * Inbox excludes adopted sources.
 */
#include "catalog/resolve.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- builders -------------------------------------------------------- */

/* A record with the fields the resolver reads. Ids are 32 hex spelled out
 * from a short seed so each case reads clearly. */
static void mk_record(TBoxRecord *rec, char kind, const char *o,
                      const char *p, const char *name, long long r,
                      int has_size, long long size) {
    memset(rec, 0, sizeof *rec);
    rec->version = 1;
    rec->kind = kind;
    snprintf(rec->object_id, sizeof rec->object_id, "%s", o);
    snprintf(rec->parent, sizeof rec->parent, "%s", p);
    snprintf(rec->name, sizeof rec->name, "%s", name);
    rec->rev = r;
    rec->has_size = has_size;
    rec->size = size;
}

static TBoxResolveMsg mk_file(const char *msg, const char *o, const char *p,
                              const char *name, long long r, long long size) {
    TBoxResolveMsg m;
    memset(&m, 0, sizeof m);
    m.message_id = msg;
    m.kind = TBOX_MSG_FILE;
    m.date = 1760000000;
    m.orig_name = name;
    m.mime = "application/octet-stream";
    m.has_size = 1;
    m.size = size;
    mk_record(&m.rec, 'f', o, p, name, r, 1, size);
    m.rec.has_sha256 = 0;
    return m;
}

static TBoxResolveMsg mk_folder(const char *msg, const char *o, const char *p,
                                const char *name, long long r) {
    TBoxResolveMsg m;
    memset(&m, 0, sizeof m);
    m.message_id = msg;
    m.kind = TBOX_MSG_FOLDER;
    m.date = 1760000000;
    mk_record(&m.rec, 'd', o, p, name, r, 0, 0);
    return m;
}

static TBoxResolveMsg mk_unmanaged(const char *msg, const char *name,
                                   long long size) {
    TBoxResolveMsg m;
    memset(&m, 0, sizeof m);
    m.message_id = msg;
    m.kind = TBOX_MSG_UNMANAGED;
    m.date = 1760000000;
    m.orig_name = name;
    m.mime = "text/plain";
    m.has_size = 1;
    m.size = size;
    return m;
}

static TBoxResolveMsg mk_note(const char *msg) {
    TBoxResolveMsg m;
    memset(&m, 0, sizeof m);
    m.message_id = msg;
    m.kind = TBOX_MSG_NOTE;
    m.date = 1760000000;
    return m;
}

/* ---- assertions ------------------------------------------------------ */

/* Ids used across the cases (32 hex each). */
#define O_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define O_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define O_C "cccccccccccccccccccccccccccccccc"
#define O_D "dddddddddddddddddddddddddddddddd"
#define O_E "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
#define MISSING "99999999999999999999999999999999"

static TBoxResolveResult run(const TBoxResolveMsg *msgs, size_t n) {
    TBoxResolveResult res;
    int rc = tbox_resolve(msgs, n, &res);
    assert(rc == 0);
    return res;
}

static const TBoxObject *find_obj(const TBoxResolveResult *r, const char *id) {
    for (size_t i = 0; i < r->object_count; i++) {
        if (strcmp(r->objects[i].object_id, id) == 0) {
            return &r->objects[i];
        }
    }
    return NULL;
}

static int count_findings(const TBoxResolveResult *r, TBoxFindingCode code) {
    int n = 0;
    for (size_t i = 0; i < r->finding_count; i++) {
        if (r->findings[i].code == code) {
            n++;
        }
    }
    return n;
}

/* ---- cases ----------------------------------------------------------- */

static void test_empty(void) {
    TBoxResolveResult r = run(NULL, 0);
    assert(r.object_count == 0 && r.inbox_count == 0 && r.finding_count == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: empty input is empty output\n");
}

static void test_single_file_at_root(void) {
    TBoxResolveMsg m = mk_file("1", O_A, "root", "a.txt", 1, 10);
    TBoxResolveResult r = run(&m, 1);
    assert(r.object_count == 1);
    const TBoxObject *o = find_obj(&r, O_A);
    assert(o != NULL);
    assert(o->kind == 'f');
    assert(strcmp(o->parent_id, "root") == 0);
    assert(strcmp(o->name, "a.txt") == 0);
    assert(o->rev == 1 && o->has_size && o->size == 10);
    assert(!o->has_trashed && !o->conflict);
    assert(r.inbox_count == 0 && count_findings(&r, TBOX_FIND_LOST_PARENT) == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: a lone file sits at root\n");
}

static void test_single_folder_at_root(void) {
    TBoxResolveMsg m = mk_folder("1", O_A, "root", "docs", 1);
    TBoxResolveResult r = run(&m, 1);
    const TBoxObject *o = find_obj(&r, O_A);
    assert(o != NULL && o->kind == 'd' && !o->has_size);
    tbox_resolve_result_free(&r);
    printf("resolve: a lone folder sits at root\n");
}

static void test_file_in_folder(void) {
    TBoxResolveMsg msgs[2] = {
        mk_folder("1", O_A, "root", "docs", 1),
        mk_file("2", O_B, O_A, "a.txt", 1, 10)
    };
    TBoxResolveResult r = run(msgs, 2);
    const TBoxObject *f = find_obj(&r, O_B);
    assert(f != NULL && strcmp(f->parent_id, O_A) == 0);
    assert(count_findings(&r, TBOX_FIND_LOST_PARENT) == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: a file resolves into its folder\n");
}

static void test_duplicate_names_are_kept(void) {
    TBoxResolveMsg msgs[2] = {
        mk_file("1", O_A, "root", "same.txt", 1, 1),
        mk_file("2", O_B, "root", "same.txt", 1, 2)
    };
    TBoxResolveResult r = run(msgs, 2);
    assert(r.object_count == 2); /* both kept */
    assert(count_findings(&r, TBOX_FIND_DUPLICATE_NAME) == 2);
    tbox_resolve_result_free(&r);
    printf("resolve: two live items may share a name (reported)\n");
}

static void test_duplicate_is_case_insensitive(void) {
    TBoxResolveMsg msgs[2] = {
        mk_file("1", O_A, "root", "Report.pdf", 1, 1),
        mk_file("2", O_B, "root", "report.PDF", 1, 2)
    };
    TBoxResolveResult r = run(msgs, 2);
    assert(count_findings(&r, TBOX_FIND_DUPLICATE_NAME) == 2);
    tbox_resolve_result_free(&r);
    printf("resolve: duplicate names are compared case-insensitively\n");
}

static void test_same_name_different_folders(void) {
    TBoxResolveMsg msgs[4] = {
        mk_folder("1", O_C, "root", "one", 1),
        mk_folder("2", O_D, "root", "two", 1),
        mk_file("3", O_A, O_C, "same.txt", 1, 1),
        mk_file("4", O_B, O_D, "same.txt", 1, 1)
    };
    TBoxResolveResult r = run(msgs, 4);
    assert(count_findings(&r, TBOX_FIND_DUPLICATE_NAME) == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: the same name in different folders is fine\n");
}

static void test_missing_parent_goes_lost(void) {
    TBoxResolveMsg m = mk_file("1", O_A, MISSING, "a.txt", 1, 1);
    TBoxResolveResult r = run(&m, 1);
    const TBoxObject *o = find_obj(&r, O_A);
    assert(o != NULL && strcmp(o->parent_id, "lost") == 0);
    assert(count_findings(&r, TBOX_FIND_LOST_PARENT) == 1);
    tbox_resolve_result_free(&r);
    printf("resolve: a missing parent lands in Lost & Found\n");
}

static void test_file_parent_goes_lost(void) {
    TBoxResolveMsg msgs[2] = {
        mk_file("1", O_A, "root", "a.txt", 1, 1),
        mk_folder("2", O_B, O_A, "child", 1) /* parent is a file, not a folder */
    };
    TBoxResolveResult r = run(msgs, 2);
    const TBoxObject *o = find_obj(&r, O_B);
    assert(o != NULL && strcmp(o->parent_id, "lost") == 0);
    assert(count_findings(&r, TBOX_FIND_LOST_PARENT) == 1);
    tbox_resolve_result_free(&r);
    printf("resolve: a non-folder parent lands in Lost & Found\n");
}

static void test_self_parent_cycle(void) {
    TBoxResolveMsg m = mk_folder("1", O_A, O_A, "loop", 1);
    TBoxResolveResult r = run(&m, 1);
    const TBoxObject *o = find_obj(&r, O_A);
    assert(o != NULL && strcmp(o->parent_id, "lost") == 0);
    assert(count_findings(&r, TBOX_FIND_CYCLE) == 1);
    tbox_resolve_result_free(&r);
    printf("resolve: a self-parent is a cycle\n");
}

static void test_two_node_cycle(void) {
    TBoxResolveMsg msgs[2] = {
        mk_folder("1", O_A, O_B, "a", 1),
        mk_folder("2", O_B, O_A, "b", 1)
    };
    TBoxResolveResult r = run(msgs, 2);
    assert(strcmp(find_obj(&r, O_A)->parent_id, "lost") == 0);
    assert(strcmp(find_obj(&r, O_B)->parent_id, "lost") == 0);
    assert(count_findings(&r, TBOX_FIND_CYCLE) == 2);
    tbox_resolve_result_free(&r);
    printf("resolve: a two-node cycle goes to Lost & Found\n");
}

static void test_three_node_cycle(void) {
    TBoxResolveMsg msgs[3] = {
        mk_folder("1", O_A, O_C, "a", 1),
        mk_folder("2", O_B, O_A, "b", 1),
        mk_folder("3", O_C, O_B, "c", 1)
    };
    TBoxResolveResult r = run(msgs, 3);
    assert(count_findings(&r, TBOX_FIND_CYCLE) == 3);
    tbox_resolve_result_free(&r);
    printf("resolve: a three-node cycle goes to Lost & Found\n");
}

static void test_trashed_item(void) {
    TBoxResolveMsg m = mk_file("1", O_A, "root", "a.txt", 1, 1);
    m.rec.has_trashed = 1;
    m.rec.trashed_at = 1760000001;
    TBoxResolveResult r = run(&m, 1);
    const TBoxObject *o = find_obj(&r, O_A);
    assert(o != NULL && strcmp(o->parent_id, "trash") == 0);
    assert(o->has_trashed && o->trashed_at == 1760000001);
    tbox_resolve_result_free(&r);
    printf("resolve: a trashed item lives in Trash\n");
}

static void test_trashed_folder_hides_child(void) {
    TBoxResolveMsg msgs[2] = {
        mk_folder("1", O_A, "root", "docs", 1),
        mk_file("2", O_B, O_A, "a.txt", 1, 1)
    };
    msgs[0].rec.has_trashed = 1;
    msgs[0].rec.trashed_at = 1760000001;
    TBoxResolveResult r = run(msgs, 2);
    const TBoxObject *f = find_obj(&r, O_A);
    const TBoxObject *c = find_obj(&r, O_B);
    assert(strcmp(f->parent_id, "trash") == 0);
    assert(strcmp(c->parent_id, "trash") == 0); /* hidden with its parent */
    assert(c->has_trashed);
    tbox_resolve_result_free(&r);
    printf("resolve: a trashed folder hides its child in Trash\n");
}

static void test_trashed_child_of_live_folder(void) {
    TBoxResolveMsg msgs[2] = {
        mk_folder("1", O_A, "root", "docs", 1),
        mk_file("2", O_B, O_A, "gone.txt", 1, 1)
    };
    msgs[1].rec.has_trashed = 1;
    msgs[1].rec.trashed_at = 1760000002;
    TBoxResolveResult r = run(msgs, 2);
    assert(strcmp(find_obj(&r, O_A)->parent_id, "root") == 0);
    assert(strcmp(find_obj(&r, O_B)->parent_id, "trash") == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: a trashed child leaves the live folder\n");
}

static void test_folder_under_lost_folder(void) {
    TBoxResolveMsg msgs[2] = {
        mk_folder("1", O_A, MISSING, "orphan", 1),
        mk_file("2", O_B, O_A, "a.txt", 1, 1)
    };
    TBoxResolveResult r = run(msgs, 2);
    assert(strcmp(find_obj(&r, O_A)->parent_id, "lost") == 0);
    assert(strcmp(find_obj(&r, O_B)->parent_id, "lost") == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: a child of a lost folder is lost too\n");
}

static void test_inbox_holds_unmanaged(void) {
    TBoxResolveMsg m = mk_unmanaged("5", "photo.jpg", 1234);
    TBoxResolveResult r = run(&m, 1);
    assert(r.object_count == 0);
    assert(r.inbox_count == 1);
    assert(strcmp(r.inbox[0].message_id, "5") == 0);
    assert(strcmp(r.inbox[0].orig_name, "photo.jpg") == 0);
    assert(r.inbox[0].has_size && r.inbox[0].size == 1234);
    tbox_resolve_result_free(&r);
    printf("resolve: an unmanaged file is in the Inbox\n");
}

static void test_adopted_source_leaves_inbox(void) {
    /* message 5 is adopted: a managed record carries src=5 */
    TBoxResolveMsg msgs[2] = {
        mk_unmanaged("5", "photo.jpg", 1234),
        mk_file("6", O_A, "root", "photo.jpg", 1, 1234)
    };
    msgs[1].rec.has_src = 1;
    snprintf(msgs[1].rec.src, sizeof msgs[1].rec.src, "5");
    TBoxResolveResult r = run(msgs, 2);
    assert(r.inbox_count == 0); /* the Inbox item vanished */
    assert(r.object_count == 1);
    tbox_resolve_result_free(&r);
    printf("resolve: an adopted source leaves the Inbox\n");
}

static void test_note_and_other_ignored(void) {
    TBoxResolveMsg msgs[2] = {
        mk_note("1"),
        mk_note("2")
    };
    msgs[1].kind = TBOX_MSG_OTHER;
    TBoxResolveResult r = run(msgs, 2);
    assert(r.object_count == 0 && r.inbox_count == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: notes and other messages are not catalog items\n");
}

static void test_bad_record_is_reported(void) {
    TBoxResolveMsg m = mk_unmanaged("7", "broken.bin", 10);
    m.bad_record = 1;
    TBoxResolveResult r = run(&m, 1);
    assert(count_findings(&r, TBOX_FIND_BAD_RECORD) == 1);
    assert(r.inbox_count == 1); /* still shown in the Inbox */
    tbox_resolve_result_free(&r);
    printf("resolve: a bad record is reported and kept in the Inbox\n");
}

static void test_conflict_tie(void) {
    TBoxResolveMsg msgs[2] = {
        mk_file("10", O_A, "root", "a.txt", 3, 1),
        mk_file("20", O_A, "root", "a.txt", 3, 2) /* same r: a tie */
    };
    TBoxResolveResult r = run(msgs, 2);
    /* the winner keeps the real id; the loser is a conflict item */
    assert(r.object_count == 2);
    const TBoxObject *win = find_obj(&r, O_A);
    assert(win != NULL && !win->conflict);
    assert(strcmp(win->message_id, "20") == 0); /* higher message id wins */
    const TBoxObject *lose = find_obj(&r, O_A "-10");
    assert(lose != NULL && lose->conflict);
    assert(strcmp(lose->name, "a.txt (conflict 10)") == 0);
    assert(strcmp(lose->parent_id, "root") == 0);
    assert(count_findings(&r, TBOX_FIND_CONFLICT) == 1);
    tbox_resolve_result_free(&r);
    printf("resolve: a tie becomes a conflict item\n");
}

static void test_older_revision_is_not_a_conflict(void) {
    TBoxResolveMsg msgs[2] = {
        mk_file("10", O_A, "root", "a.txt", 1, 1),
        mk_file("20", O_A, "root", "a.txt", 2, 2) /* higher r wins cleanly */
    };
    TBoxResolveResult r = run(msgs, 2);
    assert(r.object_count == 1);
    const TBoxObject *o = find_obj(&r, O_A);
    assert(o->rev == 2 && strcmp(o->message_id, "20") == 0);
    assert(count_findings(&r, TBOX_FIND_CONFLICT) == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: a lower revision is simply superseded\n");
}

static void test_kind_mismatch(void) {
    TBoxResolveMsg msgs[2] = {
        mk_folder("10", O_A, "root", "thing", 1),
        mk_file("20", O_A, "root", "thing", 2, 5) /* same o, different k */
    };
    TBoxResolveResult r = run(msgs, 2);
    assert(count_findings(&r, TBOX_FIND_KIND_MISMATCH) == 1);
    const TBoxObject *o = find_obj(&r, O_A);
    assert(o->rev == 2 && o->kind == 'f'); /* keep the higher r */
    tbox_resolve_result_free(&r);
    printf("resolve: a kind mismatch is reported, higher r kept\n");
}

static void test_deterministic_order(void) {
    TBoxResolveMsg msgs[3] = {
        mk_unmanaged("5", "c.bin", 1),
        mk_unmanaged("9", "a.bin", 1),
        mk_unmanaged("7", "b.bin", 1)
    };
    TBoxResolveResult r = run(msgs, 3);
    assert(r.inbox_count == 3);
    /* Inbox is ordered by message id descending */
    assert(strcmp(r.inbox[0].message_id, "9") == 0);
    assert(strcmp(r.inbox[1].message_id, "7") == 0);
    assert(strcmp(r.inbox[2].message_id, "5") == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: the Inbox is ordered by message id descending\n");
}

static void test_objects_sorted_by_id(void) {
    TBoxResolveMsg msgs[3] = {
        mk_file("1", O_C, "root", "c", 1, 1),
        mk_file("2", O_A, "root", "a", 1, 1),
        mk_file("3", O_B, "root", "b", 1, 1)
    };
    TBoxResolveResult r = run(msgs, 3);
    assert(strcmp(r.objects[0].object_id, O_A) == 0);
    assert(strcmp(r.objects[1].object_id, O_B) == 0);
    assert(strcmp(r.objects[2].object_id, O_C) == 0);
    tbox_resolve_result_free(&r);
    printf("resolve: objects are ordered by object id\n");
}

static void test_flat_is_a_pure_function(void) {
    /* Resolving the same set in a different message order must give the same
     * catalog: this is the invariant 7.4 hangs on. */
    TBoxResolveMsg a[3] = {
        mk_folder("1", O_A, "root", "docs", 1),
        mk_file("2", O_B, O_A, "a.txt", 1, 1),
        mk_unmanaged("3", "raw.bin", 2)
    };
    TBoxResolveMsg b[3] = { a[2], a[0], a[1] };
    TBoxResolveResult ra = run(a, 3);
    TBoxResolveResult rb = run(b, 3);
    assert(ra.object_count == rb.object_count);
    assert(ra.inbox_count == rb.inbox_count);
    for (size_t i = 0; i < ra.object_count; i++) {
        assert(strcmp(ra.objects[i].object_id, rb.objects[i].object_id) == 0);
        assert(strcmp(ra.objects[i].parent_id, rb.objects[i].parent_id) == 0);
        assert(strcmp(ra.objects[i].name, rb.objects[i].name) == 0);
        assert(ra.objects[i].rev == rb.objects[i].rev);
    }
    tbox_resolve_result_free(&ra);
    tbox_resolve_result_free(&rb);
    printf("resolve: input order does not change the result\n");
}

static void test_free_is_idempotent(void) {
    TBoxResolveMsg m = mk_file("1", O_A, "root", "a.txt", 1, 1);
    TBoxResolveResult r = run(&m, 1);
    tbox_resolve_result_free(&r);
    tbox_resolve_result_free(&r); /* second free is a no-op */
    printf("resolve: freeing twice is safe\n");
}

static void test_finding_code_names(void) {
    assert(strcmp(tbox_finding_code_name(TBOX_FIND_BAD_RECORD), "BAD_RECORD") == 0);
    assert(strcmp(tbox_finding_code_name(TBOX_FIND_CONFLICT), "CONFLICT") == 0);
    assert(strcmp(tbox_finding_code_name(TBOX_FIND_DUPLICATE_NAME),
                  "DUPLICATE_NAME") == 0);
    assert(strcmp(tbox_finding_code_name(TBOX_FIND_LOST_PARENT),
                  "LOST_PARENT") == 0);
    assert(strcmp(tbox_finding_code_name(TBOX_FIND_CYCLE), "CYCLE") == 0);
    assert(strcmp(tbox_finding_code_name(TBOX_FIND_KIND_MISMATCH),
                  "KIND_MISMATCH") == 0);
    printf("resolve: finding codes match the spec's tokens\n");
}

int main(void) {
    test_empty();
    test_single_file_at_root();
    test_single_folder_at_root();
    test_file_in_folder();
    test_duplicate_names_are_kept();
    test_duplicate_is_case_insensitive();
    test_same_name_different_folders();
    test_missing_parent_goes_lost();
    test_file_parent_goes_lost();
    test_self_parent_cycle();
    test_two_node_cycle();
    test_three_node_cycle();
    test_trashed_item();
    test_trashed_folder_hides_child();
    test_trashed_child_of_live_folder();
    test_folder_under_lost_folder();
    test_inbox_holds_unmanaged();
    test_adopted_source_leaves_inbox();
    test_note_and_other_ignored();
    test_bad_record_is_reported();
    test_conflict_tie();
    test_older_revision_is_not_a_conflict();
    test_kind_mismatch();
    test_deterministic_order();
    test_objects_sorted_by_id();
    test_flat_is_a_pure_function();
    test_free_is_idempotent();
    test_finding_code_names();
    printf("resolve OK\n");
    return 0;
}

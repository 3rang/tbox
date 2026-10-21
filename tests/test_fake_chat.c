/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_fake_chat.c — the in-memory Saved Messages chat the sync tests rely on.
 *
 * If the fake is wrong, every sync test built on it is wrong, so it gets its
 * own checks: newest-first paging below a cursor, pages shorter than the
 * limit, getMessage, caption edits and text edits queueing the right update,
 * and permanent versus cache deletes behaving differently.
 */
#include "fake_chat.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* Reads the nth message id out of a messages reply. */
static long long id_at(const char *json, int n) {
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    cJSON *msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
    assert(cJSON_IsArray(msgs));
    cJSON *m = cJSON_GetArrayItem(msgs, n);
    assert(m != NULL);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
    long long v = (long long)id->valuedouble;
    cJSON_Delete(root);
    return v;
}

static int count_at(const char *json) {
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    cJSON *msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
    int n = cJSON_GetArraySize(msgs);
    cJSON_Delete(root);
    return n;
}

static void test_add_and_order(void) {
    FakeChat *c = fake_chat_new();
    long long a = fake_chat_add_media(c, "document", "a.bin", 10, 100,
                                      "tbx1 {}");
    long long b = fake_chat_add_text(c, "hello");
    long long d = fake_chat_add_media(c, "photo", "p.jpg", 20, 200, NULL);
    assert(a == 1 && b == 2 && d == 3);
    assert(fake_chat_count(c) == 3);
    /* stored newest first */
    char *h = fake_chat_history_json(c, 0, 0, 100);
    assert(id_at(h, 0) == 3);
    assert(id_at(h, 1) == 2);
    assert(id_at(h, 2) == 1);
    free(h);

    long long ids[8];
    assert(fake_chat_ids(c, ids, 8) == 3);
    assert(ids[0] == 3 && ids[2] == 1);
    fake_chat_free(c);
    printf("fake: messages come back newest first\n");
}

static void test_paging_cursor(void) {
    FakeChat *c = fake_chat_new();
    for (int i = 0; i < 10; i++) {
        fake_chat_add_media(c, "document", "f.bin", i, i, NULL);
    }
    /* first page of 3 from the top: ids 10,9,8 */
    char *p1 = fake_chat_history_json(c, 0, 0, 3);
    assert(count_at(p1) == 3);
    assert(id_at(p1, 0) == 10);
    assert(id_at(p1, 2) == 8);
    free(p1);

    /* next page from the smallest id of the previous page: 7,6,5 */
    char *p2 = fake_chat_history_json(c, 8, 0, 3);
    assert(count_at(p2) == 3);
    assert(id_at(p2, 0) == 7);
    assert(id_at(p2, 2) == 5);
    free(p2);

    /* walk to the end: a page below id 1 is empty */
    char *empty = fake_chat_history_json(c, 1, 0, 100);
    assert(count_at(empty) == 0);
    free(empty);
    fake_chat_free(c);
    printf("fake: history pages follow the message-id cursor\n");
}

static void test_short_pages(void) {
    FakeChat *c = fake_chat_new();
    for (int i = 0; i < 10; i++) {
        fake_chat_add_media(c, "document", "f.bin", i, i, NULL);
    }
    /* TDLib is allowed to return fewer than `limit`; the scanner must keep
     * going until a page is truly empty. Force pages of 2 even when limit=100 */
    fake_chat_set_page_size(c, 2);
    long long cursor = 0;
    int total = 0;
    for (;;) {
        char *p = fake_chat_history_json(c, cursor, 0, 100);
        int n = count_at(p);
        if (n == 0) {
            free(p);
            break;
        }
        assert(n <= 2); /* never over the cap */
        cursor = id_at(p, n - 1);
        total += n;
        free(p);
        assert(total <= 10);
    }
    assert(total == 10); /* every message seen exactly once */
    fake_chat_set_page_size(c, 0);
    fake_chat_free(c);
    printf("fake: short pages still reach every message\n");
}

static void test_get_message(void) {
    FakeChat *c = fake_chat_new();
    long long id = fake_chat_add_media(c, "document", "report.pdf", 1234, 555,
                                       "tbx1 {\"v\":1}");
    char *j = fake_chat_message_json(c, id);
    assert(j != NULL);
    cJSON *root = cJSON_Parse(j);
    assert(cJSON_IsObject(root));
    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(content, "@type");
    assert(strcmp(cJSON_GetStringValue(type), "messageDocument") == 0);
    cJSON *doc = cJSON_GetObjectItemCaseSensitive(content, "document");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(doc, "file_name");
    assert(strcmp(cJSON_GetStringValue(name), "report.pdf") == 0);
    cJSON *cap = cJSON_GetObjectItemCaseSensitive(content, "caption");
    cJSON *capText = cJSON_GetObjectItemCaseSensitive(cap, "text");
    assert(strcmp(cJSON_GetStringValue(capText), "tbx1 {\"v\":1}") == 0);
    cJSON_Delete(root);
    free(j);

    assert(fake_chat_message_json(c, 999) == NULL);
    fake_chat_free(c);
    printf("fake: getMessage returns the stored content\n");
}

static void test_text_message_shape(void) {
    FakeChat *c = fake_chat_new();
    long long id = fake_chat_add_text(c, "tbx1 {\"k\":\"d\"}");
    char *j = fake_chat_message_json(c, id);
    cJSON *root = cJSON_Parse(j);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(content, "@type");
    assert(strcmp(cJSON_GetStringValue(type), "messageText") == 0);
    cJSON_Delete(root);
    free(j);
    fake_chat_free(c);
    printf("fake: a text message is messageText\n");
}

static void test_edit_queues_update(void) {
    FakeChat *c = fake_chat_new();
    long long id = fake_chat_add_media(c, "document", "a.bin", 1, 1, "old");
    fake_chat_edit_caption(c, id, "new");
    const FakeMsg *m = fake_chat_get(c, id);
    assert(strcmp(m->caption, "new") == 0);

    char *u = fake_chat_take_update(c);
    assert(u != NULL);
    cJSON *root = cJSON_Parse(u);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "@type");
    assert(strcmp(cJSON_GetStringValue(type), "updateMessageContent") == 0);
    cJSON_Delete(root);
    free(u);
    assert(fake_chat_take_update(c) == NULL); /* queue drained */

    /* a text edit is updateMessageEdited */
    long long t = fake_chat_add_text(c, "note");
    fake_chat_edit_caption(c, t, "changed");
    char *u2 = fake_chat_take_update(c);
    cJSON *r2 = cJSON_Parse(u2);
    cJSON *t2 = cJSON_GetObjectItemCaseSensitive(r2, "@type");
    assert(strcmp(cJSON_GetStringValue(t2), "updateMessageEdited") == 0);
    cJSON_Delete(r2);
    free(u2);
    fake_chat_free(c);
    printf("fake: edits queue the right update\n");
}

static void test_permanent_vs_cache_delete(void) {
    FakeChat *c = fake_chat_new();
    long long keep = fake_chat_add_media(c, "document", "keep.bin", 1, 1, NULL);
    long long gone = fake_chat_add_media(c, "document", "gone.bin", 1, 2, NULL);

    /* a cache eviction must not remove the message */
    fake_chat_delete(c, keep, /*permanent=*/0);
    assert(fake_chat_get(c, keep) != NULL);
    char *u = fake_chat_take_update(c);
    cJSON *r = cJSON_Parse(u);
    cJSON *from_cache = cJSON_GetObjectItemCaseSensitive(r, "from_cache");
    assert(cJSON_IsTrue(from_cache));
    cJSON *perm = cJSON_GetObjectItemCaseSensitive(r, "is_permanent");
    assert(cJSON_IsFalse(perm));
    cJSON_Delete(r);
    free(u);

    /* a permanent delete removes it */
    fake_chat_delete(c, gone, /*permanent=*/1);
    assert(fake_chat_get(c, gone) == NULL);
    assert(fake_chat_count(c) == 1);
    char *u2 = fake_chat_take_update(c);
    cJSON *r2 = cJSON_Parse(u2);
    cJSON *perm2 = cJSON_GetObjectItemCaseSensitive(r2, "is_permanent");
    assert(cJSON_IsTrue(perm2));
    cJSON_Delete(r2);
    free(u2);

    fake_chat_free(c);
    printf("fake: cache eviction is ignored, permanent delete removes\n");
}

static void test_photo_has_photo_field(void) {
    FakeChat *c = fake_chat_new();
    long long id = fake_chat_add_media(c, "photo", "p.jpg", 99, 7, "cap");
    char *j = fake_chat_message_json(c, id);
    cJSON *root = cJSON_Parse(j);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    cJSON *photo = cJSON_GetObjectItemCaseSensitive(content, "photo");
    assert(photo != NULL);
    cJSON *file = cJSON_GetObjectItemCaseSensitive(photo, "photo");
    assert(file != NULL);
    cJSON_Delete(root);
    free(j);
    fake_chat_free(c);
    printf("fake: a photo carries a photo/file object\n");
}

int main(void) {
    test_add_and_order();
    test_paging_cursor();
    test_short_pages();
    test_get_message();
    test_text_message_shape();
    test_edit_queues_update();
    test_permanent_vs_cache_delete();
    test_photo_has_photo_field();
    printf("fake_chat OK\n");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_message.c — turning a TDLib message into a `messages` row (spec 7.1,
 * 7.2). Table-driven over the content types that matter and the record rules.
 */
#include "catalog/message.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define O1 "0123456789abcdef0123456789abcdef"

/* Parses a JSON string into a message row and returns the parse code. The
 * tree is deleted before returning, so callers must copy out what they need
 * (or use expect_* helpers that do). */
static int parse(const char *json, TBoxMessageRow *row) {
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    int rc = tbox_message_parse(root, row);
    cJSON_Delete(root);
    return rc;
}

static void test_document_with_record(void) {
    const char *json =
        "{\"@type\":\"message\",\"id\":42,\"date\":1760000000,"
        "\"content\":{\"@type\":\"messageDocument\",\"caption\":{\"@type\":"
        "\"formattedText\",\"text\":\"tbx1 {\\\"v\\\":1,\\\"k\\\":\\\"f\\\","
        "\\\"o\\\":\\\"" O1 "\\\",\\\"p\\\":\\\"root\\\",\\\"n\\\":\\\"a.txt\\\","
        "\\\"r\\\":1,\\\"s\\\":1234}\"},"
        "\"document\":{\"@type\":\"document\",\"file_name\":\"a.txt\","
        "\"mime_type\":\"text/plain\",\"document\":{\"@type\":\"file\","
        "\"id\":7,\"size\":1234,\"remote\":{\"@type\":\"remoteFile\","
        "\"id\":99}}}}}";
    TBoxMessageRow row;
    assert(parse(json, &row) == 1);
    assert(strcmp(row.message_id, "42") == 0);
    assert(row.date == 1760000000);
    assert(strcmp(row.kind, "file") == 0);
    assert(row.has_record && row.rec.kind == 'f');
    assert(strcmp(row.rec.name, "a.txt") == 0);
    assert(row.rec.rev == 1 && row.rec.has_size && row.rec.size == 1234);
    assert(row.has_file_id && row.file_id == 7);
    assert(row.has_remote_id && row.remote_id == 99);
    assert(row.has_size && row.size == 1234);
    assert(strcmp(row.orig_name, "a.txt") == 0);
    assert(strcmp(row.mime, "text/plain") == 0);
    assert(!row.bad_record);
    tbox_message_row_free(&row);
    printf("message: a documented file with a record parses\n");
}

static void test_document_without_record(void) {
    const char *json =
        "{\"@type\":\"message\",\"id\":5,\"date\":1,"
        "\"content\":{\"@type\":\"messageDocument\","
        "\"document\":{\"@type\":\"document\",\"file_name\":\"raw.bin\","
        "\"mime_type\":\"application/octet-stream\",\"document\":{\"@type\":"
        "\"file\",\"id\":3,\"size\":10,\"remote\":{\"@type\":\"remoteFile\","
        "\"id\":4}}}}}";
    TBoxMessageRow row;
    assert(parse(json, &row) == 1);
    assert(strcmp(row.kind, "unmanaged") == 0);
    assert(!row.has_record);
    assert(strcmp(row.orig_name, "raw.bin") == 0);
    assert(row.has_size && row.size == 10);
    tbox_message_row_free(&row);
    printf("message: a file without a record is unmanaged\n");
}

static void test_text_folder_record(void) {
    const char *json =
        "{\"@type\":\"message\",\"id\":9,\"date\":2,"
        "\"content\":{\"@type\":\"messageText\",\"text\":{\"@type\":"
        "\"formattedText\",\"text\":\"tbx1 {\\\"v\\\":1,\\\"k\\\":\\\"d\\\","
        "\\\"o\\\":\\\"" O1 "\\\",\\\"p\\\":\\\"root\\\",\\\"n\\\":\\\"docs\\\","
        "\\\"r\\\":1}\"}}}";
    TBoxMessageRow row;
    assert(parse(json, &row) == 1);
    assert(strcmp(row.kind, "folder") == 0);
    assert(row.has_record && row.rec.kind == 'd');
    assert(strcmp(row.rec.name, "docs") == 0);
    tbox_message_row_free(&row);
    printf("message: a text record is a folder\n");
}

static void test_text_note(void) {
    const char *json =
        "{\"@type\":\"message\",\"id\":11,\"date\":3,"
        "\"content\":{\"@type\":\"messageText\",\"text\":{\"@type\":"
        "\"formattedText\",\"text\":\"just a note\"}}}";
    TBoxMessageRow row;
    assert(parse(json, &row) == 1);
    assert(strcmp(row.kind, "note") == 0);
    assert(!row.has_record && !row.bad_record);
    assert(row.text != NULL && strcmp(row.text, "just a note") == 0);
    tbox_message_row_free(&row);
    printf("message: a text note is a note\n");
}

static void test_bad_record_on_file(void) {
    const char *json =
        "{\"@type\":\"message\",\"id\":13,\"date\":4,"
        "\"content\":{\"@type\":\"messageDocument\","
        "\"caption\":{\"@type\":\"formattedText\",\"text\":\"tbx1 {broken}\"},"
        "\"document\":{\"@type\":\"document\",\"file_name\":\"x.bin\","
        "\"document\":{\"@type\":\"file\",\"id\":1,\"size\":2,\"remote\":"
        "{\"@type\":\"remoteFile\",\"id\":2}}}}}";
    TBoxMessageRow row;
    assert(parse(json, &row) == 1);
    assert(strcmp(row.kind, "unmanaged") == 0); /* shown in the Inbox */
    assert(!row.has_record && row.bad_record);
    tbox_message_row_free(&row);
    printf("message: a broken caption is a bad_record unmanaged file\n");
}

static void test_bad_record_on_text(void) {
    const char *json =
        "{\"@type\":\"message\",\"id\":14,\"date\":4,"
        "\"content\":{\"@type\":\"messageText\",\"text\":{\"@type\":"
        "\"formattedText\",\"text\":\"tbx1 {broken}\"}}}";
    TBoxMessageRow row;
    assert(parse(json, &row) == 1);
    assert(strcmp(row.kind, "note") == 0);
    assert(row.bad_record);
    tbox_message_row_free(&row);
    printf("message: a broken text record is a bad_record note\n");
}

static void test_other_is_ignored(void) {
    const char *json =
        "{\"@type\":\"message\",\"id\":15,\"date\":5,"
        "\"content\":{\"@type\":\"messageSticker\",\"sticker\":{}}}";
    TBoxMessageRow row;
    assert(parse(json, &row) == 0);
    printf("message: a sticker is ignored\n");
}

static void test_photo_has_no_name(void) {
    const char *json =
        "{\"@type\":\"message\",\"id\":16,\"date\":6,"
        "\"content\":{\"@type\":\"messagePhoto\",\"caption\":{\"@type\":"
        "\"formattedText\",\"text\":\"tbx1 {\\\"v\\\":1,\\\"k\\\":\\\"f\\\","
        "\\\"o\\\":\\\"" O1 "\\\",\\\"p\\\":\\\"root\\\",\\\"n\\\":\\\"p.jpg\\\","
        "\\\"r\\\":1,\\\"s\\\":555}\"},"
        "\"photo\":{\"@type\":\"photo\",\"photo\":{\"@type\":\"file\","
        "\"id\":8,\"size\":555,\"remote\":{\"@type\":\"remoteFile\","
        "\"id\":12}}}}}";
    TBoxMessageRow row;
    assert(parse(json, &row) == 1);
    assert(strcmp(row.kind, "file") == 0); /* the caption names it */
    assert(row.has_file_id && row.file_id == 8);
    assert(row.has_size && row.size == 555);
    assert(row.orig_name[0] == '\0'); /* photos carry no file name */
    tbox_message_row_free(&row);
    printf("message: a photo file carries no original name\n");
}

static void test_malformed(void) {
    TBoxMessageRow row;
    assert(parse("{\"@type\":\"message\",\"date\":1}", &row) == -1); /* no id */
    assert(parse("{\"@type\":\"message\",\"id\":1}", &row) == 0);   /* no content */
    printf("message: a message without an id is rejected\n");
}

int main(void) {
    test_document_with_record();
    test_document_without_record();
    test_text_folder_record();
    test_text_note();
    test_bad_record_on_file();
    test_bad_record_on_text();
    test_other_is_ignored();
    test_photo_has_no_name();
    test_malformed();
    printf("message OK\n");
    return 0;
}

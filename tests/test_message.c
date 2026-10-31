/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_message.c - message display helpers.
 */
#include <ctype.h>

#include "check.h"
#include "tg/message.h"

static void text_of(const char *json, char *buf, size_t n) {
    cJSON *m = cJSON_Parse(json);
    CHECK(m != NULL);
    message_text(m, buf, n);
    cJSON_Delete(m);
}

static void test_text(void) {
    char buf[256];
    text_of("{\"content\":{\"@type\":\"messageText\",\"text\":{\"text\":\"hello\"}}}", buf, sizeof buf);
    CHECK_STR(buf, "hello");

    text_of("{\"content\":{\"@type\":\"messageDocument\",\"document\":{\"file_name\":\"a.pdf\"},"
            "\"caption\":{\"text\":\"the report\"}}}", buf, sizeof buf);
    CHECK_STR(buf, "[file a.pdf] the report");

    text_of("{\"content\":{\"@type\":\"messagePhoto\",\"caption\":{\"text\":\"\"}}}", buf, sizeof buf);
    CHECK_STR(buf, "[photo]");

    text_of("{\"content\":{\"@type\":\"messageSticker\",\"sticker\":{\"emoji\":\"X\"}}}", buf, sizeof buf);
    CHECK_STR(buf, "[sticker X]");

    text_of("{\"content\":{\"@type\":\"messageCall\"}}", buf, sizeof buf);
    CHECK_STR(buf, "[call]");

    text_of("{\"content\":{\"@type\":\"messageChatAddMembers\"}}", buf, sizeof buf);
    CHECK_STR(buf, "[chatAddMembers]");

    text_of("{}", buf, sizeof buf);
    CHECK_STR(buf, "[?]");
}

static void test_sender(void) {
    long long user, chat;
    cJSON *m = cJSON_Parse("{\"sender_id\":{\"@type\":\"messageSenderUser\",\"user_id\":7}}");
    message_sender(m, &user, &chat);
    CHECK(user == 7 && chat == 0);
    cJSON_Delete(m);

    m = cJSON_Parse("{\"sender_id\":{\"@type\":\"messageSenderChat\",\"chat_id\":-9}}");
    message_sender(m, &user, &chat);
    CHECK(user == 0 && chat == -9);
    cJSON_Delete(m);

    message_sender(NULL, &user, &chat);
    CHECK(user == 0 && chat == 0);
}

static void test_time(void) {
    char buf[32];
    message_time(1700000000, buf, sizeof buf);
    /* "YYYY-MM-DD HH:MM" in local time; check the shape, not the zone. */
    CHECK(strlen(buf) == 16);
    CHECK(buf[4] == '-' && buf[7] == '-' && buf[10] == ' ' && buf[13] == ':');
    CHECK(isdigit((unsigned char)buf[0]) && isdigit((unsigned char)buf[15]));
    CHECK(strncmp(buf, "2023-11-1", 9) == 0);
}

static void test_outgoing(void) {
    cJSON *m = cJSON_Parse("{\"is_outgoing\":true}");
    CHECK(message_is_outgoing(m) == 1);
    cJSON_Delete(m);
    CHECK(message_is_outgoing(NULL) == 0);
}

int main(void) {
    test_text();
    test_sender();
    test_time();
    test_outgoing();
    TEST_MAIN_END();
}

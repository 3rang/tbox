/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_outbox.c - sending and waiting for updateMessageSendSucceeded / Failed.
 */
#include "check.h"
#include "fake_td.h"
#include "tg/outbox.h"

/* sendMessage reply: the pending message with temporary id 55 (and, for
 * a document, the file id 9 inside the content). */
#define PENDING_TEXT "{\"@type\":\"message\",\"id\":55,\"content\":{\"@type\":\"messageText\"}}"
#define PENDING_DOC                                                                      \
    "{\"@type\":\"message\",\"id\":55,\"content\":{\"@type\":\"messageDocument\","     \
    "\"document\":{\"document\":{\"id\":9}}}}"
#define SENT(old, final) \
    "{\"@type\":\"updateMessageSendSucceeded\",\"old_message_id\":" #old ",\"message\":{\"id\":" #final "}}"

static cJSON *text_content(void) {
    cJSON *c = jreq("inputMessageText");
    cJSON *f = jreq("formattedText");
    cJSON_AddStringToObject(f, "text", "hi");
    cJSON_AddItemToObject(c, "text", f);
    return c;
}

static void test_success(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";

    fake_td_on("sendMessage", PENDING_TEXT);
    /* Noise, a result for a different message, then ours. */
    fake_td_push_when_idle("{\"@type\":\"updateChatLastMessage\"}");
    fake_td_push_when_idle(SENT(1, 1000));
    fake_td_push_when_idle(SENT(55, 2000));

    CHECK(outbox_send(&td, -100, text_content(), err, sizeof err) == 2000);
    cJSON *sent = fake_td_last_of("sendMessage");
    CHECK(jint(sent, "chat_id") == -100);
    CHECK(jis(jobj(sent, "input_message_content"), "inputMessageText"));
    CHECK(td.on_update == NULL);
}

static void test_failure(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";
    fake_td_on("sendMessage", PENDING_TEXT);
    fake_td_push_when_idle("{\"@type\":\"updateMessageSendFailed\",\"old_message_id\":55,"
                           "\"message\":{\"id\":55},\"error\":{\"code\":400,\"message\":\"USER_IS_BLOCKED\"}}");
    CHECK(outbox_send(&td, -100, text_content(), err, sizeof err) == 0);
    CHECK_STR(err, "USER_IS_BLOCKED");
}

static void test_rejected_request(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";
    fake_td_on("sendMessage", "{\"@type\":\"error\",\"code\":400,\"message\":\"Chat not found\"}");
    CHECK(outbox_send(&td, -100, text_content(), err, sizeof err) == 0);
    CHECK(strstr(err, "Chat not found") != NULL);
}

static void test_timeout(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";
    fake_td_on("sendMessage", PENDING_TEXT);
    CHECK(outbox_send(&td, -100, text_content(), err, sizeof err) == 0);
    CHECK(strstr(err, "timed out") != NULL);
}

static void test_upload_progress_does_not_disturb(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";
    fake_td_on("sendMessage", PENDING_DOC);
    fake_td_push_when_idle("{\"@type\":\"updateFile\",\"file\":{\"id\":9,\"expected_size\":100,"
                           "\"remote\":{\"uploaded_size\":50}}}");
    fake_td_push_when_idle("{\"@type\":\"updateFile\",\"file\":{\"id\":9,\"expected_size\":100,"
                           "\"remote\":{\"uploaded_size\":100}}}");
    fake_td_push_when_idle(SENT(55, 3000));
    CHECK(outbox_send(&td, -100, jreq("inputMessageDocument"), err, sizeof err) == 3000);
}

int main(void) {
    test_success();
    test_failure();
    test_rejected_request();
    test_timeout();
    test_upload_progress_does_not_disturb();
    TEST_MAIN_END();
}

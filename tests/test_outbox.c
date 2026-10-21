/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_outbox.c - waiting for updateMessageSendSucceeded / Failed.
 */
#include "check.h"
#include "fake_td.h"
#include "outbox.h"

static void test_success(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";

    /* Noise, then a result for a different message, then ours. */
    fake_td_push("{\"@type\":\"updateChatLastMessage\"}");
    fake_td_push("{\"@type\":\"updateMessageSendSucceeded\",\"old_message_id\":1,"
                 "\"message\":{\"id\":1000}}");
    fake_td_push("{\"@type\":\"updateMessageSendSucceeded\",\"old_message_id\":55,"
                 "\"message\":{\"id\":2000}}");
    CHECK(outbox_wait(&td, 55, 0, 5.0, err, sizeof err) == 2000);
    CHECK(td.on_update == NULL);
}

static void test_failure(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";
    fake_td_push("{\"@type\":\"updateMessageSendFailed\",\"old_message_id\":55,"
                 "\"message\":{\"id\":55},\"error\":{\"code\":400,\"message\":\"USER_IS_BLOCKED\"}}");
    CHECK(outbox_wait(&td, 55, 0, 5.0, err, sizeof err) == 0);
    CHECK_STR(err, "USER_IS_BLOCKED");

    fake_td_reset();
    td_init(&td, fake_td_api());
    fake_td_push("{\"@type\":\"updateMessageSendFailed\",\"old_message_id\":55,"
                 "\"message\":{\"id\":55},\"error_code\":400,\"error_message\":\"old style\"}");
    CHECK(outbox_wait(&td, 55, 0, 5.0, err, sizeof err) == 0);
    CHECK_STR(err, "old style");
}

static void test_timeout(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";
    CHECK(outbox_wait(&td, 55, 0, 1.0, err, sizeof err) == 0);
    CHECK(strstr(err, "timed out") != NULL);
}

static void test_progress_does_not_disturb(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[256] = "";
    fake_td_push("{\"@type\":\"updateFile\",\"file\":{\"id\":9,\"expected_size\":100,"
                 "\"remote\":{\"uploaded_size\":50}}}");
    fake_td_push("{\"@type\":\"updateFile\",\"file\":{\"id\":9,\"expected_size\":100,"
                 "\"remote\":{\"uploaded_size\":100}}}");
    fake_td_push("{\"@type\":\"updateMessageSendSucceeded\",\"old_message_id\":55,"
                 "\"message\":{\"id\":3000}}");
    CHECK(outbox_wait(&td, 55, 9, 5.0, err, sizeof err) == 3000);
}

int main(void) {
    test_success();
    test_failure();
    test_timeout();
    test_progress_does_not_disturb();
    TEST_MAIN_END();
}

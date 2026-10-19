/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_td.c - the request/reply bookkeeping of td.c against the fake.
 */
#include "check.h"
#include "fake_td.h"
#include "td.h"

static int updates_seen;
static char last_update_type[64];

static void on_update(Td *td, const cJSON *ev, void *user) {
    (void)td;
    (void)user;
    updates_seen++;
    snprintf(last_update_type, sizeof last_update_type, "%s", jtype(ev));
}

static void test_init_sets_verbosity(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    CHECK(td.client_id == 1);
    CHECK(strstr(fake_td_last_execute(), "setLogVerbosityLevel") != NULL);
    CHECK(!td.closed);
}

static void test_request_matches_extra_and_dispatches_updates(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    td_set_update_handler(&td, on_update, NULL);
    updates_seen = 0;

    /* An unrelated update sits in front of the reply. */
    fake_td_once("getMe", "{\"@type\":\"user\",\"id\":42}",
                 "{\"@type\":\"updateConnectionState\"}");

    cJSON *r = td_request(&td, jreq("getMe"), 1.0);
    CHECK(r != NULL);
    CHECK(jis(r, "user"));
    CHECK(jint(r, "id") == 42);
    CHECK(!td_is_error(r));
    cJSON_Delete(r);

    CHECK(updates_seen == 1);
    CHECK_STR(last_update_type, "updateConnectionState");

    cJSON *sent = fake_td_sent(0);
    CHECK(jis(sent, "getMe"));
    CHECK(strncmp(jstr(sent, "@extra"), "tbox-", 5) == 0);
}

static void test_request_without_reply_times_out(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    cJSON *r = td_request(&td, jreq("getMe"), 0.01);
    CHECK(r == NULL);
    CHECK(td_is_error(r));
    CHECK(strstr(td_error_message(r), "timeout") != NULL);
}

static void test_error_reply(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    fake_td_on("getChat", "{\"@type\":\"error\",\"code\":400,\"message\":\"Chat not found\"}");
    cJSON *r = td_request(&td, jreq("getChat"), 1.0);
    CHECK(td_is_error(r));
    CHECK(td_error_code(r) == 400);
    CHECK_STR(td_error_message(r), "Chat not found");
    cJSON_Delete(r);
}

static void test_close_waits_for_closed_state(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    fake_td_once("close", "{\"@type\":\"ok\"}",
                 "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":"
                 "{\"@type\":\"authorizationStateClosed\"}}");
    td_close(&td);
    CHECK(td.closed);
    CHECK(fake_td_count_of("close") == 1);
    td_close(&td); /* idempotent */
    CHECK(fake_td_count_of("close") == 1);
}

int main(void) {
    test_init_sets_verbosity();
    test_request_matches_extra_and_dispatches_updates();
    test_request_without_reply_times_out();
    test_error_reply();
    test_close_waits_for_closed_state();
    TEST_MAIN_END();
}

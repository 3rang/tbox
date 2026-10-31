/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_auth.c - the login state machine, driven non-interactively.
 */
#include "tg/auth.h"
#include "check.h"
#include "fake_td.h"

#define STATE(name) \
    "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"" name "\"}}"

static Config cfg_with_credentials(void) {
    Config c;
    memset(&c, 0, sizeof c);
    c.api_id = 12345;
    snprintf(c.api_hash, sizeof c.api_hash, "deadbeef");
    snprintf(c.tdlib_dir, sizeof c.tdlib_dir, "/tmp/tbox-test/tdlib");
    return c;
}

static void test_state_mapping(void) {
    CHECK(auth_state_from_type("authorizationStateWaitTdlibParameters") == AUTH_WAIT_PARAMS);
    CHECK(auth_state_from_type("authorizationStateWaitPhoneNumber") == AUTH_WAIT_PHONE);
    CHECK(auth_state_from_type("authorizationStateWaitOtherDeviceConfirmation") == AUTH_WAIT_QR);
    CHECK(auth_state_from_type("authorizationStateWaitPassword") == AUTH_WAIT_PASSWORD);
    CHECK(auth_state_from_type("authorizationStateReady") == AUTH_READY);
    CHECK(auth_state_from_type("authorizationStateLoggingOut") == AUTH_LOGGING_OUT);
    CHECK(auth_state_from_type("authorizationStateClosing") == AUTH_CLOSING);
    CHECK(auth_state_from_type("authorizationStateClosed") == AUTH_CLOSED);
    /* States tbox does not drive are recognised as such, not as garbage. */
    CHECK(auth_state_from_type("authorizationStateWaitEmailAddress") == AUTH_WAIT_OTHER);
    CHECK(auth_state_from_type("authorizationStateWaitCode") == AUTH_WAIT_OTHER);
    CHECK(auth_state_from_type("authorizationStateWaitRegistration") == AUTH_WAIT_OTHER);
    CHECK(auth_state_from_type("banana") == AUTH_UNKNOWN);
    CHECK(auth_state_from_type(NULL) == AUTH_UNKNOWN);
    CHECK_STR(auth_state_name(AUTH_READY), "ready");
    CHECK_STR(auth_state_name(AUTH_WAIT_QR), "wait-qr");
}

static void test_tdlib_parameters(void) {
    Config c = cfg_with_credentials();
    cJSON *p = auth_tdlib_parameters(&c);
    CHECK(jis(p, "setTdlibParameters"));
    CHECK(jint(p, "api_id") == 12345);
    CHECK_STR(jstr(p, "api_hash"), "deadbeef");
    CHECK_STR(jstr(p, "database_directory"), "/tmp/tbox-test/tdlib");
    CHECK(jbool(p, "use_message_database") == 1);
    CHECK(jbool(p, "use_secret_chats") == 0);
    CHECK(jbool(p, "use_test_dc") == 0);
    CHECK(jstr(p, "application_version")[0] != '\0');
    cJSON_Delete(p);
}

static void test_persisted_session_reaches_ready(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    Config c = cfg_with_credentials();
    char err[256] = "";

    fake_td_on("getAuthorizationState", "{\"@type\":\"authorizationStateWaitTdlibParameters\"}");
    fake_td_once("setTdlibParameters", "{\"@type\":\"ok\"}", STATE("authorizationStateReady"));

    CHECK(auth_run(&td, &c, 0, err, sizeof err) == AUTH_OK);
    CHECK(fake_td_count_of("setTdlibParameters") == 1);
    CHECK(jint(fake_td_last_of("setTdlibParameters"), "api_id") == 12345);
    /* The handler is restored afterwards. */
    CHECK(td.on_update == NULL);
}

static void test_fresh_session_needs_login(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    Config c = cfg_with_credentials();
    char err[256] = "";

    fake_td_on("getAuthorizationState", "{\"@type\":\"authorizationStateWaitTdlibParameters\"}");
    fake_td_once("setTdlibParameters", "{\"@type\":\"ok\"}", STATE("authorizationStateWaitPhoneNumber"));

    CHECK(auth_run(&td, &c, 0, err, sizeof err) == AUTH_NOT_LOGGED_IN);
    /* Non-interactive: never starts a QR login. */
    CHECK(fake_td_count_of("requestQrCodeAuthentication") == 0);
}

static void test_qr_and_password_states_need_login_too(void) {
    const char *states[] = { "authorizationStateWaitOtherDeviceConfirmation",
                             "authorizationStateWaitPassword" };
    for (size_t i = 0; i < 2; i++) {
        fake_td_reset();
        Td td;
        td_init(&td, fake_td_api());
        Config c = cfg_with_credentials();
        char err[256] = "";
        char reply[128];
        snprintf(reply, sizeof reply, "{\"@type\":\"%s\"}", states[i]);
        fake_td_on("getAuthorizationState", reply);
        CHECK(auth_run(&td, &c, 0, err, sizeof err) == AUTH_NOT_LOGGED_IN);
    }
}

static void test_missing_credentials(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    Config c;
    memset(&c, 0, sizeof c);
    char err[256] = "";

    fake_td_on("getAuthorizationState", "{\"@type\":\"authorizationStateWaitTdlibParameters\"}");
    CHECK(auth_run(&td, &c, 0, err, sizeof err) == AUTH_NOT_LOGGED_IN);
    CHECK(fake_td_count_of("setTdlibParameters") == 0);
    CHECK(strstr(err, "credentials") != NULL);
}

static void test_parameter_error_fails(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    Config c = cfg_with_credentials();
    char err[256] = "";

    fake_td_on("getAuthorizationState", "{\"@type\":\"authorizationStateWaitTdlibParameters\"}");
    fake_td_on("setTdlibParameters",
               "{\"@type\":\"error\",\"code\":400,\"message\":\"Valid api_id must be provided\"}");
    CHECK(auth_run(&td, &c, 0, err, sizeof err) == AUTH_FAILED);
    CHECK(strstr(err, "Valid api_id") != NULL);
}

static void test_unknown_state_is_refused(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    Config c = cfg_with_credentials();
    char err[256] = "";
    fake_td_on("getAuthorizationState", "{\"@type\":\"authorizationStateWaitRegistration\"}");
    CHECK(auth_run(&td, &c, 1, err, sizeof err) == AUTH_FAILED);
    CHECK(strstr(err, "unsupported") != NULL);
}

#define QR_STATE(link)                                                                       \
    "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":"        \
    "\"authorizationStateWaitOtherDeviceConfirmation\",\"link\":\"" link "\"}}"

static void test_interactive_qr_login(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    Config c = cfg_with_credentials();
    char err[256] = "";

    fake_td_on("getAuthorizationState", "{\"@type\":\"authorizationStateWaitTdlibParameters\"}");
    fake_td_once("setTdlibParameters", "{\"@type\":\"ok\"}", STATE("authorizationStateWaitPhoneNumber"));
    /* The QR request is answered with the first link; Telegram then
     * rotates the link once, and finally the phone confirms the scan. */
    fake_td_once("requestQrCodeAuthentication", "{\"@type\":\"ok\"}", QR_STATE("tg://login?token=one"));
    fake_td_push_when_idle(QR_STATE("tg://login?token=two"));
    fake_td_push_when_idle(STATE("authorizationStateReady"));

    CHECK(auth_run(&td, &c, 1, err, sizeof err) == AUTH_OK);
    CHECK(fake_td_count_of("requestQrCodeAuthentication") == 1);
    CHECK(cJSON_IsArray(jobj(fake_td_last_of("requestQrCodeAuthentication"), "other_user_ids")));
    CHECK(fake_td_count_of("setAuthenticationPhoneNumber") == 0);
}

static void test_qr_request_refused(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    Config c = cfg_with_credentials();
    char err[256] = "";
    fake_td_on("getAuthorizationState", "{\"@type\":\"authorizationStateWaitPhoneNumber\"}");
    fake_td_on("requestQrCodeAuthentication",
               "{\"@type\":\"error\",\"code\":400,\"message\":\"API_ID_INVALID\"}");
    CHECK(auth_run(&td, &c, 1, err, sizeof err) == AUTH_FAILED);
    CHECK(strstr(err, "QR") != NULL);
}

static void test_closed_session_fails(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    Config c = cfg_with_credentials();
    char err[256] = "";
    fake_td_on("getAuthorizationState", "{\"@type\":\"authorizationStateClosed\"}");
    CHECK(auth_run(&td, &c, 0, err, sizeof err) == AUTH_FAILED);
    CHECK(strstr(err, "closed") != NULL);
}

int main(void) {
    test_state_mapping();
    test_tdlib_parameters();
    test_persisted_session_reaches_ready();
    test_fresh_session_needs_login();
    test_qr_and_password_states_need_login_too();
    test_missing_credentials();
    test_parameter_error_fails();
    test_unknown_state_is_refused();
    test_interactive_qr_login();
    test_qr_request_refused();
    test_closed_session_fails();
    TEST_MAIN_END();
}

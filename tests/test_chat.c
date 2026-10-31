/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_chat.c - chat references and their resolution.
 */
#include "tg/chat.h"
#include "check.h"
#include "fake_td.h"

static void test_ref_parse(void) {
    ChatRef r;

    CHECK(chat_ref_parse("me", &r) == 0 && r.kind == CHAT_REF_ME);
    CHECK(chat_ref_parse("Saved", &r) == 0 && r.kind == CHAT_REF_ME);

    CHECK(chat_ref_parse("123456", &r) == 0 && r.kind == CHAT_REF_ID && r.id == 123456);
    CHECK(chat_ref_parse("-1001234567890", &r) == 0 && r.kind == CHAT_REF_ID &&
          r.id == -1001234567890LL);

    CHECK(chat_ref_parse("@durov", &r) == 0 && r.kind == CHAT_REF_USERNAME);
    CHECK_STR(r.text, "durov");
    CHECK(chat_ref_parse("https://t.me/telegram/", &r) == 0 && r.kind == CHAT_REF_USERNAME);
    CHECK_STR(r.text, "telegram");
    CHECK(chat_ref_parse("t.me/tdlib", &r) == 0 && r.kind == CHAT_REF_USERNAME);
    CHECK_STR(r.text, "tdlib");

    /* Titles are not resolved: tbox watch shows the id instead. */
    CHECK(chat_ref_parse("Family group", &r) == -1);
    CHECK(chat_ref_parse("-", &r) == -1);
    CHECK(chat_ref_parse("12ab", &r) == -1);

    CHECK(chat_ref_parse("", &r) == -1);
    CHECK(chat_ref_parse("@", &r) == -1);
    CHECK(chat_ref_parse(NULL, &r) == -1);
}

static void test_names(void) {
    cJSON *user = cJSON_Parse("{\"first_name\":\"Ada\",\"last_name\":\"Lovelace\","
                              "\"usernames\":{\"active_usernames\":[\"ada\",\"countess\"]}}");
    char name[64];
    chat_user_name(user, name, sizeof name);
    CHECK_STR(name, "Ada Lovelace");
    CHECK_STR(chat_username(user), "ada");
    cJSON_Delete(user);

    user = cJSON_Parse("{\"first_name\":\"Bot\",\"username\":\"legacy\"}");
    chat_user_name(user, name, sizeof name);
    CHECK_STR(name, "Bot");
    CHECK_STR(chat_username(user), "legacy");
    cJSON_Delete(user);
    CHECK_STR(chat_username(NULL), "");
}

static void test_resolve_me(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[512] = "";
    fake_td_on("getMe", "{\"@type\":\"user\",\"id\":777}");
    fake_td_on("createPrivateChat", "{\"@type\":\"chat\",\"id\":777}");
    CHECK(chat_resolve(&td, "me", err, sizeof err) == 777);
    CHECK(jint(fake_td_last_of("createPrivateChat"), "user_id") == 777);
}

static void test_resolve_username(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[512] = "";
    fake_td_on("searchPublicChat", "{\"@type\":\"chat\",\"id\":-100123}");
    CHECK(chat_resolve(&td, "@tdlib", err, sizeof err) == -100123);
    CHECK_STR(jstr(fake_td_last_of("searchPublicChat"), "username"), "tdlib");

    fake_td_reset();
    td_init(&td, fake_td_api());
    fake_td_on("searchPublicChat", "{\"@type\":\"error\",\"code\":400,\"message\":\"USERNAME_NOT_OCCUPIED\"}");
    CHECK(chat_resolve(&td, "@nobody", err, sizeof err) == 0);
    CHECK(strstr(err, "USERNAME_NOT_OCCUPIED") != NULL);
}

static void test_resolve_rejects_other_text(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[512] = "";
    CHECK(chat_resolve(&td, "Family", err, sizeof err) == 0);
    CHECK(strstr(err, "not a chat") != NULL);
    CHECK(fake_td_sent_count() == 0);
}

static void test_resolve_id_loads_chats_on_miss(void) {
    fake_td_reset();
    Td td;
    td_init(&td, fake_td_api());
    char err[512] = "";
    fake_td_once("getChat", "{\"@type\":\"error\",\"code\":400,\"message\":\"Chat not found\"}", NULL);
    fake_td_on("loadChats", "{\"@type\":\"ok\"}");
    fake_td_on("getChat", "{\"@type\":\"chat\",\"id\":4242}");
    CHECK(chat_resolve(&td, "4242", err, sizeof err) == 4242);
    CHECK(fake_td_count_of("loadChats") == 1);
    CHECK(fake_td_count_of("getChat") == 2);

    fake_td_reset();
    td_init(&td, fake_td_api());
    fake_td_on("getChat", "{\"@type\":\"error\",\"code\":400,\"message\":\"Chat not found\"}");
    fake_td_on("loadChats", "{\"@type\":\"error\",\"code\":404,\"message\":\"Not Found\"}");
    CHECK(chat_resolve(&td, "4242", err, sizeof err) == 0);
    CHECK(strstr(err, "4242") != NULL);
}

int main(void) {
    test_ref_parse();
    test_names();
    test_resolve_me();
    test_resolve_username();
    test_resolve_rejects_other_text();
    test_resolve_id_loads_chats_on_miss();
    TEST_MAIN_END();
}

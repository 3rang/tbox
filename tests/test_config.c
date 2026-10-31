/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_config.c - the config file, the environment and the data directory.
 */
#include "check.h"
#include "core/config.h"
#include "core/platform.h"

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (value[0] == '\0') {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

static void test_parse(void) {
    Config c;
    memset(&c, 0, sizeof c);
    config_parse(&c,
                 "# comment\n"
                 "api_id = 4242   # trailing comment\n"
                 "\n"
                 "junk line without equals\n"
                 "api_hash=abc123\r\n"
                 "unknown=ignored\n");
    CHECK(c.api_id == 4242);
    CHECK_STR(c.api_hash, "abc123");
    CHECK(config_has_credentials(&c));

    memset(&c, 0, sizeof c);
    config_parse(&c, "api_id=1");
    CHECK(c.api_id == 1);
    CHECK(!config_has_credentials(&c));
}

static void test_load_save_roundtrip(void) {
    char home[256];
    snprintf(home, sizeof home, "%s/tbox-test-config-%d", 
#ifdef _WIN32
             getenv("TEMP") ? getenv("TEMP") : ".",
#else
             "/tmp",
#endif
             (int)(size_t)&home % 100000);
    set_env("TBOX_HOME", home);
    set_env("TBOX_API_ID", "");
    set_env("TBOX_API_HASH", "");

    Config c;
    char err[256] = "";
    CHECK(config_load(&c, err, sizeof err) == 0);
    CHECK_STR(c.home, home);
    CHECK(strstr(c.tdlib_dir, "/tdlib") != NULL);
    CHECK(!config_has_credentials(&c));
    CHECK(platform_mkdir(c.tdlib_dir) == 0); /* exists already */

    c.api_id = 99;
    snprintf(c.api_hash, sizeof c.api_hash, "hash99");
    CHECK(config_save(&c, err, sizeof err) == 0);
    CHECK(platform_file_exists(c.config_file));

    Config again;
    CHECK(config_load(&again, err, sizeof err) == 0);
    CHECK(again.api_id == 99);
    CHECK_STR(again.api_hash, "hash99");

    /* The environment wins over the file. */
    set_env("TBOX_API_ID", "7");
    set_env("TBOX_API_HASH", "envhash");
    CHECK(config_load(&again, err, sizeof err) == 0);
    CHECK(again.api_id == 7);
    CHECK_STR(again.api_hash, "envhash");

    set_env("TBOX_API_ID", "");
    set_env("TBOX_API_HASH", "");
    remove(c.config_file);
}

int main(void) {
    test_parse();
    test_load_save_roundtrip();
    TEST_MAIN_END();
}

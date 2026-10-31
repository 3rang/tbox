/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_config.c - the data directory and the credentials from the environment.
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

static void test_home_and_dirs(void) {
    char home[256];
    snprintf(home, sizeof home, "%s/tbox-test-config-%d",
#ifdef _WIN32
             getenv("TEMP") ? getenv("TEMP") : ".",
#else
             "/tmp",
#endif
             (int)(size_t)&home % 100000);
    set_env("TBOX_HOME", home);

    Config c;
    char err[256] = "";
    CHECK(config_load(&c, err, sizeof err) == 0);
    CHECK_STR(c.home, home);
    CHECK(strstr(c.tdlib_dir, "/tdlib") != NULL);
    CHECK(platform_mkdir(c.tdlib_dir) == 0); /* created already */
}

static void test_credentials_from_environment(void) {
    Config c;
    char err[256] = "";

    set_env("TBOX_API_ID", "");
    set_env("TBOX_API_HASH", "");
    CHECK(config_load(&c, err, sizeof err) == 0);
    /* Unless this build was configured with defaults, there are none. */
    CHECK(config_has_credentials(&c) == (c.api_id > 0 && c.api_hash[0] != '\0'));

    set_env("TBOX_API_ID", "7");
    set_env("TBOX_API_HASH", "envhash");
    CHECK(config_load(&c, err, sizeof err) == 0);
    CHECK(c.api_id == 7);
    CHECK_STR(c.api_hash, "envhash");
    CHECK(config_has_credentials(&c));

    /* Half a pair is no pair (unless this build compiled a default in). */
    set_env("TBOX_API_HASH", "");
    CHECK(config_load(&c, err, sizeof err) == 0);
    CHECK(c.api_id == 7);
    CHECK(config_has_credentials(&c) == (c.api_hash[0] != '\0'));

    set_env("TBOX_API_ID", "");
    CHECK(strstr(CONFIG_CREDENTIALS_HELP, "TBOX_API_ID") != NULL);
    CHECK(strstr(CONFIG_CREDENTIALS_HELP, "my.telegram.org") != NULL);
}

int main(void) {
    test_home_and_dirs();
    test_credentials_from_environment();
    TEST_MAIN_END();
}

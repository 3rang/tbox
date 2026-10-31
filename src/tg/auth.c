/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "tg/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/platform.h"

static const struct {
    const char *type;
    AuthState state;
} STATES[] = {
    { "authorizationStateWaitTdlibParameters", AUTH_WAIT_PARAMS },
    { "authorizationStateWaitPhoneNumber", AUTH_WAIT_PHONE },
    { "authorizationStateWaitCode", AUTH_WAIT_CODE },
    { "authorizationStateWaitPassword", AUTH_WAIT_PASSWORD },
    { "authorizationStateWaitRegistration", AUTH_WAIT_REGISTRATION },
    { "authorizationStateReady", AUTH_READY },
    { "authorizationStateLoggingOut", AUTH_LOGGING_OUT },
    { "authorizationStateClosing", AUTH_CLOSING },
    { "authorizationStateClosed", AUTH_CLOSED },
};

static const char *NAMES[] = {
    "unknown", "wait-parameters", "wait-phone", "wait-code", "wait-password",
    "wait-registration", "wait-other", "ready", "logging-out", "closing", "closed",
};

AuthState auth_state_from_type(const char *type) {
    if (type == NULL) {
        return AUTH_UNKNOWN;
    }
    for (size_t i = 0; i < sizeof STATES / sizeof STATES[0]; i++) {
        if (strcmp(type, STATES[i].type) == 0) {
            return STATES[i].state;
        }
    }
    return strncmp(type, "authorizationState", 18) == 0 ? AUTH_WAIT_OTHER : AUTH_UNKNOWN;
}

const char *auth_state_name(AuthState s) {
    return (size_t)s < sizeof NAMES / sizeof NAMES[0] ? NAMES[s] : "?";
}

cJSON *auth_tdlib_parameters(const Config *cfg) {
    cJSON *p = jreq("setTdlibParameters");
    cJSON_AddNumberToObject(p, "api_id", (double)cfg->api_id);
    cJSON_AddStringToObject(p, "api_hash", cfg->api_hash);
    cJSON_AddStringToObject(p, "database_directory", cfg->tdlib_dir);
    cJSON_AddBoolToObject(p, "use_test_dc", 0);
    cJSON_AddBoolToObject(p, "use_file_database", 1);
    cJSON_AddBoolToObject(p, "use_chat_info_database", 1);
    cJSON_AddBoolToObject(p, "use_message_database", 1);
    cJSON_AddBoolToObject(p, "use_secret_chats", 0);
    cJSON_AddStringToObject(p, "system_language_code", "en");
    cJSON_AddStringToObject(p, "device_model", "tbox");
    cJSON_AddStringToObject(p, "application_version", TBOX_VERSION);
    return p;
}

/* --- the flow ------------------------------------------------------------ */

typedef struct AuthCtx {
    AuthState state;
    cJSON *state_obj;     /* last authorizationState object, for hints */
} AuthCtx;

static void set_state(AuthCtx *a, const cJSON *st) {
    cJSON_Delete(a->state_obj);
    a->state_obj = cJSON_Duplicate(st, 1);
    a->state = auth_state_from_type(jtype(st));
}

static void on_update(Td *td, const cJSON *ev, void *user) {
    (void)td;
    if (jis(ev, "updateAuthorizationState")) {
        set_state((AuthCtx *)user, jobj(ev, "authorization_state"));
    }
}

/* Pump until the state differs from `before`. 1 on change, 0 on timeout. */
static int wait_for_change(Td *td, AuthCtx *a, AuthState before, double timeout) {
    for (double left = timeout; left > 0 && a->state == before;) {
        if (!td_pump(td, 0.5)) {
            left -= 0.5;
        }
    }
    return a->state != before;
}

static const char *code_destination(const cJSON *state_obj) {
    const char *t = jtype(jobj(jobj(state_obj, "code_info"), "type"));
    if (strcmp(t, "authenticationCodeTypeTelegramMessage") == 0) {
        return "your other Telegram app";
    }
    if (strcmp(t, "authenticationCodeTypeSms") == 0) {
        return "SMS";
    }
    if (strcmp(t, "authenticationCodeTypeCall") == 0) {
        return "a phone call";
    }
    if (strcmp(t, "authenticationCodeTypeFlashCall") == 0) {
        return "a flash call (the code is the caller's number)";
    }
    return "Telegram";
}

/* Send a step of the flow and report TDLib's answer. 0 = accepted. */
static int step(Td *td, cJSON *req) {
    cJSON *r = td_request(td, req, TD_TIMEOUT);
    int rc = 0;
    if (td_is_error(r)) {
        fprintf(stderr, "error: %s\n", td_error_message(r));
        rc = -1;
    }
    cJSON_Delete(r);
    return rc;
}

AuthResult auth_run(Td *td, const Config *cfg, int interactive, char *err, size_t errlen) {
    AuthCtx a = { AUTH_UNKNOWN, NULL };
    TdUpdateFn old_fn = td->on_update;
    void *old_user = td->on_update_user;
    td_set_update_handler(td, on_update, &a);

    AuthResult result = AUTH_FAILED;
    char line[256];

    /* The first request wakes the client up; its reply is the state. */
    cJSON *r = td_request(td, jreq("getAuthorizationState"), TD_TIMEOUT);
    if (td_is_error(r)) {
        snprintf(err, errlen, "getAuthorizationState: %s", td_error_message(r));
        cJSON_Delete(r);
        goto out;
    }
    set_state(&a, r);
    cJSON_Delete(r);

    for (;;) {
        AuthState before = a.state;
        switch (a.state) {
        case AUTH_WAIT_PARAMS:
            if (!config_has_credentials(cfg)) {
                snprintf(err, errlen, "no API credentials; run `tbox login`");
                result = AUTH_NOT_LOGGED_IN;
                goto out;
            }
            r = td_request(td, auth_tdlib_parameters(cfg), TD_TIMEOUT);
            if (td_is_error(r)) {
                snprintf(err, errlen, "setTdlibParameters: %s", td_error_message(r));
                cJSON_Delete(r);
                goto out;
            }
            cJSON_Delete(r);
            break;

        case AUTH_WAIT_PHONE: {
            if (!interactive) {
                result = AUTH_NOT_LOGGED_IN;
                goto out;
            }
            if (platform_read_line("Phone number (international format, e.g. +41791234567): ",
                                   line, sizeof line) != 0 || line[0] == '\0') {
                snprintf(err, errlen, "login aborted");
                goto out;
            }
            cJSON *req = jreq("setAuthenticationPhoneNumber");
            cJSON_AddStringToObject(req, "phone_number", line);
            if (step(td, req) != 0) {
                continue; /* wrong input: ask again, state is unchanged */
            }
            break;
        }

        case AUTH_WAIT_CODE: {
            if (!interactive) {
                result = AUTH_NOT_LOGGED_IN;
                goto out;
            }
            printf("A login code was sent via %s.\n", code_destination(a.state_obj));
            if (platform_read_line("Code: ", line, sizeof line) != 0 || line[0] == '\0') {
                snprintf(err, errlen, "login aborted");
                goto out;
            }
            cJSON *req = jreq("checkAuthenticationCode");
            cJSON_AddStringToObject(req, "code", line);
            if (step(td, req) != 0) {
                continue;
            }
            break;
        }

        case AUTH_WAIT_PASSWORD: {
            if (!interactive) {
                result = AUTH_NOT_LOGGED_IN;
                goto out;
            }
            const char *hint = jstr(a.state_obj, "password_hint");
            if (hint[0] != '\0') {
                printf("Two-step verification is on. Hint: %s\n", hint);
            } else {
                printf("Two-step verification is on.\n");
            }
            if (platform_read_secret("Password: ", line, sizeof line) != 0 || line[0] == '\0') {
                snprintf(err, errlen, "login aborted");
                goto out;
            }
            cJSON *req = jreq("checkAuthenticationPassword");
            cJSON_AddStringToObject(req, "password", line);
            memset(line, 0, sizeof line);
            if (step(td, req) != 0) {
                continue;
            }
            break;
        }

        case AUTH_WAIT_REGISTRATION:
            snprintf(err, errlen,
                     "this phone number has no Telegram account; sign up with the Telegram app first");
            goto out;

        case AUTH_READY:
            result = AUTH_OK;
            goto out;

        case AUTH_LOGGING_OUT:
        case AUTH_CLOSING:
        case AUTH_CLOSED:
            snprintf(err, errlen, "TDLib closed the session (%s)", auth_state_name(a.state));
            goto out;

        case AUTH_WAIT_OTHER:
        case AUTH_UNKNOWN:
        default:
            snprintf(err, errlen, "unsupported authorization state: %s", jtype(a.state_obj));
            goto out;
        }

        if (!wait_for_change(td, &a, before, 60.0)) {
            snprintf(err, errlen, "timed out waiting for Telegram (state %s); check the network",
                     auth_state_name(a.state));
            goto out;
        }
    }

out:
    cJSON_Delete(a.state_obj);
    td_set_update_handler(td, old_fn, old_user);
    return result;
}

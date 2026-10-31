/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "tg/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/platform.h"
#include "tg/qr.h"

static const struct {
    const char *type;
    AuthState state;
} STATES[] = {
    { "authorizationStateWaitTdlibParameters", AUTH_WAIT_PARAMS },
    { "authorizationStateWaitPhoneNumber", AUTH_WAIT_PHONE },
    { "authorizationStateWaitOtherDeviceConfirmation", AUTH_WAIT_QR },
    { "authorizationStateWaitPassword", AUTH_WAIT_PASSWORD },
    { "authorizationStateReady", AUTH_READY },
    { "authorizationStateLoggingOut", AUTH_LOGGING_OUT },
    { "authorizationStateClosing", AUTH_CLOSING },
    { "authorizationStateClosed", AUTH_CLOSED },
};

static const char *NAMES[] = {
    "unknown", "wait-parameters", "wait-phone", "wait-qr", "wait-password",
    "wait-other", "ready", "logging-out", "closing", "closed",
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
    cJSON *state_obj;     /* last authorizationState object: link, hint */
    int updates;          /* counts updateAuthorizationState, so a new QR
                             link in the same state is noticed too */
} AuthCtx;

static void set_state(AuthCtx *a, const cJSON *st) {
    cJSON_Delete(a->state_obj);
    a->state_obj = cJSON_Duplicate(st, 1);
    a->state = auth_state_from_type(jtype(st));
    a->updates++;
}

static void on_update(Td *td, const cJSON *ev, void *user) {
    (void)td;
    if (jis(ev, "updateAuthorizationState")) {
        set_state((AuthCtx *)user, jobj(ev, "authorization_state"));
    }
}

/* Pump until another authorization update arrives. Only silence counts
 * against the timeout. 1 on update, 0 on timeout. */
static int wait_for_update(Td *td, AuthCtx *a, int seen, double timeout) {
    for (double left = timeout; left > 0 && a->updates == seen;) {
        if (!td_pump(td, 0.5)) {
            left -= 0.5;
        }
    }
    return a->updates != seen;
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
    AuthCtx a = { AUTH_UNKNOWN, NULL, 0 };
    TdUpdateFn old_fn = td->on_update;
    void *old_user = td->on_update_user;
    td_set_update_handler(td, on_update, &a);

    AuthResult result = AUTH_FAILED;
    char shown_link[1024] = "";
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
        int seen = a.updates;
        switch (a.state) {
        case AUTH_WAIT_PARAMS:
            if (!config_has_credentials(cfg)) {
                snprintf(err, errlen, "no API credentials");
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
            cJSON *req = jreq("requestQrCodeAuthentication");
            cJSON_AddItemToObject(req, "other_user_ids", cJSON_CreateArray());
            if (step(td, req) != 0) {
                snprintf(err, errlen, "Telegram refused the QR login request");
                goto out;
            }
            break;
        }

        case AUTH_WAIT_QR: {
            if (!interactive) {
                result = AUTH_NOT_LOGGED_IN;
                goto out;
            }
            const char *link = jstr(a.state_obj, "link");
            if (strcmp(link, shown_link) != 0) {
                snprintf(shown_link, sizeof shown_link, "%s", link);
                printf("\nScan this QR code with Telegram on your phone:\n"
                       "Settings > Devices > Link Desktop Device\n\n");
                if (qr_render(link, stdout) != 0) {
                    printf("(cannot draw the QR code; open this link on the phone)\n%s\n", link);
                }
                printf("\nWaiting for the scan... (the code refreshes every ~30 s)\n");
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
                continue; /* wrong password: the state is unchanged, ask again */
            }
            break;
        }

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

        if (!wait_for_update(td, &a, seen, 90.0)) {
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

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * demo/qr_login.c - minimal TDLib QR-code login demo (new API, cJSON,
 * qrcodegen). Credentials are compiled in by CMake, easy-telegram style:
 *
 *   cmake -S . -B build -DTG_API_ID=123456 -DTG_API_HASH=0123456789abcdef...
 *   cmake --build build
 *   build\qr-login.exe
 *
 * Flow: td_create_client_id -> setTdlibParameters (in-memory, no files) ->
 * authorizationStateWaitPhoneNumber -> requestQrCodeAuthentication (never a
 * phone number / SMS / code) -> authorizationStateWaitOtherDeviceConfirmation
 * -> QR in the terminal (true black/white blocks on a Windows console) ->
 * scan with the Telegram app (Settings > Devices > Link Desktop Device) ->
 * if the account has 2FA: authorizationStateWaitPassword -> type the
 * password (masked) -> checkAuthenticationPassword ->
 * authorizationStateReady -> getMe -> print account -> close -> exit 0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#endif

#include <cJSON.h>
#include <td/telegram/td_json_client.h>
#include <qrcodegen.h>

#define RECV_TIMEOUT 1.0 /* seconds each td_receive() poll waits */

/* Credentials arrive as compile definitions from CMake; without them the
 * demo explains how to configure and exits before touching TDLib. */
#ifndef TBOX_TG_API_ID
#define TBOX_TG_API_ID 0
#endif
#ifndef TBOX_TG_API_HASH
#define TBOX_TG_API_HASH ""
#endif

static int g_vt = 0; /* ANSI/UTF-8 console support enabled */

static void send_req(int client_id, cJSON *req); /* forward */

#ifdef _WIN32
/* Switch the Windows console to UTF-8 output + ANSI escapes so the QR can
 * be drawn as real black/white blocks no matter what colors the user's
 * terminal theme uses. Returns 1 on success. */
static int enable_vt(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return 0;
    if (!GetConsoleMode(h, &mode))
        return 0;
    if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return 0;
    if (SetConsoleOutputCP(65001) == 0)
        return 0;
    return 1;
}
#endif

/* Read a password from the console without echoing it (masked with '*'). */
static void prompt_and_send_password(int client_id)
{
    char pw[256];
    size_t len = 0;

    printf("  Two-step verification is enabled on this account.\n"
           "  Enter your 2FA password: ");
    fflush(stdout);

#ifdef _WIN32
    {
        int c;
        while ((c = _getch()) != 13 && c != 10) {
            if (c == 3 || c == 27) { /* Ctrl-C / Esc */
                printf("\n  Cancelled.\n");
                exit(0);
            }
            if (c == 224 || c == 0) { /* extended key prefix: eat code */
                (void)_getch();
                continue;
            }
            if ((c == 8 || c == 127) && len > 0) {
                len--;
                fputs("\b \b", stdout);
                fflush(stdout);
                continue;
            }
            if (c >= 32 && len + 1 < sizeof pw) {
                pw[len++] = (char)c;
                fputc('*', stdout);
                fflush(stdout);
            }
        }
    }
#else
    if (fgets(pw, sizeof pw, stdin) == NULL)
        return;
    len = strcspn(pw, "\r\n");
#endif
    pw[len] = '\0';
    printf("\n");

    {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "@type", "checkAuthenticationPassword");
        cJSON_AddStringToObject(req, "password", pw);
        cJSON_AddStringToObject(req, "@extra", "pwd");
        send_req(client_id, req);
    }
    memset(pw, 0, sizeof pw);
}

static const char *type_of(const cJSON *obj)
{
    const cJSON *t;
    if (obj == NULL)
        return "";
    t = cJSON_GetObjectItem(obj, "@type");
    return cJSON_IsString(t) ? t->valuestring : "";
}

static void send_req(int client_id, cJSON *req)
{
    char *json = cJSON_PrintUnformatted(req);
    if (json != NULL) {
        td_send(client_id, json);
        cJSON_free(json);
    }
    cJSON_Delete(req);
}

static void request_qr(int client_id)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "@type", "requestQrCodeAuthentication");
    cJSON_AddItemToObject(req, "other_user_ids", cJSON_CreateArray());
    send_req(client_id, req);
}

/* Render the tg://login link as a QR made of real black/white blocks.
 * On a Windows console with ANSI support we switch on vt mode and paint
 * every module as a black or white cell (independent of the terminal
 * theme), two cells wide per module for a square shape, plus a white
 * quiet zone so phone scanners can lock on. Fallback: old ## grid. */
static void print_qr(const char *link)
{
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    int size, x, y;
    const int margin = 4;

    printf("\n  Scan this QR code with your Telegram app\n"
           "  (Settings > Devices > Link Desktop Device):\n\n");

    if (!qrcodegen_encodeText(link, temp, qr, qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, 1)) {
        printf("  (QR too large to render - open the link directly:\n"
               "  %s)\n\n", link);
        return;
    }
    size = qrcodegen_getSize(qr);

    if (g_vt) {
        const char *blk = "\033[40m  \033[0m"; /* black module */
        const char *wht = "\033[47m  \033[0m"; /* white module */
        for (y = -margin; y < size + margin; y++) {
            fputs("  ", stdout);
            for (x = -margin; x < size + margin; x++) {
                int dark = 0;
                if (x >= 0 && x < size && y >= 0 && y < size)
                    dark = qrcodegen_getModule(qr, x, y);
                fputs(dark ? blk : wht, stdout);
            }
            putchar('\n');
        }
    } else {
        for (y = 0; y < size; y++) {
            fputs("  ", stdout);
            for (x = 0; x < size; x++)
                fputs(qrcodegen_getModule(qr, x, y) ? "##" : "  ", stdout);
            putchar('\n');
        }
    }
    printf("\n  ...or open directly: %s\n\n", link);
}

int main(void)
{
    int client_id, done = 0, qr_sent = 0, getme_sent = 0;
    char prev_link[512] = "";

    if (TBOX_TG_API_ID == 0 || TBOX_TG_API_HASH[0] == '\0') {
        fprintf(stderr,
                "qr-login: no Telegram credentials compiled in.\n"
                "Configure them first, e.g.\n"
                "  cmake -S . -B build -DTG_API_ID=<id> "
                "-DTG_API_HASH=<hash>\n");
        return 1;
    }

#ifdef _WIN32
    g_vt = enable_vt();
#endif

    /* Keep the terminal clean: TDLib's own log chatter off. */
    td_set_log_verbosity_level(0);

    client_id = td_create_client_id();

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "@type", "setTdlibParameters");
    cJSON_AddNumberToObject(params, "api_id", (double)TBOX_TG_API_ID);
    cJSON_AddStringToObject(params, "api_hash", TBOX_TG_API_HASH);
    cJSON_AddBoolToObject(params, "use_test_dc", 0);
    cJSON_AddStringToObject(params, "database_directory", "");
    cJSON_AddStringToObject(params, "files_directory", "");
    cJSON_AddBoolToObject(params, "use_file_database", 0);
    cJSON_AddBoolToObject(params, "use_message_database", 0);
    cJSON_AddBoolToObject(params, "use_secret_chats", 0);
    cJSON_AddStringToObject(params, "device_model", "qr-login-demo");
    cJSON_AddStringToObject(params, "system_language_code", "en");
    cJSON_AddStringToObject(params, "application_version", "0.5.0");
    send_req(client_id, params);

    while (!done) {
        const char *raw = td_receive(RECV_TIMEOUT);
        if (raw == NULL)
            continue; /* timeout: no update yet */

        cJSON *upd = cJSON_Parse(raw);
        if (upd == NULL)
            continue;

        const char *type = type_of(upd);

        if (strcmp(type, "updateAuthorizationState") == 0) {
            cJSON *state = cJSON_GetObjectItem(upd, "authorization_state");
            const char *st = type_of(state);

            if (strcmp(st, "authorizationStateWaitPhoneNumber") == 0 &&
                !qr_sent) {
                qr_sent = 1;
                request_qr(client_id);
            }

            if (strcmp(st,
                       "authorizationStateWaitOtherDeviceConfirmation") == 0) {
                cJSON *item = cJSON_GetObjectItem(state, "link");
                const char *link = cJSON_GetStringValue(item);
                if (link != NULL && strcmp(link, prev_link) != 0) {
                    snprintf(prev_link, sizeof prev_link, "%s", link);
                    print_qr(link);
                }
            }

            if (strcmp(st, "authorizationStateWaitPassword") == 0) {
                const char *hint = cJSON_GetStringValue(
                    cJSON_GetObjectItem(state, "password_hint"));
                if (hint != NULL && hint[0] != '\0')
                    printf("  (hint: %s)\n", hint);
                prompt_and_send_password(client_id);
            }

            if (strcmp(st, "authorizationStateReady") == 0 && !getme_sent) {
                getme_sent = 1;
                cJSON *me = cJSON_CreateObject();
                cJSON_AddStringToObject(me, "@type", "getMe");
                cJSON_AddStringToObject(me, "@extra", "qr-demo");
                send_req(client_id, me);
            }

            /* `close` results arrive NESTED here, not as a top-level
             * "authorizationStateClosed" object. This is the real exit. */
            if (strcmp(st, "authorizationStateClosed") == 0) {
                printf("  Closed. Bye!\n");
                done = 1;
            }
        }

        if (strcmp(type, "user") == 0) {
            cJSON *extra = cJSON_GetObjectItem(upd, "@extra");
            if (cJSON_IsString(extra) &&
                strcmp(extra->valuestring, "qr-demo") == 0) {
                const char *first = cJSON_GetStringValue(
                    cJSON_GetObjectItem(upd, "first_name"));
                const char *last = cJSON_GetStringValue(
                    cJSON_GetObjectItem(upd, "last_name"));
                printf("  Logged in as %s %s\n",
                       first != NULL ? first : "",
                       last != NULL ? last : "");
                cJSON *close = cJSON_CreateObject();
                cJSON_AddStringToObject(close, "@type", "close");
                send_req(client_id, close);
            }
        }

        if (strcmp(type, "error") == 0) {
            cJSON *code = cJSON_GetObjectItem(upd, "code");
            const char *msg = cJSON_GetStringValue(
                cJSON_GetObjectItem(upd, "message"));
            fprintf(stderr, "  TDLib error %d: %s\n",
                    cJSON_IsNumber(code) ? (int)code->valuedouble : -1,
                    msg != NULL ? msg : "");
            if (msg != NULL && strcmp(msg, "PASSWORD_HASH_INVALID") == 0) {
                /* TDLib re-sends authorizationStateWaitPassword: retry. */
                printf("  Wrong 2FA password - try again.\n");
            } else {
                cJSON_Delete(upd);
                return 2;
            }
        }

        if (strcmp(type, "authorizationStateClosed") == 0) {
            printf("  Closed. Bye!\n");
            done = 1;
        }

        cJSON_Delete(upd);
    }

    return 0;
}

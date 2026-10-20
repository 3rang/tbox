/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_session.c — the UI-free session engine, driven by the fake tdjson.
 * No dll, no network, no terminal: exactly what a front end would see.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define TBOX_ACCESS _access
#define TBOX_F_OK 0
#else
#include <unistd.h>
#define TBOX_ACCESS access
#define TBOX_F_OK F_OK
#endif

#include "service/tbox_session.h"
#include "fake_td.h"

static const char *DIR = "tbox_session_test";

static int take(TBoxSession *s, TBoxSessionEventOut *out) {
    return tbox_session_take_event(s, out, 3000);
}

static int sent_contains(FakeTd *f, const char *needle) {
    for (int i = 0; i < f->sent_n; i++) {
        if (strstr(f->sent[i], needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Requests made BY the receive thread (in reaction to an update) land just
 * after the event that announced them, so wait for them like the client
 * tests wait for updates. */
static void wait_sent(FakeTd *f, const char *needle) {
    for (int i = 0; i < 400 && !sent_contains(f, needle); i++) {
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
    }
}

int main(void) {
    /* bad input is refused, not half-started */
    FakeTd *fake = fake_td_start(7);
    assert(tbox_session_start(&fake->api, "", 123, "hash") == NULL);
    assert(tbox_session_start(&fake->api, DIR, 0, "hash") == NULL);

    /* 1. start: dirs exist, parameters go out eagerly, no cue needed */
    TBoxSession *s = tbox_session_start(&fake->api, DIR, 123, "hash");
    assert(s != NULL);
    assert(TBOX_ACCESS("tbox_session_test/tdlib", TBOX_F_OK) == 0);
    assert(TBOX_ACCESS("tbox_session_test/files", TBOX_F_OK) == 0);
    assert(sent_contains(fake, "setTdlibParameters"));
    assert(sent_contains(fake, "tbox_session_test/tdlib"));

    TBoxSessionEventOut ev;

    /* 2. WaitPhoneNumber -> the engine asks for QR on its own */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitPhoneNumber\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(strcmp(ev.data, "WAIT_PHONE") == 0);
    wait_sent(fake, "requestQrCodeAuthentication");
    assert(sent_contains(fake, "requestQrCodeAuthentication"));

    /* 3. a QR link arrives once ... */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitOtherDeviceConfirmation\",\"link\":\"tg://login?token=AAA\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(strcmp(ev.data, "WAIT_QR") == 0);
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_QR_LINK);
    assert(strcmp(ev.data, "tg://login?token=AAA") == 0);

    /* ... but the same link twice is not a new link (no redraw storm) */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitOtherDeviceConfirmation\",\"link\":\"tg://login?token=AAA\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(tbox_session_take_event(s, &ev, 150) == 0);

    /* a rotated link is a new link again */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitOtherDeviceConfirmation\",\"link\":\"tg://login?token=BBB\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_QR_LINK);
    assert(strcmp(ev.data, "tg://login?token=BBB") == 0);

    /* 4. 2FA: hint out, password in - and the value really travels */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitPassword\",\"password_hint\":\"hint\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(strcmp(ev.data, "WAIT_PASSWORD") == 0);
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_PASSWORD_NEEDED);
    assert(strcmp(ev.data, "hint") == 0);
    tbox_session_submit_password(s, "secret");
    assert(sent_contains(fake, "checkAuthenticationPassword"));
    assert(sent_contains(fake, "secret"));

    /* 5. Ready -> the engine asks for the account, the reply becomes READY */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateReady\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(strcmp(ev.data, "READY") == 0);
    wait_sent(fake, "getMe");
    assert(sent_contains(fake, "getMe"));

    fake_td_enqueue(fake, "{\"@type\":\"user\",\"@extra\":\"me\",\"first_name\":\"Tarang\",\"last_name\":\"Patel\"}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_READY);
    assert(strcmp(ev.data, "Tarang Patel") == 0);

    /* 6. a tdlib error reply surfaces with its code, and the engine does
     *    NOT decide what it means - that is the front's call */
    fake_td_enqueue(fake, "{\"@type\":\"error\",\"@extra\":\"qr\",\"code\":400,\"message\":\"API_ID_INVALID\"}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_ERROR);
    assert(ev.code == 400);
    assert(strcmp(ev.data, "API_ID_INVALID") == 0);

    /* 7. clean shutdown: close asked, tdlib confirms, engine reports it */
    tbox_session_request_close(s);
    assert(sent_contains(fake, "\"close\""));
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateClosed\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(strcmp(ev.data, "CLOSED") == 0);
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_CLOSED);

    tbox_session_close(s);
    fake_td_free(fake);
    remove("tbox_session_test/tdlib");
    remove("tbox_session_test/files");
    remove("tbox_session_test");

    printf("session flow OK\n");
    return 0;
}
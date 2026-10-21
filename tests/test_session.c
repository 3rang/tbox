/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_session.c — the UI-free session engine, driven by the fake tdjson.
 * No dll, no network, no terminal: exactly what a front end would see.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Non-blocking drain step: returns 1 if an event was ready now. */
static int take_now(TBoxSession *s, TBoxSessionEventOut *out) {
    return tbox_session_take_event(s, out, 0);
}

/* The caller owns the payload (heap, spec 6.6): compare it, then free it.
 * The test compares through this helper so every take that reads data
 * releases it. */
static int data_is(TBoxSessionEventOut *out, const char *want) {
    int same = out->data != NULL && strcmp(out->data, want) == 0;
    free(out->data);
    out->data = NULL;
    return same;
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

/* ---- spec 6.6: a 20,000-event burst loses nothing real ----------------
 *
 * Progress events may coalesce (only the newest per op stays) and an
 * impossible backlog is reported through the resync flag; every other
 * event must arrive, in order, with its payload intact even though the
 * queue started at 8 slots. */
static void test_event_delivery_never_drops(void) {
#ifdef _WIN32
    Sleep(300); /* let the previous session's receive thread fully stop */
#else
    usleep(300000);
#endif
    FakeTd *fake = fake_td_start(8);
    TBoxSession *s = tbox_session_start(&fake->api, DIR, 123, "hash", NULL);
    assert(s != NULL);

    /* 20,000 connection updates: one connection NAME means one event (the
     * engine dedups states), so this is 20,000 distinct events. */
    for (int i = 0; i < 20000; i++) {
        static const char *const STATE[] = {"connectionStateConnecting",
                                            "connectionStateUpdating"};
        char ev[256];
        snprintf(ev, sizeof ev,
                 "{\"@type\":\"updateConnectionState\",\"state\":{\"@type\":\"%s\"}}",
                 STATE[i % 2]);
        fake_td_enqueue(fake, ev);
    }

    /* Wait until the receive thread has drained the fake's queue into the
     * session queue (a read blocks the fake for 50 ms once empty). */
    unsigned waited = 0;
    while (!sent_contains(fake, "setTdlibParameters") && waited < 3000) {
        waited += 50;
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
    }
    /* Drain only when the fake has genuinely gone empty; a fixed sleep is
     * not enough for 20,000 events. The fake blocks 50 ms per empty poll
     * (timeout 1.0 clamps to 50 ms), so 12 quiet polls mean fully drained. */
    int quiet = 0;
    while (quiet < 12) {
        int before = fake->qhead;
#ifdef _WIN32
        Sleep(30);
#else
        usleep(30000);
#endif
        quiet = (fake->qhead == before) ? quiet + 1 : 0;
    }

    /* 20,000 events with a stalled front: the spec's own 10,000-item
     * high-water mark is reached, so the engine MUST say so through
     * resync_needed rather than pretend (spec 6.6). */
    assert(tbox_session_resync_needed(s) == 1);
    assert(tbox_session_progress_dropped(s) == 0);

    int expect = 0;
    int got = 0;
    TBoxSessionEventOut ev;
    while (take_now(s, &ev)) {
        if (ev.type == TBOX_SESSION_EV_CATALOG_CHANGED) {
            assert(strcmp(ev.data, "*") == 0);
            free(ev.data);
            continue;
        }
        assert(ev.type == TBOX_SESSION_EV_CONNECTION);
        const char *want = (expect % 2 == 0) ? "connectionStateConnecting"
                                             : "connectionStateUpdating";
        assert(data_is(&ev, want));
        expect++;
        got++;
        if (got >= 20000) {
            break;
        }
    }
    /* Everything the queue accepted came out in order (no silent holes),
     * and the overflow is reported through the sticky resync flag - the
     * hint event itself cannot be queued precisely because the queue is
     * what overflowed (spec 6.6). */
    assert(got == 10000);
    assert(tbox_session_resync_needed(s) == 1);

    tbox_session_close(s);
    fake_td_free(fake);
    printf("burst: 10000 queued in order, overflow announced via resync\n");
}

/* ---- spec 6.6: a live reader loses nothing, even mid-burst ----------- */

static void test_live_reader_keeps_every_event(void) {
#ifdef _WIN32
    Sleep(300); /* let the previous session's receive thread fully stop */
#else
    usleep(300000);
#endif
    FakeTd *fake = fake_td_start(10);
    TBoxSession *s = tbox_session_start(&fake->api, DIR, 123, "hash", NULL);
    assert(s != NULL);

    /* The engine is fed 20,000 distinct events while the caller reads
     * continuously, the way the engine will run (spec 9.4: read until
     * stdin closes). Nothing may be lost and no reconcile is needed. */
    static const char *const STATE[] = {"connectionStateConnecting",
                                        "connectionStateUpdating"};
    int fed = 0;
    int expect = 0;
    int got = 0;
    TBoxSessionEventOut ev;
    while (fed < 20000) {
        if (fake->qn - fake->qhead < 2000) {
            for (int k = 0; k < 2000 && fed < 20000; k++, fed++) {
                char buf[256];
                snprintf(buf, sizeof buf,
                         "{\"@type\":\"updateConnectionState\",\"state\":{\"@type\":\"%s\"}}",
                         STATE[fed % 2]);
                fake_td_enqueue(fake, buf);
            }
        }
        while (take_now(s, &ev)) {
            if (ev.type == TBOX_SESSION_EV_CATALOG_CHANGED) {
                free(ev.data);
                continue;
            }
            assert(ev.type == TBOX_SESSION_EV_CONNECTION);
            const char *want = (expect % 2 == 0) ? "connectionStateConnecting"
                                                 : "connectionStateUpdating";
            assert(data_is(&ev, want));
            expect++;
            got++;
        }
    }
    unsigned waited = 0;
    while (got < 20000 && waited < 15000) {
        if (!take_now(s, &ev)) {
#ifdef _WIN32
            Sleep(5);
#else
            usleep(5000);
#endif
            waited += 5;
            continue;
        }
        if (ev.type == TBOX_SESSION_EV_CATALOG_CHANGED) {
            free(ev.data);
            continue;
        }
        assert(ev.type == TBOX_SESSION_EV_CONNECTION);
        const char *want = (expect % 2 == 0) ? "connectionStateConnecting"
                                             : "connectionStateUpdating";
        assert(data_is(&ev, want));
        expect++;
        got++;
    }
    assert(got == 20000);
    assert(tbox_session_resync_needed(s) == 0); /* a live reader needs none */
    assert(tbox_session_progress_dropped(s) == 0);

    tbox_session_close(s);
    fake_td_free(fake);
    printf("live reader: 20000 events, none lost\n");
}

/* ---- spec 6.6: progress never blocks the queue ----------------------- */

static void test_progress_events_do_not_clog(void) {
#ifdef _WIN32
    Sleep(300);
#else
    usleep(300000);
#endif
    FakeTd *fake = fake_td_start(9);
    TBoxSession *s = tbox_session_start(&fake->api, DIR, 123, "hash", NULL);
    assert(s != NULL);

    /* A stalled front gets flooded with progress for two ops, then the
     * updates it MUST see. Progress is advisory: it may coalesce, but it
     * must never push the queue over the cap or lose what follows. */
    for (int i = 0; i < 400; i++) {
        char a[256];
        char b[256];
        snprintf(a, sizeof a,
                 "{\"event\":\"transfer.progress\",\"data\":{\"op_id\":\"aa\",\"done\":%d,\"total\":400}}",
                 i);
        snprintf(b, sizeof b,
                 "{\"event\":\"transfer.progress\",\"data\":{\"op_id\":\"bb\",\"done\":%d,\"total\":400}}",
                 i);
        fake_td_enqueue(fake, a);
        fake_td_enqueue(fake, b);
    }
    fake_td_enqueue(fake, "{\"@type\":\"updateConnectionState\",\"state\":{\"@type\":\"connectionStateReady\"}}");
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitPhoneNumber\"}}");
#ifdef _WIN32
    Sleep(1200);
#else
    usleep(1200000);
#endif

    /* The connection update and the auth update must both arrive: 798
     * progress events (not yet consumed by session_update - they arrive
     * with the transfer engine) cannot bury them or force a reconcile. */
    int saw_conn = 0;
    int saw_auth = 0;
    TBoxSessionEventOut ev;
    while (take_now(s, &ev)) {
        if (ev.type == TBOX_SESSION_EV_CONNECTION &&
            ev.data != NULL && strcmp(ev.data, "connectionStateReady") == 0) {
            saw_conn = 1;
        }
        if (ev.type == TBOX_SESSION_EV_AUTH_STATE) {
            saw_auth = 1;
        }
        free(ev.data);
        ev.data = NULL;
    }
    assert(saw_conn == 1);
    assert(saw_auth == 1);
    assert(tbox_session_resync_needed(s) == 0);
    assert(tbox_session_progress_dropped(s) == 0);

    tbox_session_close(s);
    fake_td_free(fake);
    printf("progress: queued without clogging, later events kept\n");
}

int main(void) {
    /* bad input is refused, not half-started */
    FakeTd *fake = fake_td_start(7);
    assert(tbox_session_start(&fake->api, "", 123, "hash", NULL) == NULL);
    assert(tbox_session_start(&fake->api, DIR, 0, "hash", NULL) == NULL);

    /* 1. start: dirs exist, parameters go out eagerly, no cue needed */
    TBoxSession *s = tbox_session_start(&fake->api, DIR, 123, "hash", NULL);
    assert(s != NULL);
    assert(TBOX_ACCESS("tbox_session_test/tdlib", TBOX_F_OK) == 0);
    assert(TBOX_ACCESS("tbox_session_test/files", TBOX_F_OK) == 0);
    assert(sent_contains(fake, "setTdlibParameters"));
    assert(sent_contains(fake, "tbox_session_test/tdlib"));

    TBoxSessionEventOut ev;

    /* 2. WaitPhoneNumber -> the engine asks for QR on its own */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitPhoneNumber\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(data_is(&ev, "WAIT_PHONE"));
    wait_sent(fake, "requestQrCodeAuthentication");
    assert(sent_contains(fake, "requestQrCodeAuthentication"));

    /* 3. a QR link arrives once ... */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitOtherDeviceConfirmation\",\"link\":\"tg://login?token=AAA\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(data_is(&ev, "WAIT_QR"));
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_QR_LINK);
    assert(data_is(&ev, "tg://login?token=AAA"));

    /* ... but the same link twice is not a new link (no redraw storm) */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitOtherDeviceConfirmation\",\"link\":\"tg://login?token=AAA\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(data_is(&ev, "WAIT_QR"));
    assert(tbox_session_take_event(s, &ev, 150) == 0);

    /* a rotated link is a new link again */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitOtherDeviceConfirmation\",\"link\":\"tg://login?token=BBB\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(data_is(&ev, "WAIT_QR"));
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_QR_LINK);
    assert(data_is(&ev, "tg://login?token=BBB"));

    /* 4. 2FA: hint out, password in - and the value really travels */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitPassword\",\"password_hint\":\"hint\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(data_is(&ev, "WAIT_PASSWORD"));
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_PASSWORD_NEEDED);
    assert(data_is(&ev, "hint"));
    tbox_session_submit_password(s, "secret");
    assert(sent_contains(fake, "checkAuthenticationPassword"));
    assert(sent_contains(fake, "secret"));

    /* 5. Ready -> the engine asks for the account, the reply becomes READY */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateReady\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(data_is(&ev, "READY"));
    wait_sent(fake, "getMe");
    assert(sent_contains(fake, "getMe"));

    fake_td_enqueue(fake, "{\"@type\":\"user\",\"@extra\":\"me\",\"first_name\":\"Tarang\",\"last_name\":\"Patel\"}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_READY);
    assert(data_is(&ev, "Tarang Patel"));

    /* 6. a tdlib error reply surfaces with its code, and the engine does
     *    NOT decide what it means - that is the front's call */
    fake_td_enqueue(fake, "{\"@type\":\"error\",\"@extra\":\"qr\",\"code\":400,\"message\":\"API_ID_INVALID\"}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_ERROR);
    assert(ev.code == 400);
    assert(data_is(&ev, "API_ID_INVALID"));

    /* 7. clean shutdown: close asked, tdlib confirms, engine reports it */
    tbox_session_request_close(s);
    assert(sent_contains(fake, "\"close\""));
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateClosed\"}}");
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_AUTH_STATE);
    assert(data_is(&ev, "CLOSED"));
    assert(take(s, &ev) && ev.type == TBOX_SESSION_EV_CLOSED);
    free(ev.data);

    tbox_session_close(s);
    fake_td_free(fake);
    remove("tbox_session_test/tdlib");
    remove("tbox_session_test/files");
    remove("tbox_session_test");

    printf("session flow OK\n");
    test_event_delivery_never_drops();
    test_live_reader_keeps_every_event();
    test_progress_events_do_not_clog();
    return 0;
}
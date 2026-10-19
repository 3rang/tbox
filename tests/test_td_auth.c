/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_td_auth.c — walks the proven QR login path through the FSM.
 * No tdjson needed: the table is pure logic, so this runs anywhere.
 */
#include <assert.h>
#include <stdio.h>

#include "td/tbox_td_auth.h"

/* helper: step returns the action; also asserts the next state */
static TBoxAuthAction step(TBoxAuthState *s, const char *td_state, TBoxAuthState want) {
    TBoxAuthTransition t = tbox_auth_next(*s, td_state);
    assert(t.next == want);
    *s = t.next;
    return t.action;
}

int main(void) {
    /* 1. the proven QR flow, start to finish */
    TBoxAuthState s = TBOX_AUTH_START;
    assert(step(&s, "authorizationStateWaitPhoneNumber", TBOX_AUTH_WAIT_PHONE) ==
           TBOX_AUTH_ACT_REQUEST_QR);
    assert(step(&s, "authorizationStateWaitOtherDeviceConfirmation", TBOX_AUTH_WAIT_QR) ==
           TBOX_AUTH_ACT_SHOW_QR);
    assert(step(&s, "authorizationStateWaitPassword", TBOX_AUTH_WAIT_PASSWORD) ==
           TBOX_AUTH_ACT_ASK_PASSWORD);
    assert(step(&s, "authorizationStateReady", TBOX_AUTH_READY) ==
           TBOX_AUTH_ACT_AUTHORIZED);
    assert(step(&s, "authorizationStateClosed", TBOX_AUTH_CLOSED) ==
           TBOX_AUTH_ACT_CLOSED);

    /* 2. password-less path (account without 2FA) */
    s = TBOX_AUTH_START;
    (void)step(&s, "authorizationStateWaitPhoneNumber", TBOX_AUTH_WAIT_PHONE);
    (void)step(&s, "authorizationStateWaitOtherDeviceConfirmation", TBOX_AUTH_WAIT_QR);
    assert(step(&s, "authorizationStateReady", TBOX_AUTH_READY) ==
           TBOX_AUTH_ACT_AUTHORIZED);

    /* 3. closed can arrive nested, from any live state */
    s = TBOX_AUTH_WAIT_QR;
    assert(step(&s, "authorizationStateClosed", TBOX_AUTH_CLOSED) ==
           TBOX_AUTH_ACT_CLOSED);
    s = TBOX_AUTH_WAIT_PASSWORD;
    assert(step(&s, "authorizationStateClosed", TBOX_AUTH_CLOSED) ==
           TBOX_AUTH_ACT_CLOSED);

    /* 4. transient closing and logging out keep the machine alive */
    s = TBOX_AUTH_READY;
    assert(step(&s, "authorizationStateClosing", TBOX_AUTH_CLOSING) ==
           TBOX_AUTH_ACT_NONE);
    (void)step(&s, "authorizationStateLoggingOut", TBOX_AUTH_CLOSING);

    /* 5. repeats are safe: caller guards double sends, table stays stable */
    s = TBOX_AUTH_START;
    for (int i = 0; i < 3; i++) {
        assert(step(&s, "authorizationStateWaitPhoneNumber", TBOX_AUTH_WAIT_PHONE) ==
               TBOX_AUTH_ACT_REQUEST_QR);
    }

    /* 6. unknown and invalid input fail closed */
    s = TBOX_AUTH_START;
    assert(step(&s, "authorizationStateWaitCode", TBOX_AUTH_FAILED) ==
           TBOX_AUTH_ACT_FATAL);
    assert(tbox_auth_next(TBOX_AUTH_START, NULL).action == TBOX_AUTH_ACT_FATAL);

    printf("td_auth flow OK\n");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td_auth.h — QR authentication state machine (pure logic).
 *
 * No I/O happens here: the caller feeds TDLib's authorizationState* names
 * in and gets the next state plus the action it must perform. Callers own
 * three known-gotcha guards:
 *   1. send requestQrCodeAuthentication only ONCE even if the QR state
 *      repeats (the table keeps asking while WaitPhoneNumber holds).
 *   2. re-render the QR whenever the link changes (~30 s token rotation).
 *   3. authorizationStateClosed arrives NESTED inside
 *      updateAuthorizationState — never as a top-level message.
 */
#ifndef TBOX_TD_AUTH_H
#define TBOX_TD_AUTH_H

/* States the FSM can be in (mirrors TDLib's authorizationState values). */
typedef enum TBoxAuthState {
    TBOX_AUTH_START = 0,
    TBOX_AUTH_WAIT_PARAMS,    /* tdlib asks for setTdlibParameters */
    TBOX_AUTH_WAIT_PHONE,     /* needs requestQrCodeAuthentication */
    TBOX_AUTH_WAIT_QR,        /* QR link shown, waiting for scan */
    TBOX_AUTH_WAIT_PASSWORD,  /* 2FA: cloud password needed */
    TBOX_AUTH_READY,          /* authorized */
    TBOX_AUTH_CLOSING,        /* transient: closing / logging out */
    TBOX_AUTH_CLOSED,
    TBOX_AUTH_FAILED,         /* unknown / invalid input */
} TBoxAuthState;

/* What the caller must DO after a transition. */
typedef enum TBoxAuthAction {
    TBOX_AUTH_ACT_NONE = 0,
    TBOX_AUTH_ACT_REQUEST_PARAMS, /* send setTdlibParameters (caller guards repeats) */
    TBOX_AUTH_ACT_REQUEST_QR, /* send requestQrCodeAuthentication */
    TBOX_AUTH_ACT_SHOW_QR,    /* the update carried a fresh QR link */
    TBOX_AUTH_ACT_ASK_PASSWORD,
    TBOX_AUTH_ACT_AUTHORIZED,
    TBOX_AUTH_ACT_CLOSED,
    TBOX_AUTH_ACT_FATAL,      /* cannot continue login */
} TBoxAuthAction;

typedef struct TBoxAuthTransition {
    TBoxAuthState next;
    TBoxAuthAction action;
} TBoxAuthTransition;

/* One step: TDLib state name in (NULL = invalid), transition out. */
TBoxAuthTransition tbox_auth_next(TBoxAuthState current, const char *td_state);

#endif /* TBOX_TD_AUTH_H */

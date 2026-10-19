/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td_auth.c — the transition table. Only this file knows the strings;
 * the rest of the code deals in enums. Unknown input fails closed.
 */
#include "tbox_td_auth.h"

#include <string.h>

typedef struct TBoxAuthRow {
    const char *name;          /* TDLib's authorizationState* type */
    TBoxAuthTransition into;
} TBoxAuthRow;

static const TBoxAuthRow TABLE[] = {
    /*  name                                              next-state            action                       */
    { "authorizationStateWaitPhoneNumber",             { TBOX_AUTH_WAIT_PHONE,    TBOX_AUTH_ACT_REQUEST_QR   } },
    { "authorizationStateWaitOtherDeviceConfirmation", { TBOX_AUTH_WAIT_QR,       TBOX_AUTH_ACT_SHOW_QR      } },
    { "authorizationStateWaitPassword",                { TBOX_AUTH_WAIT_PASSWORD, TBOX_AUTH_ACT_ASK_PASSWORD } },
    { "authorizationStateReady",                       { TBOX_AUTH_READY,         TBOX_AUTH_ACT_AUTHORIZED   } },
    { "authorizationStateClosing",                     { TBOX_AUTH_CLOSING,       TBOX_AUTH_ACT_NONE         } },
    { "authorizationStateLoggingOut",                  { TBOX_AUTH_CLOSING,       TBOX_AUTH_ACT_NONE         } },
    /* Closed can arrive from any point — including nested in the last
     * updateAuthorizationState before the session ends. */
    { "authorizationStateClosed",                      { TBOX_AUTH_CLOSED,        TBOX_AUTH_ACT_CLOSED       } },
};

TBoxAuthTransition tbox_auth_next(TBoxAuthState current, const char *td_state) {
    (void)current; /* the caller's clock; kept for future guards */
    TBoxAuthTransition fail = { TBOX_AUTH_FAILED, TBOX_AUTH_ACT_FATAL };
    if (td_state == NULL) {
        return fail;
    }
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(td_state, TABLE[i].name) == 0) {
            return TABLE[i].into;
        }
    }
    return fail;
}

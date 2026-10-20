/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td_api.c — the ONLY file in the codebase referencing native tdjson
 * symbols directly (besides tbox_td.c's execute wrapper). Everyone else
 * goes through the TBoxTdApi function table, which lets tests inject a
 * fake without touching the native library.
 */
#include "td/tbox_td_api.h"
#include "td/tbox_td.h"

static void real_set_verbosity(int level) { td_set_log_verbosity_level(level); }
static int real_create_client_id(void) { return td_create_client_id(); }
static void real_send(int client_id, const char *request) { td_send(client_id, request); }
static const char *real_receive(double timeout) { return td_receive(timeout); }

const TBoxTdApi *tbox_td_api(void) {
    static const TBoxTdApi api = {
        real_set_verbosity, real_create_client_id, real_send, real_receive,
    };
    return &api;
}

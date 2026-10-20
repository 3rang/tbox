/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td_api.h — the tdjson entry points as injectable data.
 *
 * JSON-free on purpose: the api table moves raw strings both ways, so the
 * real bindings never depend on a JSON parser. The production instance
 * (tbox_td_api()) wraps the linked symbols; tests build their own fakes.
 */
#ifndef TBOX_TD_API_H
#define TBOX_TD_API_H

typedef struct TBoxTdApi {
    void (*set_verbosity)(int level);
    int  (*create_client_id)(void);
    void (*send)(int client_id, const char *request);
    const char *(*receive)(double timeout); /* valid until the next call */
} TBoxTdApi;

/* The production API: the real tdjson symbols linked at build time. */
const TBoxTdApi *tbox_td_api(void);

#endif /* TBOX_TD_API_H */

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * fake_td.h - an in-memory stand-in for libtdjson.
 *
 * Tests script it: "when a request of this @type arrives, answer with this
 * reply (and maybe emit this update first)". Everything that was sent is
 * kept for inspection. No threads, no network, no TDLib.
 */
#ifndef TBOX_FAKE_TD_H
#define TBOX_FAKE_TD_H

#include "cJSON.h"
#include "core/td.h"

/* Forget scripts, queued events and sent requests. */
void fake_td_reset(void);

const TdApi *fake_td_api(void);

/* Queue an event (an update) for the next td_receive. */
void fake_td_push(const char *json);

/* Reply to every request of this @type with reply_json (the fake copies
 * the request's @extra onto it). */
void fake_td_on(const char *type, const char *reply_json);

/* Like fake_td_on, but consumed after one use, and optionally emits
 * update_json before the reply (as TDLib does with state changes). */
void fake_td_once(const char *type, const char *reply_json, const char *update_json);

/* What was sent, in order. */
int fake_td_sent_count(void);
cJSON *fake_td_sent(int i);                 /* borrowed */
cJSON *fake_td_last_of(const char *type);   /* borrowed, or NULL */
int fake_td_count_of(const char *type);

/* The last string passed to td_execute, or "". */
const char *fake_td_last_execute(void);

#endif

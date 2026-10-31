/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * message.h - reading a TDLib message object for display. Pure functions,
 * no TDLib calls, so they are easy to test.
 */
#ifndef TBOX_MESSAGE_H
#define TBOX_MESSAGE_H

#include <stddef.h>

#include "cJSON.h"

/* The text of a message, or a tag for other content:
 * "hello", "[photo] caption", "[document report.pdf]", "[sticker X]", "[call]" */
void message_text(const cJSON *msg, char *buf, size_t n);

/* Who sent it: sets *user_id or *chat_id (the other is 0). */
void message_sender(const cJSON *msg, long long *user_id, long long *chat_id);

/* Unix time as local "YYYY-MM-DD HH:MM". */
void message_time(long long unix_time, char *buf, size_t n);

#endif

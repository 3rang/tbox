/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * outbox.h - waiting for a sent message to actually reach Telegram.
 *
 * sendMessage replies at once with a message in the "pending" state and a
 * temporary id. The outcome arrives later as an update:
 *   updateMessageSendSucceeded  { message (final id), old_message_id }
 *   updateMessageSendFailed     { message, old_message_id, error }
 * For a file, updateFile reports upload progress on the way.
 */
#ifndef TBOX_OUTBOX_H
#define TBOX_OUTBOX_H

#include <stddef.h>

#include "core/td.h"

/* Block until the message with temporary id `old_message_id` is sent.
 * file_id > 0 enables a progress line for that file's upload.
 * Returns the final message id, or 0 with err filled. */
long long outbox_wait(Td *td, long long old_message_id, long long file_id, double timeout,
                      char *err, size_t errlen);

#endif

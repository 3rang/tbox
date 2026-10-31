/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * outbox.h - sending a message and waiting until Telegram has it.
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

/* Send `content` (an inputMessageText, inputMessageDocument, ...) to a
 * chat and block until Telegram accepts it. Takes ownership of content.
 * Prints upload progress for a document. Gives up after two minutes of
 * silence. Returns the final message id, or 0 with err filled. */
long long outbox_send(Td *td, long long chat_id, cJSON *content, char *err, size_t errlen);

#endif

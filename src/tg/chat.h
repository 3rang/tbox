/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * chat.h - turning what the user typed into a TDLib chat id.
 *
 *   me / saved            your Saved Messages
 *   @name, t.me/name      a public username (user, group or channel)
 *   123456, -100123456    a chat id as TDLib prints it
 *   anything else         searched in the title of chats you are in
 */
#ifndef TBOX_CHAT_H
#define TBOX_CHAT_H

#include <stddef.h>

#include "cJSON.h"
#include "core/td.h"

typedef enum ChatRefKind {
    CHAT_REF_ME,
    CHAT_REF_ID,
    CHAT_REF_USERNAME,
    CHAT_REF_NAME,
} ChatRefKind;

typedef struct ChatRef {
    ChatRefKind kind;
    long long id;        /* CHAT_REF_ID */
    char text[256];      /* username without '@', or the name to search */
} ChatRef;

/* Classify a reference. Pure. Returns -1 for an empty string. */
int chat_ref_parse(const char *s, ChatRef *out);

/* Resolve to a chat id, or 0 with err filled. */
long long chat_resolve(Td *td, const char *ref, char *err, size_t errlen);

/* "private", "group", "supergroup", "channel", "secret" or "?". Pure. */
const char *chat_type_name(const cJSON *chat);

/* One line "id  type  title  @username" describing a chat object. */
void chat_describe(Td *td, const cJSON *chat, char *buf, size_t n);

/* Display name of a user object: "First Last". Pure. */
void chat_user_name(const cJSON *user, char *buf, size_t n);

/* First active @username of a user or supergroup object, or "". Pure. */
const char *chat_username(const cJSON *obj);

#endif

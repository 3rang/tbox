/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * fake_chat.h — an in-memory Saved Messages chat for the sync tests.
 *
 * Where tests/fake_td.h is a scripted transport double (it replays whatever
 * bytes you queue), this is a semantic model: it stores messages, answers
 * getChatHistory and getMessage with real paging, and records the updates an
 * edit or a delete would produce. The sync tests drive the scan and update
 * code against it so they can assert on behavior, not on canned JSON.
 *
 * The model keeps messages in a dynamic array and, like TDLib, hands history
 * back newest-first below a given id. A page may be shorter than `limit` on
 * purpose (fake_chat_set_page_size), because spec 8.2 requires the scanner to
 * keep paging until a page truly comes back empty.
 */
#ifndef TBOX_TEST_FAKE_CHAT_H
#define TBOX_TEST_FAKE_CHAT_H

#include <stddef.h>

/* One media attached to a message (a subset of TDLib's `file`). */
typedef struct FakeFile {
    long long   id;         /* local file id (session-local) */
    long long   remote_id;  /* remote file id (stable) */
    char       *name;       /* original file name */
    long long   size;
    char        mime[64];
} FakeFile;

/* One message in the fake chat. `caption` is the formatted caption (the record
 * for managed messages); `text` is the text of a text message (folders and
 * notes). A media message has `file` populated. */
typedef struct FakeMsg {
    long long   id;
    const char *kind;   /* "document","photo","video","audio","voice","text" */
    char       *caption; /* or NULL */
    char       *text;    /* or NULL */
    int         has_file;
    FakeFile    file;
    long long   date;
} FakeMsg;

typedef struct FakeChat FakeChat;

/* A chat with no messages. */
FakeChat *fake_chat_new(void);

/* Frees the chat, its messages, and any queued updates. */
void fake_chat_free(FakeChat *c);

/* ---- adding messages ------------------------------------------------- */

/* Appends a media message (document/photo/video/audio/voice) and returns its
 * new message id. `caption` may be NULL. */
long long fake_chat_add_media(FakeChat *c, const char *kind, const char *name,
                              long long size, long long remote_id,
                              const char *caption);

/* Appends a text message (a folder record or a note) and returns its id. */
long long fake_chat_add_text(FakeChat *c, const char *text);

/* ---- changing messages ----------------------------------------------- */

/* Replaces a message's caption (an edit). Queues updateMessageContent, or
 * updateMessageEdited for a text message, exactly as TDLib would. */
void fake_chat_edit_caption(FakeChat *c, long long id, const char *caption);

/* Removes a message. `permanent` selects updateDeleteMessages is_permanent
 * (a real delete) versus from_cache (a local eviction the sync must ignore).
 * Only a permanent delete removes it from the model. */
void fake_chat_delete(FakeChat *c, long long id, int permanent);

/* ---- reading --------------------------------------------------------- */

/* The message with this id, or NULL. */
const FakeMsg *fake_chat_get(const FakeChat *c, long long id);

/* Number of messages currently stored. */
size_t fake_chat_count(const FakeChat *c);

/* Makes every history page return at most this many messages, no matter what
 * `limit` asked for (TDLib is allowed to). 0 restores "honour limit". This is
 * how a test forces the scanner through the short-page path. */
void fake_chat_set_page_size(FakeChat *c, int page_size);

/* The message ids the chat holds, newest first, into out[0..cap). Returns the
 * number written. */
size_t fake_chat_ids(const FakeChat *c, long long *out, size_t cap);

/* ---- TDLib-shaped JSON ----------------------------------------------- */

/* A getChatHistory reply: {"@type":"messages","total_count":N,"messages":[...]}
 * with the newest messages below `from_message_id` (0 means "from the top"),
 * at most `limit` of them, obeying the page-size cap. `offset` is applied the
 * way TDLib applies it (skip within that window). The caller frees the result
 * with free(). */
char *fake_chat_history_json(const FakeChat *c, long long from_message_id,
                             int offset, int limit);

/* A getMessage reply: {"@type":"message", ...}, or NULL if there is no such
 * message. Caller frees. */
char *fake_chat_message_json(const FakeChat *c, long long id);

/* ---- queued updates -------------------------------------------------- */

/* The next queued update as JSON, or NULL when the queue is empty. The caller
 * frees the result. Edits and permanent deletes queue their update here. */
char *fake_chat_take_update(FakeChat *c);

#endif /* TBOX_TEST_FAKE_CHAT_H */

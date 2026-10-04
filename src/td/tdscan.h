/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TDSCAN_H
#define TDSCAN_H

#include <stdbool.h>
#include <stddef.h>

#include "core/index.h"
#include "td/tdbridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * tdscan - walk Saved Messages and fill an archive index from it.
 *
 * The account itself is the archive: its own "Saved Messages" chat is a
 * private cloud folder, and every file ever uploaded through tbox is a
 * document in there with a JSON caption saying what it is really called.
 * This is the piece that reads that chat.
 *
 * Two TDLib requests, on purpose:
 *
 *   getMe          the user id, which IS the Saved Messages chat id. There
 *                  is no other way to name that chat.
 *   getChatHistory the messages, 100 at a time, newest to oldest.
 *
 * Why not searchMessages with a document filter? Because it only ever
 * searches TDLib's *local* database. On a cold session that database is
 * empty, so "warm it with getChatHistory, then search it" costs a full
 * history walk anyway and the search is then pure overhead. getChatHistory
 * does both jobs in one pass, straight from the server, paginating by
 * from_message_id - whose meaning has been stable across every TDLib
 * release, where searchMessages' offset/offset_order has not.
 *
 * Only documents are visible. A message with no `content.document` is not a
 * file, so it is not an entry - the v0.3 rule, and the reason the tree is not
 * thousands of "note to self" messages.
 *
 * Where the three interesting fields live in current TDLib, since none of them
 * is where the older documentation says:
 *
 *   the name      content.document.file_name, and for a file tbox uploaded
 *                 the caption at content.caption.text (a formattedText, a
 *                 sibling of `document` - not inside it)
 *   the size      content.document.document.expected_size - the Document
 *                 object carries no size of its own
 *   the file      content.document.document.id, the numeric File.id that
 *                 downloadFile asks for. The base64 string at
 *                 content.document.document.remote.id is not accepted by
 *                 downloadFile and is never used.
 *
 * A scan walks the whole chat every time rather than resuming where the last
 * one stopped: a file deleted in Telegram has to vanish from the tree, and
 * an incremental scan cannot see deletions.
 */

/* Scan results. */
#define TBOX_SCAN_OK        0
#define TBOX_SCAN_CANCELLED 1   /* should_stop became true; partial results */
#define TBOX_SCAN_ERROR   (-1)

/* Messages per request. TDLib's own default is 100; nothing here needs more. */
#define TBOX_SCAN_PAGE_LIMIT 100

/* How long one page may take, in milliseconds. */
#define TBOX_SCAN_PAGE_TIMEOUT 30000L

/*
 * How many times the *first* page may be asked again after TDLib answers
 * "Chat not found", and how long to wait in between.
 *
 * That answer is not a failure: on a session whose database has never held
 * Saved Messages, the chat is created while the database opens, and a request
 * that arrives first finds nothing. Once created it is cached forever, so a
 * short wait settles it and the scan starts on a clean session too.
 */
#define TBOX_SCAN_WARM_TRIES 5
#define TBOX_SCAN_WARM_WAIT_MS 1000L

typedef struct
{
    long long pages;     /* getChatHistory requests sent */
    long long scanned;   /* messages seen, documents and text alike */
    long long indexed;   /* documents that became a new entry */
    long long superseded;/* documents whose name an older one already claimed */
    int truncated;       /* 1 when max_pages stopped the walk early */

} tbox_scan_stats_t;

/*
 * Fill `chat_id` with the account's own user id (Saved Messages), and copy
 * the first name and phone into `user_name` / `phone` when those buffers are
 * non-NULL. Returns 0, or -1 with a message printed.
 */
int tbox_td_scan_whoami(tbox_tdbridge_t *bridge, long long *chat_id,
                        char *user_name, size_t user_name_size,
                        char *phone, size_t phone_size);

/*
 * Walk `chat_id` and add every document to `ar`.
 *
 * Entries are added newest first, and a name already in `ar` is left alone:
 * tbox_archive_add is "newest wins", so adding the older duplicate on top
 * would silently resurrect it.
 *
 * Cancellation is cooperative and checked between pages, so stopping costs at
 * most one request. A cancelled scan returns TBOX_SCAN_CANCELLED with
 * everything it had already collected in `ar` - usable, just incomplete.
 *
 * `max_pages` <= 0 walks the whole chat (a big Saved Messages is a few
 * seconds, not a problem); a positive value stops early and sets `truncated`.
 * `stats` may be NULL. Returns 0, 1 or -1 as above.
 */
int tbox_td_scan(tbox_tdbridge_t *bridge, long long chat_id,
                 volatile bool *should_stop, long long max_pages,
                 tbox_archive_t *ar, tbox_scan_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* TDSCAN_H */

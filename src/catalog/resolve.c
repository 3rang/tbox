/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * resolve.c — the pure resolver of spec 7.4: messages in, resolved objects,
 * Inbox and findings out. See resolve.h for the contract.
 *
 * Shape of the algorithm:
 *   1. group the managed messages by object id;
 *   2. per group pick the current revision (highest `r`, then highest message
 *      id), turn any other revision tied at that `r` into a conflict item
 *      (`object_id` "<o>-<message_id>") and note a kind mismatch across `k`;
 *   3. walk the tree: pick each object's effective parent (root, a live
 *      folder, or `lost`), send cycles and orphans to `lost`, and put
 *      trashed objects - and everything under them - in `trash`;
 *   4. the Inbox is every unmanaged file whose message id is not some
 *      record's `src`;
 *   5. report the findings (bad record, conflict, duplicate name, lost
 *      parent, cycle, kind mismatch).
 *
 * The output order is deterministic: objects by object_id, Inbox by message
 * id descending. Nothing here allocates per-message; the only allocation is
 * the result's own arrays plus a small intern arena for synthesized names.
 */
#include "catalog/resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny growable buffers ------------------------------------------- */

/* The result is built into three arrays plus an intern arena. Growth goes
 * through push_object/push_inbox/push_finding below. */

/* ---- intern arena ---------------------------------------------------- */

/* Copies s into the arena and returns the stable pointer. NULL on OOM. */
static const char *intern(TBoxResolveResult *r, const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *copy = (char *)malloc(n);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, n);
    char **grown = (char **)realloc(r->arena,
                                    (r->arena_count + 1) * sizeof(char *));
    if (grown == NULL) {
        free(copy);
        return NULL;
    }
    r->arena = grown;
    r->arena[r->arena_count++] = copy;
    return copy;
}

/* ---- ordering helpers ------------------------------------------------ */

/* Compares two decimal message ids numerically: skip leading zeros, compare
 * by significant length, then lexically. Exact across the whole id range,
 * with no floating point and no locale. Message ids from TDLib are always
 * non-negative, so no sign handling is needed. */
static int msgid_cmp(const char *a, const char *b) {
    while (*a == '0') {
        a++;
    }
    while (*b == '0') {
        b++;
    }
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb) {
        return la < lb ? -1 : 1;
    }
    int c = strcmp(a, b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static int msgid_gt(const char *a, const char *b) {
    return msgid_cmp(a, b) > 0;
}

static int object_id_cmp(const void *pa, const void *pb) {
    const TBoxObject *a = (const TBoxObject *)pa;
    const TBoxObject *b = (const TBoxObject *)pb;
    return strcmp(a->object_id, b->object_id);
}

static int inbox_cmp(const void *pa, const void *pb) {
    const TBoxInboxItem *a = (const TBoxInboxItem *)pa;
    const TBoxInboxItem *b = (const TBoxInboxItem *)pb;
    if (msgid_gt(a->message_id, b->message_id)) {
        return -1;
    }
    if (msgid_gt(b->message_id, a->message_id)) {
        return 1;
    }
    return 0;
}

/* ---- names ----------------------------------------------------------- */

/* case-insensitive equality for the duplicate-name finding. */
static int name_ieq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ---- result plumbing ------------------------------------------------- */

void tbox_resolve_result_free(TBoxResolveResult *out) {
    if (out == NULL) {
        return;
    }
    for (size_t i = 0; i < out->arena_count; i++) {
        free(out->arena[i]);
    }
    free(out->arena);
    free(out->objects);
    free(out->inbox);
    free(out->findings);
    memset(out, 0, sizeof *out);
}

/* Appends one object, growing the array. 0 ok, -1 OOM. */
static int push_object(TBoxResolveResult *r, const TBoxObject *o) {
    TBoxObject *grown = (TBoxObject *)realloc(
        r->objects, (r->object_count + 1) * sizeof(TBoxObject));
    if (grown == NULL) {
        return -1;
    }
    r->objects = grown;
    r->objects[r->object_count++] = *o;
    return 0;
}

static int push_inbox(TBoxResolveResult *r, const TBoxInboxItem *it) {
    TBoxInboxItem *grown = (TBoxInboxItem *)realloc(
        r->inbox, (r->inbox_count + 1) * sizeof(TBoxInboxItem));
    if (grown == NULL) {
        return -1;
    }
    r->inbox = grown;
    r->inbox[r->inbox_count++] = *it;
    return 0;
}

static int push_finding(TBoxResolveResult *r, TBoxFindingCode code,
                        const char *object_id, const char *message_id,
                        const char *detail) {
    TBoxFinding f;
    memset(&f, 0, sizeof f);
    f.code = code;
    if (object_id != NULL) {
        size_t n = strlen(object_id);
        if (n > TBOX_RESOLVE_ID_MAX) {
            n = TBOX_RESOLVE_ID_MAX;
        }
        memcpy(f.object_id, object_id, n);
        f.object_id[n] = '\0';
    }
    f.message_id = message_id;
    f.detail = detail ? intern(r, detail) : NULL;
    if (detail != NULL && f.detail == NULL) {
        return -1;
    }
    TBoxFinding *grown = (TBoxFinding *)realloc(
        r->findings, (r->finding_count + 1) * sizeof(TBoxFinding));
    if (grown == NULL) {
        return -1;
    }
    r->findings = grown;
    r->findings[r->finding_count++] = f;
    return 0;
}

/* ---- grouping -------------------------------------------------------- */

/* A group of managed messages sharing one object id. */
typedef struct Group {
    char object_id[TBOX_ID_HEX_LEN + 1];
    const TBoxResolveMsg **msgs; /* the messages in this group (borrowed) */
    size_t count;
    size_t cap;
} Group;

static int group_push(Group *g, const TBoxResolveMsg *m) {
    if (g->count == g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 4;
        const TBoxResolveMsg **grown = (const TBoxResolveMsg **)realloc(
            g->msgs, cap * sizeof(*g->msgs));
        if (grown == NULL) {
            return -1;
        }
        g->msgs = grown;
        g->cap = cap;
    }
    g->msgs[g->count++] = m;
    return 0;
}

/* Sort a group's revisions: highest `r` first, then highest message id. This
 * is the order rule 2 and 3 rely on. */
static int rev_cmp(const void *pa, const void *pb) {
    const TBoxResolveMsg *a = *(const TBoxResolveMsg *const *)pa;
    const TBoxResolveMsg *b = *(const TBoxResolveMsg *const *)pb;
    if (a->rec.rev != b->rec.rev) {
        return a->rec.rev > b->rec.rev ? -1 : 1;
    }
    if (msgid_gt(a->message_id, b->message_id)) {
        return -1;
    }
    if (msgid_gt(b->message_id, a->message_id)) {
        return 1;
    }
    return 0;
}

/* ---- the Inbox `src` rule -------------------------------------------- */

/* True when message id `id` is some record's `src` (so it is adopted and must
 * not appear in the Inbox). */
static int is_adopted_src(const TBoxResolveMsg *msgs, size_t count,
                          const char *id) {
    for (size_t i = 0; i < count; i++) {
        if ((msgs[i].kind == TBOX_MSG_FILE ||
             msgs[i].kind == TBOX_MSG_FOLDER) &&
            msgs[i].rec.has_src && strcmp(msgs[i].rec.src, id) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---- id lookup ------------------------------------------------------- */

/* A sorted copy of the object indices, so parent lookups are O(log n) rather
 * than O(n). Built once per resolve and used by classify(). */
typedef struct Idx {
    size_t   *order; /* indices into res->objects, sorted by object_id */
    size_t    count;
    const TBoxResolveResult *res;
} Idx;

/* Set only around the sort; resolve is single-threaded by design (spec 3.1
 * rule 2), so a file-scope pointer is safe here and keeps the comparator
 * portable to MSVC, which has no qsort_r. */
static const TBoxResolveResult *g_sort_res;

static int idx_cmp_plain(const void *pa, const void *pb) {
    size_t a = *(const size_t *)pa;
    size_t b = *(const size_t *)pb;
    return strcmp(g_sort_res->objects[a].object_id,
                  g_sort_res->objects[b].object_id);
}

static size_t idx_find(const Idx *ix, const char *id) {
    size_t lo = 0;
    size_t hi = ix->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(ix->res->objects[ix->order[mid]].object_id, id);
        if (c == 0) {
            return ix->order[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (size_t)-1;
}

/* ---- tree walk ------------------------------------------------------- */

/* The classification of one object's placement, before it is written back. */
typedef enum {
    PLACE_ROOT,   /* a live folder directly under root */
    PLACE_FOLDER, /* a live folder under another live folder */
    PLACE_LOST,   /* orphan or cycle -> Lost & Found */
    PLACE_TRASH   /* trashed, or under a trashed folder -> Trash */
} Placement;

/* Decides where object `self` lives by following the RAW parent chain of the
 * original data (never a resolved parent), so the answer for any object is
 * the same regardless of the order objects are processed. `raw` holds each
 * object's record-declared parent; `trashed` holds each object's own `t`.
 *
 * A missing parent, a non-folder parent, or a parent chain that loops is
 * `lost`. An own `t`, or any trashed ancestor, is `trash`. */
static Placement classify(const TBoxResolveResult *res, const Idx *ix,
                          const char *const *raw, const int *trashed,
                          size_t self, int *is_cycle) {
    if (is_cycle != NULL) {
        *is_cycle = 0;
    }
    if (trashed[self]) {
        return PLACE_TRASH;
    }
    const char *parent = raw[self];
    if (strcmp(parent, TBOX_FOLDER_ROOT) == 0) {
        return PLACE_ROOT;
    }
    /* Walk up. Each step must find a live folder; the chain may not return to
     * a node it already visited (that would be a cycle). */
    const char *cur = parent;
    for (size_t hop = 0; hop <= ix->count; hop++) {
        size_t found = idx_find(ix, cur);
        if (found == (size_t)-1) {
            return PLACE_LOST; /* orphan */
        }
        if (res->objects[found].kind != 'd') {
            return PLACE_LOST; /* parent is not a folder */
        }
        if (trashed[found]) {
            return PLACE_TRASH; /* a trashed ancestor hides the item */
        }
        const char *next = raw[found];
        if (strcmp(next, TBOX_FOLDER_ROOT) == 0) {
            return PLACE_FOLDER; /* a live folder hanging off root */
        }
        if (strcmp(next, cur) == 0) {
            if (is_cycle != NULL) {
                *is_cycle = 1;
            }
            return PLACE_LOST; /* self-parent: a cycle */
        }
        /* detect a loop: revisiting an ancestor, or self, is a cycle */
        cur = next;
        if (strcmp(cur, parent) == 0 ||
            strcmp(cur, res->objects[self].object_id) == 0) {
            if (is_cycle != NULL) {
                *is_cycle = 1;
            }
            return PLACE_LOST;
        }
    }
    if (is_cycle != NULL) {
        *is_cycle = 1;
    }
    return PLACE_LOST;
}

/* ---- main ------------------------------------------------------------ */

const char *tbox_finding_code_name(TBoxFindingCode code) {
    switch (code) {
    case TBOX_FIND_BAD_RECORD:    return "BAD_RECORD";
    case TBOX_FIND_CONFLICT:      return "CONFLICT";
    case TBOX_FIND_DUPLICATE_NAME:return "DUPLICATE_NAME";
    case TBOX_FIND_LOST_PARENT:   return "LOST_PARENT";
    case TBOX_FIND_CYCLE:         return "CYCLE";
    case TBOX_FIND_KIND_MISMATCH: return "KIND_MISMATCH";
    }
    return "UNKNOWN";
}

int tbox_resolve(const TBoxResolveMsg *msgs, size_t count,
                 TBoxResolveResult *out) {
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (msgs == NULL || count == 0) {
        return 0;
    }

    Group *groups = NULL;
    size_t group_count = 0;
    int rc = 0;

    /* ---- 1. bad records and grouping --------------------------------- */
    for (size_t i = 0; i < count && rc == 0; i++) {
        const TBoxResolveMsg *m = &msgs[i];
        if (m->bad_record &&
            (m->kind == TBOX_MSG_UNMANAGED || m->kind == TBOX_MSG_NOTE)) {
            rc = push_finding(out, TBOX_FIND_BAD_RECORD, NULL, m->message_id,
                              "a managed caption did not parse");
            continue;
        }
        if (m->kind != TBOX_MSG_FILE && m->kind != TBOX_MSG_FOLDER) {
            continue;
        }
        /* find or create the group for this object id */
        Group *g = NULL;
        for (size_t k = 0; k < group_count; k++) {
            if (strcmp(groups[k].object_id, m->rec.object_id) == 0) {
                g = &groups[k];
                break;
            }
        }
        if (g == NULL) {
            Group *grown = (Group *)realloc(groups,
                                            (group_count + 1) * sizeof(Group));
            if (grown == NULL) {
                rc = -1;
                break;
            }
            groups = grown;
            g = &groups[group_count++];
            memset(g, 0, sizeof *g);
            memcpy(g->object_id, m->rec.object_id, sizeof g->object_id);
        }
        rc = group_push(g, m);
    }

    /* ---- 2. per group: winner + conflict items ----------------------- */
    for (size_t gi = 0; gi < group_count && rc == 0; gi++) {
        Group *g = &groups[gi];
        if (g->count > 1) {
            qsort(g->msgs, g->count, sizeof(*g->msgs), rev_cmp);
        }
        const TBoxResolveMsg *winner = g->msgs[0];

        /* kind mismatch across the group (rule 4) */
        for (size_t k = 1; k < g->count; k++) {
            if (g->msgs[k]->rec.kind != winner->rec.kind) {
                rc = push_finding(out, TBOX_FIND_KIND_MISMATCH, g->object_id,
                                  winner->message_id,
                                  "k differs between revisions");
                break;
            }
        }
        if (rc != 0) {
            break;
        }

        TBoxObject o;
        memset(&o, 0, sizeof o);
        /* the group's id is shorter than the object's slot: copy the string,
         * not the whole slot */
        size_t id_len = strlen(g->object_id);
        memcpy(o.object_id, g->object_id, id_len + 1);
        o.kind = winner->rec.kind;
        o.parent_id = intern(out, winner->rec.parent);
        o.name = intern(out, winner->rec.name);
        o.rev = winner->rec.rev;
        o.message_id = winner->message_id;
        o.has_size = winner->rec.has_size;
        o.size = winner->rec.size;
        o.sha256 = winner->rec.has_sha256 ? intern(out, winner->rec.sha256)
                                          : NULL;
        o.has_mtime = winner->rec.has_mtime;
        o.mtime = winner->rec.mtime;
        o.has_trashed = winner->rec.has_trashed;
        o.trashed_at = winner->rec.trashed_at;
        o.conflict = 0;
        if (o.parent_id == NULL || o.name == NULL) {
            rc = -1;
            break;
        }
        rc = push_object(out, &o);
        if (rc != 0) {
            break;
        }

        /* every other revision tied at the winning `r` becomes a conflict
         * item: same folder, name with the conflict suffix, synthetic id */
        for (size_t k = 1; k < g->count && rc == 0; k++) {
            const TBoxResolveMsg *loser = g->msgs[k];
            if (loser->rec.rev != winner->rec.rev) {
                continue; /* a genuine older revision, not a conflict */
            }
            char syn[TBOX_RESOLVE_ID_MAX + 1];
            snprintf(syn, sizeof syn, "%s-%s", g->object_id, loser->message_id);

            char label[TBOX_NAME_MAX_BYTES + 2 + TBOX_MSGID_MAX_BYTES + 24];
            snprintf(label, sizeof label, "%s (conflict %s)", loser->rec.name,
                     loser->message_id);

            TBoxObject c;
            memset(&c, 0, sizeof c);
            memcpy(c.object_id, syn, sizeof c.object_id);
            c.kind = loser->rec.kind;
            c.parent_id = intern(out, loser->rec.parent);
            c.name = intern(out, label);
            c.rev = loser->rec.rev;
            c.message_id = loser->message_id;
            c.has_size = loser->rec.has_size;
            c.size = loser->rec.size;
            c.has_mtime = loser->rec.has_mtime;
            c.mtime = loser->rec.mtime;
            c.has_trashed = loser->rec.has_trashed;
            c.trashed_at = loser->rec.trashed_at;
            c.conflict = 1;
            if (c.parent_id == NULL || c.name == NULL) {
                rc = -1;
                break;
            }
            rc = push_object(out, &c);
            if (rc != 0) {
                break;
            }
            rc = push_finding(out, TBOX_FIND_CONFLICT, g->object_id,
                              loser->message_id, "two revisions share the "
                              "highest r");
            if (rc != 0) {
                break;
            }
        }
    }

    /* ---- 3. tree walk: effective parents ----------------------------- */
    /* Snapshot the record-declared parents and trashed flags before any are
     * rewritten, so classification never depends on processing order. */
    const char **raw = NULL;
    int *trashed = NULL;
    Idx ix;
    memset(&ix, 0, sizeof ix);
    if (out->object_count > 0) {
        raw = (const char **)malloc(out->object_count * sizeof(*raw));
        trashed = (int *)malloc(out->object_count * sizeof(*trashed));
        ix.order = (size_t *)malloc(out->object_count * sizeof(*ix.order));
        if (raw == NULL || trashed == NULL || ix.order == NULL) {
            free(raw);
            free(trashed);
            free(ix.order);
            tbox_resolve_result_free(out);
            for (size_t gi = 0; gi < group_count; gi++) {
                free(groups[gi].msgs);
            }
            free(groups);
            return -1;
        }
        ix.count = out->object_count;
        ix.res = out;
        for (size_t i = 0; i < out->object_count; i++) {
            raw[i] = out->objects[i].parent_id;
            trashed[i] = out->objects[i].has_trashed;
            ix.order[i] = i;
        }
        g_sort_res = out;
        qsort(ix.order, ix.count, sizeof(*ix.order), idx_cmp_plain);
        g_sort_res = NULL;
    }
    for (size_t i = 0; i < out->object_count && rc == 0; i++) {
        TBoxObject *o = &out->objects[i];
        int is_cycle = 0;
        Placement p = classify(out, &ix, raw, trashed, i, &is_cycle);
        if (p == PLACE_ROOT || p == PLACE_FOLDER) {
            o->parent_id = raw[i];
        } else if (p == PLACE_TRASH) {
            o->parent_id = TBOX_FOLDER_TRASH;
            if (!o->has_trashed) {
                /* hidden with its trashed ancestor; a zero timestamp keeps
                 * the value stable and clearly "not directly trashed" */
                o->has_trashed = 1;
                o->trashed_at = 0;
            }
        } else { /* PLACE_LOST */
            o->parent_id = TBOX_FOLDER_LOST;
            rc = push_finding(out, is_cycle ? TBOX_FIND_CYCLE
                                            : TBOX_FIND_LOST_PARENT,
                              o->object_id, o->message_id,
                              is_cycle ? "parent chain contains a cycle"
                                       : "parent missing or not a live folder");
        }
    }
    free(raw);
    free(trashed);
    free(ix.order);

    /* ---- 4. duplicate live names in one folder ----------------------- */
    for (size_t i = 0; i < out->object_count && rc == 0; i++) {
        const TBoxObject *a = &out->objects[i];
        if (a->has_trashed || strcmp(a->parent_id, TBOX_FOLDER_LOST) == 0) {
            continue;
        }
        for (size_t j = i + 1; j < out->object_count; j++) {
            const TBoxObject *b = &out->objects[j];
            if (b->has_trashed || strcmp(b->parent_id, TBOX_FOLDER_LOST) == 0) {
                continue;
            }
            if (strcmp(a->parent_id, b->parent_id) == 0 &&
                name_ieq(a->name, b->name)) {
                rc = push_finding(out, TBOX_FIND_DUPLICATE_NAME, a->object_id,
                                  a->message_id, "two live items share a name");
                if (rc != 0) {
                    break;
                }
                rc = push_finding(out, TBOX_FIND_DUPLICATE_NAME, b->object_id,
                                  b->message_id, "two live items share a name");
                if (rc != 0) {
                    break;
                }
            }
        }
    }

    /* ---- 5. the Inbox ------------------------------------------------ */
    for (size_t i = 0; i < count && rc == 0; i++) {
        const TBoxResolveMsg *m = &msgs[i];
        if (m->kind != TBOX_MSG_UNMANAGED) {
            continue;
        }
        if (is_adopted_src(msgs, count, m->message_id)) {
            continue;
        }
        TBoxInboxItem it;
        memset(&it, 0, sizeof it);
        it.message_id = m->message_id;
        it.orig_name = m->orig_name;
        it.mime = m->mime;
        it.has_size = m->has_size;
        it.size = m->size;
        it.date = m->date;
        rc = push_inbox(out, &it);
    }

    /* ---- order ------------------------------------------------------- */
    if (out->object_count > 1) {
        qsort(out->objects, out->object_count, sizeof(TBoxObject),
              object_id_cmp);
    }
    if (out->inbox_count > 1) {
        qsort(out->inbox, out->inbox_count, sizeof(TBoxInboxItem), inbox_cmp);
    }

    for (size_t gi = 0; gi < group_count; gi++) {
        free(groups[gi].msgs);
    }
    free(groups);

    if (rc != 0) {
        tbox_resolve_result_free(out);
        return -1;
    }
    return 0;
}

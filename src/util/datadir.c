/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * util/datadir.c - per-OS state directory layout. Extracted from
 * td/tdparams.c so the TDLib-free command layer (serve, status) resolves
 * the same paths as the login flow.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: no "unsafe" warnings for memcpy etc. */

#include "datadir.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#define TBOX_PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#define TBOX_PATH_SEP '/'
#endif

#ifndef TBOX_PATH_MAX
#define TBOX_PATH_MAX 512
#endif

/*
 * Windows accepts "/" wherever it accepts "\", so a path can reach us from
 * the command line or the environment in either style and must not gain a
 * second separator. POSIX has no such tolerance: "\" is an ordinary
 * character in a filename, so treating it as a separator there would corrupt
 * legitimate names.
 */
static int is_sep(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

int tbox_datadir_default(char *out, size_t size)
{
    const char *base;

    if (out == NULL || size == 0)
        return -1;

#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (base != NULL && base[0] != '\0') {
        if (tbox_datadir_join(out, size, base, "tbox") != 0)
            return -1;
        return 0;
    }
    /* no %LOCALAPPDATA% (rare service accounts): fall back to the profile */
    base = getenv("USERPROFILE");
    if (base == NULL || base[0] == '\0')
        return -1;
    {
        char local[TBOX_PATH_MAX];

        if (tbox_datadir_join(local, sizeof local, base, "AppData\\Local") != 0)
            return -1;
        if (tbox_datadir_join(out, size, local, "tbox") != 0)
            return -1;
    }
    return 0;
#else
    base = getenv("HOME");
    if (base == NULL || base[0] == '\0')
        return -1;
#ifdef __APPLE__
    {
        char support[TBOX_PATH_MAX];

        if (tbox_datadir_join(support, sizeof support, base,
                              "Library/Application Support") != 0)
            return -1;
        return tbox_datadir_join(out, size, support, "tbox");
    }
#else
    {
        const char *xdg = getenv("XDG_DATA_HOME");

        if (xdg != NULL && xdg[0] != '\0')
            return tbox_datadir_join(out, size, xdg, "tbox");
        return tbox_datadir_join(out, size, base, ".local/share/tbox");
    }
#endif
#endif
}

int tbox_datadir_join(char *out, size_t size, const char *root, const char *leaf)
{
    size_t root_len;
    size_t leaf_len;
    size_t sep;

    if (out == NULL || size == 0 || root == NULL || leaf == NULL)
        return -1;

    root_len = strlen(root);
    leaf_len = strlen(leaf);

    /* a separator is added unless root already ends with one */
    sep = 0;
    if (root_len == 0 || !is_sep(root[root_len - 1]))
        sep = 1;

    if (root_len + sep + leaf_len + 1 > size)
        return -1;

    memcpy(out, root, root_len);
    if (sep != 0)
        out[root_len] = TBOX_PATH_SEP;
    memcpy(out + root_len + sep, leaf, leaf_len + 1);

    return 0;
}

int tbox_datadir_session(char *out, size_t size, const char *root)
{
    return tbox_datadir_join(out, size, root, "tdlib");
}

int tbox_datadir_cache(char *out, size_t size, const char *root)
{
    return tbox_datadir_join(out, size, root, "cache");
}

int tbox_datadir_index(char *out, size_t size, const char *root)
{
    return tbox_datadir_join(out, size, root, "index.json");
}

static int ensure_one(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int is_dir(const char *path)
{
#ifdef _WIN32
    struct _stat st;

    return _stat(path, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int tbox_datadir_ensure(const char *path)
{
    char buf[TBOX_PATH_MAX];
    size_t len;
    size_t i;

    if (path == NULL)
        return -1;

    len = strlen(path);
    if (len == 0 || len >= sizeof buf)
        return -1;

    memcpy(buf, path, len + 1);

    /* nothing to create for a bare root ("/" or "C:\") */
    if (is_sep(buf[len - 1])) {
#ifdef _WIN32
        if (len == 3 && buf[1] == ':')       /* "C:\" */
            return 0;
#else
        if (len == 1)                        /* "/" */
            return 0;
#endif
    }

    /* create every parent first, left to right */
    for (i = 1; i < len; i++) {
        if (!is_sep(buf[i]))
            continue;
        buf[i] = '\0';
        (void)ensure_one(buf);               /* already existing is fine */
        buf[i] = path[i];
    }

    /* "already exists" is success: this helper is called on every start */
    if (is_dir(buf))
        return 0;
    (void)ensure_one(buf);

    return is_dir(buf) ? 0 : -1;
}
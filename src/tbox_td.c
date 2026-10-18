/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td.c — implementation of the tdjson bridge.
 *
 * The shared library is loaded at runtime, never linked: the exe runs from
 * any folder, and companion DLLs (OpenSSL etc.) are found in the directory
 * the library lives in (LOAD_WITH_ALTERED_SEARCH_PATH / dlopen already does
 * that on POSIX).
 */
#include "tbox_td.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct TBoxTd {
#ifdef _WIN32
    HMODULE lib;
#else
    void *lib;
#endif
    char *(*td_execute)(const char *request);
    void (*td_set_log_verbosity_level)(int level);
};

TBoxTd *tbox_td_open(const char *library_path) {
    if (library_path == NULL || library_path[0] == '\0') {
        return NULL;
    }
    TBoxTd *td = calloc(1, sizeof(TBoxTd));
    if (td == NULL) {
        return NULL;
    }
#ifdef _WIN32
    td->lib = LoadLibraryExA(library_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    td->lib = dlopen(library_path, RTLD_NOW);
#endif
    if (td->lib == NULL) {
        free(td);
        return NULL;
    }
#ifdef _WIN32
    td->td_execute = (char *(*)(const char *))GetProcAddress(td->lib, "td_execute");
    td->td_set_log_verbosity_level =
        (void (*)(int))GetProcAddress(td->lib, "td_set_log_verbosity_level");
#else
    td->td_execute = (char *(*)(const char *))dlsym(td->lib, "td_execute");
    td->td_set_log_verbosity_level = (void (*)(int))dlsym(td->lib, "td_set_log_verbosity_level");
#endif
    if (td->td_execute == NULL) {
        tbox_td_close(td);
        return NULL;
    }
    return td;
}

char *tbox_td_execute(TBoxTd *td, const char *request_json) {
    if (td == NULL || td->td_execute == NULL || request_json == NULL) {
        return NULL;
    }
    const char *out = td->td_execute(request_json);
    if (out == NULL) {
        return NULL;
    }
    /* tdjson owns the returned buffer (valid until the next call) — copy it. */
    size_t n = strlen(out);
    char *copy = malloc(n + 1);
    if (copy != NULL) {
        memcpy(copy, out, n + 1);
    }
    return copy;
}

void tbox_td_set_verbosity(TBoxTd *td, int level) {
    if (td != NULL && td->td_set_log_verbosity_level != NULL) {
        td->td_set_log_verbosity_level(level);
    }
}

void tbox_td_close(TBoxTd *td) {
    if (td == NULL) {
        return;
    }
#ifdef _WIN32
    if (td->lib != NULL) {
        FreeLibrary(td->lib);
    }
#else
    if (td->lib != NULL) {
        dlclose(td->lib);
    }
#endif
    free(td);
}

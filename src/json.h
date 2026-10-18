/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * json.h - tiny read helpers over cJSON so the code that walks TDLib
 * objects stays readable. Every getter tolerates a NULL object and a
 * missing key, returning a default instead.
 */
#ifndef TBOX_JSON_H
#define TBOX_JSON_H

#include <string.h>

#include "cJSON.h"

static inline cJSON *jobj(const cJSON *o, const char *key) {
    return o != NULL ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
}

static inline const char *jstr(const cJSON *o, const char *key) {
    cJSON *v = jobj(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static inline long long jint(const cJSON *o, const char *key) {
    cJSON *v = jobj(o, key);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : 0;
}

static inline int jbool(const cJSON *o, const char *key) {
    cJSON *v = jobj(o, key);
    return cJSON_IsTrue(v) ? 1 : 0;
}

/* TDLib's "@type" discriminator. */
static inline const char *jtype(const cJSON *o) {
    return jstr(o, "@type");
}

static inline int jis(const cJSON *o, const char *type) {
    return o != NULL && strcmp(jtype(o), type) == 0;
}

/* Build a request: {"@type": type}. The caller adds fields. */
static inline cJSON *jreq(const char *type) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "@type", type);
    return o;
}

#endif

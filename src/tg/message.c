/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "tg/message.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/json.h"

void message_text(const cJSON *msg, char *buf, size_t n) {
    const cJSON *content = jobj(msg, "content");
    const char *t = jtype(content);

    if (strcmp(t, "messageText") == 0) {
        snprintf(buf, n, "%s", jstr(jobj(content, "text"), "text"));
        return;
    }
    if (strncmp(t, "message", 7) != 0 || t[7] == '\0') {
        snprintf(buf, n, "[?]");
        return;
    }
    /* Every other kind becomes a tag from its type name - messagePhoto ->
     * [photo], messageVoiceNote -> [voiceNote] - plus what identifies it
     * (file name, sticker emoji) and the caption, if any. */
    const char *detail = "";
    if (strcmp(t, "messageDocument") == 0) {
        detail = jstr(jobj(content, "document"), "file_name");
    } else if (strcmp(t, "messageSticker") == 0) {
        detail = jstr(jobj(content, "sticker"), "emoji");
    }
    const char *caption = jstr(jobj(content, "caption"), "text");
    snprintf(buf, n, "[%c%s%s%s]%s%s", (char)tolower((unsigned char)t[7]), t + 8,
             detail[0] != '\0' ? " " : "", detail, caption[0] != '\0' ? " " : "", caption);
}

void message_sender(const cJSON *msg, long long *user_id, long long *chat_id) {
    const cJSON *sender = jobj(msg, "sender_id");
    *user_id = 0;
    *chat_id = 0;
    if (jis(sender, "messageSenderUser")) {
        *user_id = jint(sender, "user_id");
    } else if (jis(sender, "messageSenderChat")) {
        *chat_id = jint(sender, "chat_id");
    }
}

void message_time(long long unix_time, char *buf, size_t n) {
    time_t t = (time_t)unix_time;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, n, "%Y-%m-%d %H:%M", &tmv);
}

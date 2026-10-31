/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "tg/message.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/json.h"

static void with_caption(const char *tag, const cJSON *content, char *buf, size_t n) {
    const char *caption = jstr(jobj(content, "caption"), "text");
    if (caption[0] != '\0') {
        snprintf(buf, n, "[%s] %s", tag, caption);
    } else {
        snprintf(buf, n, "[%s]", tag);
    }
}

void message_text(const cJSON *msg, char *buf, size_t n) {
    const cJSON *content = jobj(msg, "content");
    const char *t = jtype(content);

    if (strcmp(t, "messageText") == 0) {
        snprintf(buf, n, "%s", jstr(jobj(content, "text"), "text"));
    } else if (strcmp(t, "messageDocument") == 0) {
        char tag[300];
        snprintf(tag, sizeof tag, "file %s", jstr(jobj(content, "document"), "file_name"));
        with_caption(tag, content, buf, n);
    } else if (strcmp(t, "messagePhoto") == 0) {
        with_caption("photo", content, buf, n);
    } else if (strcmp(t, "messageVideo") == 0) {
        with_caption("video", content, buf, n);
    } else if (strcmp(t, "messageAudio") == 0) {
        with_caption("audio", content, buf, n);
    } else if (strcmp(t, "messageVoiceNote") == 0) {
        with_caption("voice", content, buf, n);
    } else if (strcmp(t, "messageAnimation") == 0) {
        with_caption("gif", content, buf, n);
    } else if (strcmp(t, "messageSticker") == 0) {
        snprintf(buf, n, "[sticker %s]", jstr(jobj(content, "sticker"), "emoji"));
    } else if (strncmp(t, "message", 7) == 0) {
        /* messageCall -> [call], messageChatAddMembers -> [chatAddMembers] */
        snprintf(buf, n, "[%c%s]", (char)(t[7] | 0x20), t + 8);
    } else {
        snprintf(buf, n, "[?]");
    }
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

int message_is_outgoing(const cJSON *msg) {
    return jbool(msg, "is_outgoing");
}

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_td_client.c — client routing with the fake tdjson: no native dll,
 * deterministic, safe for CI anywhere.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>

#include "cJSON.h"
#include "td/tbox_td_client.h"
#include "fake_td.h"

static int g_updates;

static void count_update(void *user_data, const cJSON *update) {
    (void)user_data;
    g_updates++;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(update, "@type");
    printf("update: %s\n", cJSON_GetStringValue(type));
}

/* spin until cond() is true or ~2 s pass; deterministic enough for tests */
static void wait_for(int (*cond)(void)) {
    for (int i = 0; i < 400 && !cond(); i++) {
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
    }
}

static int have_update(void) { return g_updates == 1; }

int main(void) {
    /* 1. open: client id + verbosity come from the fake api */
    FakeTd *fake = fake_td_start(7);
    TBoxTdClient *c = tbox_td_client_open(&fake->api, 1);
    assert(c != NULL);
    assert(fake->client_id == 7);
    assert(fake->verbosity == 1);

    /* 2. update (no @extra) reaches the handler, parsed as cJSON */
    tbox_td_client_on_update(c, count_update, NULL);
    assert(tbox_td_client_start(c) == 0);
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\"}");
    wait_for(have_update);
    assert(g_updates == 1);

    /* 3. a reply WITH @extra is NOT routed to the handler; it waits in
     *    the reply queue for the caller instead */
    fake_td_enqueue(fake, "{\"@type\":\"ok\",\"@extra\":\"xyz\"}");
    char *queued = NULL;
    for (int i = 0; i < 400 && queued == NULL; i++) {
        queued = tbox_td_client_take_reply(c);
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
    }
    assert(queued != NULL);
    assert(strstr(queued, "\"xyz\"") != NULL);
    free(queued);
    assert(g_updates == 1); /* handler untouched */

    /* 4. send is fire-and-forget and reaches the fake verbatim */
    tbox_td_client_send(c, "{\"@type\":\"requestQrCodeAuthentication\"}");
    assert(fake->sent_n == 1);
    assert(strstr(fake->sent[0], "requestQrCodeAuthentication") != NULL);

    /* 5. a reply WITH @extra lands in the slot; take returns and drains */
    fake_td_enqueue(fake, "{\"@type\":\"user\",\"@extra\":\"me\",\"first_name\":\"T\"}");
    char *reply = NULL;
    for (int i = 0; i < 400 && reply == NULL; i++) {
        reply = tbox_td_client_take_reply(c);
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
    }
    assert(reply != NULL);
    assert(strstr(reply, "\"first_name\":\"T\"") != NULL);
    free(reply);
    assert(tbox_td_client_take_reply(c) == NULL); /* drained */

    /* 6. replies queue FIFO: two quick replies lose neither */
    fake_td_enqueue(fake, "{\"@type\":\"ok\",\"@extra\":\"one\"}");
    fake_td_enqueue(fake, "{\"@type\":\"ok\",\"@extra\":\"two\"}");
    char *r1 = NULL, *r2 = NULL;
    for (int i = 0; i < 400 && r2 == NULL; i++) {
        if (r1 == NULL) {
            r1 = tbox_td_client_take_reply(c);
        } else {
            r2 = tbox_td_client_take_reply(c);
        }
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
    }
    assert(r1 != NULL && strstr(r1, "\"one\"") != NULL);
    assert(r2 != NULL && strstr(r2, "\"two\"") != NULL);
    free(r1);
    free(r2);

    /* 7. close stops the receive thread promptly */
    tbox_td_client_close(c);
    fake_td_free(fake);

    printf("td_client flow OK\n");
    return 0;
}

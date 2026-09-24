/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>
#include <cJSON.h>
#include "td/tdhandler.h"
#include "cli/cli.h"


#ifndef TBOX_VERSION
#define TBOX_VERSION "0.5.0"
#endif

int main(int argc, char *argv[])
{
   
    // cli dispatch or initialization would go here
    tbox_cli(argc, argv);



    // cJSON *req = cJSON_CreateObject();
    // cJSON_AddStringToObject(req, "@type", "getOption");
    // cJSON_AddStringToObject(req, "name", "version");

    // char *req_str = cJSON_PrintUnformatted(req);
    // cJSON_free(req);

    // int cid = td_create_client_id();
    // const char *reply = td_execute(req_str);
    // cJSON_free(req_str);

    // printf("tbox v%s (tdjson client %d, %s)\n", TBOX_VERSION, cid, reply);
    // printf("tbox v%s\n", TBOX_VERSION);
    // cJSON_free((void *)reply);
    return 0;
}
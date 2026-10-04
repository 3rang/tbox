/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TDPARAMS_H
#define TDPARAMS_H

#include <stddef.h>

#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TDLib client configuration (td layer): where the credentials and the
 * session directory come from, and how setTdlibParameters is built.
 */

typedef struct
{
    int api_id;                 /* 0 = missing */
    char api_hash[64];          /* NUL-terminated hash, "" = missing */
    char data_dir[512];         /* TDLib database_directory (UTF-8, no trailing separator) */

} tbox_td_params_t;


/*
 * Default per-OS tbox data directory:
 *   Windows : %LOCALAPPDATA%\tbox
 *   macOS   : ~/Library/Application Support/tbox
 *   Linux   : $XDG_DATA_HOME or ~/.local/share/tbox
 *
 * Returns:
 *   0 = ok (out holds a NUL-terminated path)
 *  -1 = could not determine a home / app-data location
 */
int tbox_td_default_data_dir(char *out, size_t size);


/*
 * Create (mkdir -p style) the directory + "tdlib" session subdirectory
 * and write the resolved path of the subdirectory into params->data_dir.
 *
 * Returns:
 *   0 = ok
 *  -1 = failure (caller prints/keeps its own message)
 */
int tbox_td_prepare_dir(tbox_td_params_t *params);


/*
 * Fill api_id / api_hash from the environment variables TG_API_ID and
 * TG_API_HASH - the only source; there is no config file and nothing is
 * compiled in. Both are required and validated (id: positive digits,
 * hash: exactly 32 hex digits). On failure the reason is printed to
 * stderr, including the one-liner that sets them on this OS.
 *
 * Returns:
 *   0 = credentials available
 *  -1 = missing or malformed
 */
int tbox_td_load_credentials(tbox_td_params_t *params);


/*
 * Build the setTdlibParameters request for the tbox session:
 * persistent database in params->data_dir, file + message databases on,
 * secret chats off, TBOX app identity. Caller owns the returned object.
 *
 * Returns NULL only on cJSON allocation failure.
 */
cJSON *tbox_td_build_parameters(const tbox_td_params_t *params);

#ifdef __cplusplus
}
#endif

#endif /* TDPARAMS_H */

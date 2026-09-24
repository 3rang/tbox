/*
 * tdjson_export.h - "generated" macro header for td_json_client.h.
 *
 * TDLib's own build generates this file; we recreate it locally so the
 * official C API header can be used without building tdlib from source.
 * Boost Software License 1.0 applies to the original (see td_json_client.h).
 */
#ifndef TDLIB_TDJSON_EXPORT_H
#define TDLIB_TDJSON_EXPORT_H

#if defined(_WIN32) && defined(TDJSON_BUILD)
#define TDJSON_EXPORT __declspec(dllexport)
#elif defined(_WIN32) && defined(TDJSON_STATIC)
#define TDJSON_EXPORT
#elif defined(_WIN32)
#define TDJSON_EXPORT __declspec(dllimport)
#else
#define TDJSON_EXPORT
#endif

#endif /* TDLIB_TDJSON_EXPORT_H */
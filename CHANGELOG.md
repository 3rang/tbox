# Changelog

All notable changes to `tbox` are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The rewrite into one native C binary started at `0.5.0`. Everything below
`Unreleased` was built but not yet tagged, so nothing in it has been published
as a release yet.

## [Unreleased] — in development; the binary reports `0.5.0`

### Added

- **QR login** (`tbox auth`) — the full TDLib conversation: `setTdlibParameters`,
  restore fast path, `requestQrCodeAuthentication`, QR rendered in the
  terminal, a masked 2FA prompt when the account has one, `getMe`, then a clean
  `close`. Cancel with Ctrl+C: the flag is polled on every `td_receive` tick and
  the client is closed cooperatively, never killed mid-call.
- **Session ownership** (`tbox serve`) — an exclusive lock at
  `<data>/session.lock`, enforced by the OS rather than by the file:
  `CreateFile` with `FILE_SHARE_READ` on Windows, `flock(LOCK_EX|LOCK_NB)` on
  POSIX. Ownership belongs to the process, so a crash releases it and there is
  never a stale lock to clean up. A second `serve` is refused and names the
  owning pid.
- **Status seam** (`tbox status`) — reads only `status.json` and never opens
  TDLib, so it works while `serve` runs and after it died (a dead pid is
  reported as *stale status*). The file is replaced atomically, so a reader can
  never see half of it. This is the contract a tray icon or a service will use.
- **Archive index** (`src/core/index.c`) — the caption ledger that turns a flat
  list of Telegram documents into a folder tree: caption parse/build,
  path normalization with traversal rejection, implied directories, listing
  with lexicographic ordering, and `stat`. Byte-compatible with the v0.3 Python
  client, so existing archives keep resolving.
- **`tbox selftest`** — 30 offline checks of the core rules (caption schema,
  path handling, tree shape, state names). No TDLib, no network, no writes.
- **Shared `--data` handling** — `auth`, `serve` and `status` parse the option
  and resolve the data root through one implementation, so they can never
  disagree about where the session lives.
- **`status` became strictly read-only** — it no longer creates the data
  directory it is asked about.
- **CI** (`.github/workflows/ci.yml`) — every OS must build clean with
  warnings-as-errors and pass `ctest`; plus an ASan + UBSan run and a
  fail-closed credential check that needs no secrets.
- **Release pipeline** (`.github/workflows/release.yml`) — tag-driven packaging
  with a SHA-256 sidecar, a `QUICKSTART.txt` in the archive, and a guard that
  refuses to publish a tag whose version does not match the compiled binary.
- **`-DTBOX_TDJSON_LIBRARY=OFF`** — explicitly request the TDLib-free build.
  Previously the auto-detect shadowed the value and TDLib was linked anyway.

- **The FTP server, read-only** (`src/net/ftp_proto.c`, `src/net/ftpd.c`) —
  loopback only, `USER tbox` plus a token drawn from the OS entropy source and
  compared in constant time, one thread per connection, PASV only (`EPSV` and
  `REST` are answered `502`), and the full read command set: `LIST`, `NLST`,
  `MLSD`, `SIZE`, `MDTM`, `RETR`, `CWD`, `PWD`, `CDUP`, `CUP`, `FEAT`, `NOOP`.
  Bodies come from the bounded cache and stream out as they are read, so a
  download never blocks the scan.
- **The scan loop** (`src/td/tdscan.c`) — walks Saved Messages newest to oldest
  and rebuilds the whole index, so a file deleted in Telegram disappears from
  the tree. Rescans on `updateNewMessage`, `updateDeleteMessages`,
  `updateMessageContent`, `updateEditMessageContent` and
  `updateMessageSendSucceeded`, after a fixed 400 ms settle.
- **The message → entry mapping** (`src/core/scanmap.c`) — one TDLib message to
  one archive entry, with the caption authoritative and `file_name`, size and
  date as the fallbacks for documents tbox did not upload.
- **The body cache and read path** (`src/core/read.c`) — `downloadFile` into a
  bounded on-disk cache, a fetcher injected by `main.c` so `tboxcore` stays
  TDLib-free, and partial reads so an FTP client that cancels mid-transfer
  leaves no half-written file behind.
- **`index.json`** (`src/core/indexfile.c`) — the index as a file, replaced
  atomically via `.tmp` + rename, with short keys and the hashes omitted since
  they are re-derived from the captions. This is what makes the tree browsable
  before the first scan of a session finishes.
- **One TDLib owner** (`src/td/tdbridge.c`) — a single thread owns `td_receive`
  and hands everything else a request/reply API keyed by `@extra`, so the scan
  loop, the FTP session downloading a body and the rescan can all be asking at
  once without violating TDLib's one-reader rule.

### Changed

- **The scan uses `getChatHistory`, not `searchMessages`.** The plan called for a
  document-filtered `searchMessages`, but that only ever searches TDLib's
  *local* database — on a cold session it finds nothing, and warming it first
  means downloading the history anyway. Paging `getChatHistory` reads from the
  server and, because the scan is a full rebuild, also sees deletions.
- `downloadFile` is addressed by `File.id`, the number at
  `content.document.document.id`. This generation of TDLib answers
  `File not found` for the base64 `remote.id` and for `remote_file_id`, and only
  honours `file_id`. Those ids are rebuilt by every scan, so a transfer always
  uses a handle this session knows.
- Where the interesting fields live in current TDLib is documented in
  `src/td/tdscan.h`, because none of them is where the older documentation says:
  the caption is `content.caption.text` (a sibling of `document`, not inside
  it), the size is `content.document.document.expected_size`, and `Document`
  carries no size of its own.
- Credentials come from `TG_API_ID` / `TG_API_HASH` **only**. The `.env`
  machinery, the CMake cache variables and the `TBOX_TG_API_*` compile
  definitions are gone, and the hash is validated as exactly 32 hex digits.
  Nothing about the app's identity is compiled into the binary, so one build
  serves every account.
- `src/td/tdparams.c` refactored onto `src/util/datadir.c`, so the TDLib-free
  command layer resolves byte-identical paths to the login flow.
- `util/tbox_signal.h` and `util/tbox_thread.h` became one header each with a
  Windows and a POSIX backend, so the tests are the same files on every OS.
- A directory in a listing now shows its marker's date when it has one, instead
  of always claiming Jan 1970. A directory that exists only because something
  deeper sits in it still has no date, because there is none to show.

### Fixed

- **Every download was reported as failed.** TDLib's flags are real JSON
  booleans, and cJSON does not count a boolean as a number, so
  `cJSON_IsNumber()` is false for both `true` and `false`. The progress table
  therefore never saw `is_downloading_active` or `is_downloading_completed` at
  all, kept both flags at zero, and declared the very first `updateFile` a
  failure. The flags are now read as booleans, and a download is only called
  failed once it has been seen *running* and then goes quiet.
- **`tbox serve` failed to start at random, about once every 256 runs.** The FTP
  token was generated with an all-`'0'` sentinel and the server rejected any
  token *starting* with `00` as a guessable failure — which a real 128-bit token
  does once every 256 runs. The generator now reports success or failure
  explicitly, so no caller has to infer it from the value. Regression-tested by
  drawing tokens until one begins `00` and asserting it is accepted.
- The first `getChatHistory` page is retried a few times: on a session whose
  database has never held Saved Messages, the chat is created while the database
  opens and a request that arrives first is answered `Chat not found`, which is
  a race with the load rather than a refusal.

- `tbox serve` no longer blames TDLib for having nothing to serve, and a build
  bug is now reported as one. A missing archive runner and a TDLib-less build
  are different problems, and the binary knows which it is
  (`tbox_cmd_set_tdlib_present`, set by `main.c`), so the message says which
  instead of pointing a user with a working TDLib build at
  `scripts/fetch-tdjson.*`.
- A `snprintf("%s/%s")` truncation that gcc's `-Wformat-truncation` turned into
  a `-Werror` build failure in `tbox_td_prepare_dir`.
- The vendored 2026 `td_json_client.h` no longer declares
  `td_set_log_verbosity_level`; logging is now configured with
  `td_execute("{\"@type\":\"setLogVerbosityLevel\",\"new_level\":0}")`.

### Known limitations

- The FTP server is **read-only**. `STOR`, `DELE`, `MKD`, `RMD`, `RNFR` and
  `RNTO` are not built yet, so the upload path does not exist and an archive can
  be browsed and downloaded but not written. SFTP is not started.
- The session database is TDLib's default **plain** storage; sealing it and
  moving the key into the OS credential store is still ahead.
- Only Windows TDLib prebuilts are vendored, so releases are Windows-only for
  now. See the note in `.github/workflows/release.yml`.

## [0.4.x] and earlier

The Python/CLI-only generation this rewrite replaces. Its archive layout is
still honoured by the caption ledger in `src/core/index.c`, which is why the
schema is byte-compatible with it.

[Unreleased]: https://github.com/3rang/tbox/compare/v0.4.0...HEAD
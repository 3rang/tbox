# TDLib Reference Shelf — v0.5

Official TDLib documentation, fetched for offline reference.
**No code lives here** — this is your API cheat-sheet for designing/writing `src/td/*`.

## Files

| File | What it is | Size |
|---|---|---|
| `td_json_client.h` | The official C API header (the 8 core functions). Fetched from `tdlib/td` master. | 9 KB |
| `td_api.tl` | **The complete TDLib API schema** — every method, type, update, and their fields, in TL notation. This is the authoritative list of everything you can send and receive. | 1.15 MB |
| `getting-started.md` | Official tutorial: concepts, authorization flow, updates, files, chats. | — |

## Where to find things inside `td_api.tl`

TL notation explained: `functionName arg:Type arg2:Type2 = ReturnType;`
Line numbers below are from the fetched copy (checked 2026-09-23).

### Authorization states (lines ~198–251)
Search: `^authorizationState`

- `authorizationStateWaitTdlibParameters` (198) — first state, sent after client create
- `authorizationStateWaitPhoneNumber` (202)
- `authorizationStateWaitEmailAddress` (214) — new-ish, email login path
- `authorizationStateWaitCode` (224) — code_info has the delivery method
- `authorizationStateWaitOtherDeviceConfirmation` (227) — **QR login**: contains the `link`
- `authorizationStateWaitPassword` (238) — 2FA/setup password
- `authorizationStateWaitRegistration` (230)
- `authorizationStateReady` (241)
- `authorizationStateClosed` (251)

### Auth methods
Search: `^setTdlibParameters`, `^setAuthenticationPhoneNumber`, `^checkAuthenticationCode`,
`^checkAuthenticationPassword`, `^requestQrCodeAuthentication`, `^checkAuthenticationEmailCode`,
`^setDatabaseEncryptionKey`

- `setTdlibParameters ... = Ok;` (line 11306) — note it takes a `database_encryption_key:bytes` field (v0.2 Tier-2 sealed-session usage)
- `requestQrCodeAuthentication` — triggers the QR flow for `login --qr`

### tbox-relevant core methods
| Thing to do | Search for | Return |
|---|---|---|
| Who am I | `^getMe` (11496) | `User` |
| List documents in Saved Messages | `^searchMessages` (11878) | `FoundMessages` — filter by `searchMessagesFilterDocument` (6285) |
| Send a file to Saved Messages | `^sendMessage` (12201) | `Message` |
| Upload a file | `^uploadFile` | `File` |
| Download a file | `^downloadFile` (13986) | `File` |
| File progress updates | `^updateFile` | `Update` — the hook for progress bars |
| Pause/resume uploads | `^uploadFile` + `update` flag | `File` |
| What chats matter | `chatTypePrivate`/Saved Messages chat | `chat_id` of `self` |

### Updates (the events TDLib pushes at you)
Search: `^update` — e.g.
- `updateAuthorizationState` (10398)
- `updateFile` — essential for upload/download progress
- `updateNewMessage`, `updateMessageSendSucceeded` — confirm sends
- `updateNewChat`, `updateUser` — caches ("comes before the id is returned to the app")

### Files & uploading
- `InputMessageContent` types: `inputMessageDocument` = "file with caption" → telegram stores Document
  messages; matches the v0.3 caption-ledger design (`{"v":1,"type":"file",...}`)
- `InputFile` types: `inputFileLocal` (path), `inputFileRemote`, `inputFileGenerated`
- `File` object: `pending_upload_size`, `uploaded_size`, `download_offset` → progress tracking
- `localFile` / `remoteFile` sub-objects

## Other official links (no local copy)
- Methods index (browser): https://core.telegram.org/tdlib/docs/classtd_1_1td__api_1_1_function.html
- Updates index: https://core.telegram.org/tdlib/docs/classtd_1_1td__api_1_1_update.html
- TDLib homepage: https://core.telegram.org/tdlib

## C API reminders (from td_json_client.h)
- New-style (use this): `td_create_client_id()` → `td_send(id, json)` / `td_receive(timeout)` / `td_execute(json)`
- Old-style (deprecated, easy-tg uses it): `td_json_client_create/send/receive/execute/destroy`
- Match a response to a request via the `"@extra"` field you put in the request.
- Every received object has `"@client_id"`; responses and updates arrive on the
  **single** `td_receive` loop thread, in order — never call `td_receive` from two threads.
- `td_execute` is for synchronous requests only (e.g. log level, get version).

## Note for your design
For tbox, list-and-download in Saved Messages = "the chat with yourself". You'll get
`updateAuthorizationState` → auth → then `searchMessages(filter=searchMessagesFilterDocument)`
plus `inputMessageDocument` + `uploadFile`/`downloadFile` for the file ops, and
`updateFile` for progress. All JSON in, JSON out.
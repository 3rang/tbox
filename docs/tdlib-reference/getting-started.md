# Getting started with TDLib

> Source: https://core.telegram.org/tdlib/getting-started (fetched 2026-09-23)

**TDLib** is a fully functional Telegram client which takes care of all networking,
local storage and data consistency details.

## TDLib interface

- *Client* = an interface for interaction with a TDLib instance.
- *Application* = the program that uses TDLib to interact with Telegram.
- The main API is **fully-asynchronous**: an *Application* sends a request via
  `ClientManager.send` and receives a response asynchronously through
  `ClientManager.receive`.
- In the **JSON** interface those methods are `td_send` and `td_receive`, and the
  **`@extra` field must be used to match requests with the corresponding responses**.
- The *Application* also receives lots of important data through incoming
  **updates** — correct handling of updates is crucial.

Links to browser docs:
- [List of all available TDLib API methods](https://core.telegram.org/tdlib/docs/classtd_1_1td__api_1_1_function.html)
- [Full TDLib API scheme = td_api.tl](https://github.com/tdlib/td/blob/master/td/generate/scheme/td_api.tl)
  (local copy: `td_api.tl` in this folder)

## Glossary (essentials)

- **message** — belongs to a chat, unique id within the chat, >45 content kinds
  (messageText, messagePhoto, messageScreenshotTaken, ...)
- **user** — unique id, first name, optional last name/username/profile photo
- **chat** — members; 4 chat types: private (incl. "Saved Messages" = yourself),
  basic group (≤200), supergroup (incl. channels/broadcast, unlimited), secret chat
- **file** — id, may be local (on disk) or remote (cloud); downloadable/uploadable
- **caption** — short text on media messages, can hold text entities
- **formattedText** = text + entities

## User authorization (the flow for `login --qr`)

- Driven entirely by the **`updateAuthorizationState`** update.
- First state is always `authorizationStateWaitTdlibParameters` → call `setTdlibParameters`
  with api_id, api_hash, database_directory, use_message_database, use_secret_chats,
  system_language_code, device_model, etc.
- If `setTdlibParameters` fails, the state does NOT change — handle the current
  state again.
- Then, depending on path: `authorizationStateWaitPhoneNumber`,
  `authorizationStateWaitEmailAddress`, `authorizationStateWaitEmailCode`,
  `authorizationStateWaitCode`, `authorizationStateWaitRegistration` or
  `authorizationStateWaitPassword`.
- After all steps: **`authorizationStateReady`** → ordinary requests can be sent.

## Sending a message

- Call `sendMessage(chat_id, content)`; use `inputMessageText` / `inputMessagePhoto` /
  `inputMessageDocument` etc.; use `inputFileLocal` to send a local file.

## Handling updates (order matters!)

All updates and responses must be handled **in the order received**. Important ones:

- `updateAuthorizationState` — essential for auth
- `updateNewChat` / `updateUser` / `updateBasicGroup` / `updateSupergroup` /
  `updateSecretChat` — caches; guaranteed to come BEFORE the id is returned to the
  application, so maintain caches and never fetch again
- `updateNewMessage` — new message added
- `updateMessageSendSucceeded`
- `updateMessageContent`
- `updateFile` — **essential for upload/download progress**
- a large family of `updateChat*` — maintain the chat cache

## Getting the lists of chats

- 3 chat lists: Main, Archive, Filter(folder)
- `chat.positions` is maintained by TDLib; sort by `(position.order, chat.id)` desc
- call `loadChats` only if more chats are needed (limit may be smaller than requested)

## Getting chat messages

- `getChatHistory(chat_id, from_message_id, offset, limit)` — reverse chronological.
- `from_message_id == 0` = start from the last message; paginate by passing the last
  received message id as `from_message_id`.

---

**For tbox specifically:** "Saved Messages" = your own private chat with yourself.
`chatTypePrivate` with `user_id == self`. Everything else in the file/upload world
is covered in `README.md` of this folder and the `td_api.tl` schema.
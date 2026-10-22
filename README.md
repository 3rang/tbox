<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2026 Tarang Patel -->
# tbox

Telegram from the command line, in under 2000 lines of plain C.

`tbox` is a small, readable example of talking to Telegram with
[TDLib](https://github.com/tdlib/td) from C: log in as a normal user
account, send and read messages, upload a file. It is meant to be read as
much as run - every file does one thing and is commented for that.

```
$ tbox login
Phone number (international format, e.g. +41791234567): +41791234567
A login code was sent via your other Telegram app.
Code: 12345
Two-step verification is on. Hint: usual one
Password:
Logged in.
id:       123456789
name:     Tarang Patel
username: @tarang

$ tbox send @alice "hi from the terminal"
sent to 987654321 (message 4711)

$ tbox read @alice -n 3
[2026-10-22 16:08] Alice: are you coming?
[2026-10-22 16:09] Tarang Patel: hi from the terminal
[2026-10-22 16:09] Alice: ✨

$ tbox upload me ~/backup.tar.gz "weekly backup"
uploading 10485760 / 10485760 bytes (100%)
uploaded /home/me/backup.tar.gz to 123456789 (message 4712)
```

## Commands

| Command | What it does |
|---|---|
| `tbox login [--api-id ID --api-hash HASH]` | sign in once; asks for phone, login code and 2FA password |
| `tbox logout` | sign out and delete the local session |
| `tbox me` | show the logged-in account |
| `tbox search <query>` | find chats by title (yours) or public `@username`, with their ids |
| `tbox send <chat> <text...>` | send a text message |
| `tbox read <chat> [-n COUNT]` | print the last messages, oldest first (default 20) |
| `tbox upload <chat> <file> [caption...]` | send a file as a document |

`<chat>` can be written as:

| Form | Example | Means |
|---|---|---|
| `me` or `saved` | `tbox upload me notes.txt` | your Saved Messages |
| `@username` or `t.me/name` | `tbox read @telegram` | a public user, group or channel |
| a chat id | `tbox send -1001234567890 hello` | the id as `tbox search` prints it |
| text | `tbox read "Family"` | matched against the titles of your chats |

Exit codes: `0` ok, `1` error, `2` usage, `3` not logged in.

## Install

Download the archive for your OS from the
[releases page](https://github.com/3rang/tbox/releases), unpack it and run
`bin/tbox`. The TDLib runtime (`libtdjson`) is in the same folder, nothing
else to install.

| Archive | For |
|---|---|
| `tbox-<ver>-linux-x64.tar.gz`, `-linux-arm64` | Linux (needs OpenSSL 3 and zlib, present on any current distro) |
| `tbox-<ver>-osx-arm64.tar.gz`, `-osx-x64` | macOS 12+ (Apple silicon / Intel) |
| `tbox-<ver>-win-x64.zip` | Windows 10/11 |

On macOS the first run may be blocked by Gatekeeper because the binaries
are not notarised: `xattr -dr com.apple.quarantine <unpacked folder>`.

## First run: API credentials

Telegram wants every client to identify with an **api_id** and
**api_hash**. Get yours in two minutes at
<https://my.telegram.org/apps> (log in with your phone, create an app, any
name and platform). `tbox login` asks for them the first time and stores
them in `~/.tbox/config`. Alternatively:

```sh
tbox login --api-id 12345 --api-hash 0123456789abcdef0123456789abcdef
# or
export TBOX_API_ID=12345 TBOX_API_HASH=0123456789abcdef...
```

Everything tbox keeps lives in `~/.tbox` (`TBOX_HOME` to change it):

```
~/.tbox/config   api_id, api_hash
~/.tbox/tdlib/   TDLib's database: your session, chats and message cache
```

The session is a real Telegram login (it shows up under *Devices* in the
Telegram app as "tbox"). `tbox logout` ends it and deletes `tdlib/`.

## Build from source

Requirements: CMake 3.18+, a C11 compiler (gcc, clang or MSVC), network
access during the first configure so CMake can download the prebuilt TDLib.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release        # unit tests, no account needed
cmake --install build --prefix dist      # dist/bin/tbox + libtdjson
```

CMake fetches the pinned TDLib (`libtdjson`, from the
[tdlib.native](https://github.com/ForNeVeR/tdlib.native) packages, sha256
checked) for your OS and copies it next to the executable. To use your own
build of TDLib instead:

```sh
cmake -B build -DTBOX_TDJSON_DIR=/path/to/dir/containing/libtdjson.so
```

At run time tbox looks for `libtdjson` next to its own executable, then at
`$TBOX_TDJSON`, then on the system library path.

## How it works

TDLib does all the hard parts (MTProto, encryption, the database). tbox
drives it through the
[JSON interface](https://core.telegram.org/tdlib/docs/td__json__client_8h.html):
send a JSON request, receive JSON replies and updates. The code is laid out
along that line:

| File | Role |
|---|---|
| [`src/td.c`](src/td.c) | loads `libtdjson` at run time, `td_request()` = send + wait for the matching reply, hands updates to a handler |
| [`src/auth.c`](src/auth.c) | the login state machine: `WaitTdlibParameters → WaitPhoneNumber → WaitCode → WaitPassword → Ready` |
| [`src/config.c`](src/config.c) | `~/.tbox`, the config file and the environment |
| [`src/chat.c`](src/chat.c) | `@username` / id / `me` / title → chat id |
| [`src/message.c`](src/message.c) | a message object → one line of text |
| [`src/outbox.c`](src/outbox.c) | waits for `updateMessageSendSucceeded`, shows upload progress |
| [`src/session.c`](src/session.c) | what every command does first and last |
| [`src/cmd_*.c`](src) | one file per command |
| [`src/platform.c`](src/platform.c) | the Windows/POSIX differences: dlopen, home dir, hidden input |
| [`tests/`](tests) | a scripted fake `libtdjson` and the unit tests |

A command runs like this:

1. `session_open()` loads the config and `libtdjson`, creates a client,
   and runs `auth_run()`. With a saved session that is one
   `setTdlibParameters` and TDLib answers `authorizationStateReady`.
   Otherwise the command stops with exit code 3.
2. The command sends its requests (`sendMessage`, `getChatHistory`, ...).
3. `session_close()` sends `close` and waits for
   `authorizationStateClosed` so TDLib flushes its database.

Set `TBOX_DEBUG=1` to see every JSON message going in and out - the best
way to learn the API.

## Using this in your own project

Copy `src/td.c`, `src/td.h`, `src/json.h`, `src/platform.c` and
`third_party/cjson`; that is the whole bridge. `tests/fake_td.c` shows how
to unit-test code that talks to TDLib without a network or an account.

## License

Apache-2.0, see [LICENSE](LICENSE). Third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

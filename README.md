<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2026 Tarang Patel -->
# tbox

Telegram from the command line, in a small amount of plain C.

`tbox` is a readable example of talking to Telegram with
[TDLib](https://github.com/tdlib/td) from C, as a normal user account:
log in by scanning a QR code, send a message, upload a file, get notified
of new messages. Five commands, no framework, every file does one thing.

```
$ tbox login
Scan this QR code with Telegram on your phone:
Settings > Devices > Link Desktop Device

█████████████████████████████████
██ ▄▄▄▄▄ █▀█ █▄▀▄▀▄██ ▄▄▄▄▄ ██
██ █   █ █▀▀▀█ ▀▄ ▀▄█ █   █ ██
...
Waiting for the scan... (the code refreshes every ~30 s)
Two-step verification is on. Hint: usual one
Password:
Logged in as Tarang Patel (@tarang), id 123456789

$ tbox send @alice "hi from the terminal"
sent to 987654321 (message 4711)

$ tbox upload me ~/backup.tar.gz "weekly backup"
uploading 10485760 / 10485760 bytes (100%)
uploaded /home/me/backup.tar.gz to 123456789 (message 4712)

$ tbox watch
watching... press Ctrl-C to stop
[2026-10-31 23:02] Alice (987654321): are you coming?
[2026-10-31 23:05] Family (-1001234567890) Bob: dinner at 8
```

## Commands

| Command | What it does |
|---|---|
| `tbox login [--api-id ID --api-hash HASH]` | sign in once by scanning a QR code; asks the 2FA password if you have one |
| `tbox logout` | sign out and delete the local session |
| `tbox send <chat> <text...>` | send a text message |
| `tbox upload <chat> <file> [caption...]` | send a file as a document |
| `tbox watch [chat]` | print new incoming messages as they arrive, from all chats or one |

`<chat>` is one of:

| Form | Example | Means |
|---|---|---|
| `me` or `saved` | `tbox upload me notes.txt` | your Saved Messages |
| `@username` or `t.me/name` | `tbox send @alice hi` | a public user, group or channel |
| a chat id | `tbox send -1001234567890 hello` | the number `tbox watch` prints in parentheses |

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
**api_hash**. Get yours at <https://my.telegram.org/apps> (log in with your
phone, create an app, any name and platform). `tbox login` asks for them
the first time and stores them in `~/.tbox/config`. Alternatively:

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

The session is a real Telegram login; it shows up under *Devices* in the
Telegram app as "tbox" and can be revoked there. `tbox logout` ends it and
deletes `tdlib/`.

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

TDLib does the hard parts (MTProto, encryption, the local database). Its
C interface is four functions - `td_send`, `td_receive`, `td_execute`,
`td_create_client_id` - and everything passing through them is a JSON
string. tbox is organised along that line:

```
src/
  main.c            command table and usage
  core/             things every command needs
    td.c            loads libtdjson at run time; td_request() = send + wait for the reply
    json.h          ten-line helpers over cJSON (jstr, jint, jobj, ...)
    config.c        ~/.tbox, the config file, the environment
    session.c       open (load, authorize) and close (flush) around each command
    platform.c      the Windows/POSIX differences: dlopen, home dir, hidden input, colours
  tg/               Telegram concepts
    auth.c          the login state machine: WaitTdlibParameters -> QR -> [password] -> Ready
    qr.c            draws the tg://login link as a QR code in the terminal
    chat.c          me / @username / id -> chat id; names of chats and users
    message.c       a message object -> one line of text
    outbox.c        waits for updateMessageSendSucceeded; upload progress
  cmd/              one file per command: login, logout, send, upload, watch
tests/              a scripted fake libtdjson and the unit tests
third_party/        cJSON (MIT) and Nayuki's QR encoder (MIT)
```

A command runs like this:

1. `session_open()` loads the config and `libtdjson`, creates a client,
   and runs `auth_run()`. With a saved session that is one
   `setTdlibParameters` and TDLib answers `authorizationStateReady`.
   Otherwise the command stops with exit code 3.
2. The command sends its requests (`sendMessage`, ...) and waits for the
   updates it cares about (`updateMessageSendSucceeded`, `updateNewMessage`).
3. `session_close()` sends `close` and waits for
   `authorizationStateClosed` so TDLib flushes its database.

Set `TBOX_DEBUG=1` to see every JSON message going in and out - the
quickest way to learn the TDLib API.

### Why cJSON?

Because TDLib's C API *is* JSON. There is no C struct interface; the
alternative is TDLib's C++ API, which means a C++ project and compiling
TDLib's headers. Parsing JSON by hand would be more code than cJSON's
single file and easier to get wrong, so cJSON (3k lines, MIT, vendored)
is the smallest honest dependency. `src/core/json.h` wraps it so the rest
of tbox reads like `jstr(msg, "text")`.

### Why QR login?

It is the shortest flow TDLib offers: no phone number, no SMS code.
`requestQrCodeAuthentication` makes TDLib hand us a `tg://login?token=...`
link, we draw it, the phone scans it, and TDLib moves to `Ready` (or to
`WaitPassword` if two-step verification is on). The link rotates every
~30 seconds and tbox redraws it.

## Using this in your own project

Copy `src/core/td.c`, `src/core/td.h`, `src/core/json.h`,
`src/core/platform.c` and `third_party/cjson`; that is the whole bridge.
`src/tg/auth.c` is the login flow. `tests/fake_td.c` shows how to
unit-test code that talks to TDLib without a network or an account.

## License

Apache-2.0, see [LICENSE](LICENSE). Third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

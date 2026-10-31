<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2026 Tarang Patel -->
# tbox

Telegram from the command line, in a small amount of plain C.
Project page: <https://3rang.github.io/tbox/>

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
| `tbox login` | sign in once by scanning a QR code; asks the 2FA password if you have one |
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

Telegram wants every client *application* to identify with an **api_id**
and **api_hash**. Get yours at <https://my.telegram.org/apps> (log in with
your phone, create an app, any name and platform), then put them in the
environment - your shell profile is the right place:

```sh
export TBOX_API_ID=12345
export TBOX_API_HASH=0123456789abcdef0123456789abcdef
```

Every command needs them (TDLib sends them on each start), and tbox
prints exactly this instruction if they are missing. It never writes them
to disk. If you build tbox yourself you can compile your pair in as the
default instead (`-DTBOX_API_ID=... -DTBOX_API_HASH=...`, see below); the
environment still overrides it.

Everything tbox keeps lives in `~/.tbox` (`TBOX_HOME` to change it):

```
~/.tbox/tdlib/   TDLib's database: your session, chats and message cache
```

The session is a real Telegram login; it shows up under *Devices* in the
Telegram app as "tbox" and can be revoked there. `tbox logout` ends it and
deletes `tdlib/`.

## Build from source

Requirements: CMake 3.18+, a C11 compiler (gcc, clang or MSVC), network
access during the first configure.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release        # unit tests, no account needed
cmake --install build --prefix dist      # dist/bin/tbox (+ libtdjson)
```

TDLib is linked at compile time, in one of two ways
([cmake/tdlib.cmake](cmake/tdlib.cmake)):

| | What happens | Result |
|---|---|---|
| default | CMake downloads the pinned prebuilt `libtdjson` for your OS/CPU from the [tdlib.native](https://github.com/ForNeVeR/tdlib.native) packages (sha256 checked) and links against it | `tbox` + `libtdjson` side by side; fast build |
| `-DTBOX_TDLIB_FROM_SOURCE=ON` | CMake fetches TDLib's sources at the same version and compiles them into tbox statically; needs a C++17 compiler, OpenSSL, zlib and `gperf`, and 10-40 minutes | one self-contained `tbox`, nothing beside it |

Useful options:

```sh
-DTBOX_TDJSON_DIR=/dir/with/libtdjson.so   # use a prebuilt you already have
-DTBOX_API_ID=12345 -DTBOX_API_HASH=...     # compile your credentials in as the default
```

## How it works

TDLib does the hard parts (MTProto, encryption, the local database). Its
C interface is four functions - `td_send`, `td_receive`, `td_execute`,
`td_create_client_id` - linked in at build time, and everything passing
through them is a JSON string. tbox is organised along that line:

```
src/
  main.c            command table and usage
  core/             things every command needs
    td.c            the four TDLib calls behind a table (so tests can fake them);
                    td_request() = post a request + wait for its reply
    json.h          ten-line helpers over cJSON (jstr, jint, jobj, ...)
    config.c        ~/.tbox and the API credentials from the environment
    session.c       open (load, authorize) and close (flush) around each command
    platform.c      the Windows/POSIX differences: home dir, hidden input, console colours
  tg/               Telegram concepts
    auth.c          the login state machine: WaitTdlibParameters -> QR -> [password] -> Ready
    qr.c            draws the tg://login link as a QR code in the terminal
    chat.c          me / @username / id -> chat id; names of chats and users
    message.c       a message object -> one line of text
    outbox.c        sendMessage + wait for updateMessageSendSucceeded; upload progress
  cmd/              one file per command: login, logout, send, upload, watch
tests/              a scripted fake libtdjson and the unit tests
third_party/        cJSON (MIT) and Nayuki's QR encoder (MIT)
docs/index.html     the project page, published to gh-pages by .github/workflows/pages.yml
```

A command runs like this:

1. `session_open()` reads the environment, creates a TDLib client and
   runs `auth_run()`. With a saved session that is one
   `setTdlibParameters` and TDLib answers `authorizationStateReady`.
   Otherwise the command stops with exit code 3.
2. The command sends its requests (`sendMessage`, ...) and waits for the
   updates it cares about (`updateMessageSendSucceeded`, `updateNewMessage`).
3. `session_close()` sends `close` and waits for
   `authorizationStateClosed` so TDLib flushes its database.

Set `TBOX_DEBUG=1` to see every JSON message going in and out (passwords
and the api_hash blanked) plus TDLib's own warnings - the quickest way to
learn the TDLib API.

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

### Why no config file for the credentials?

api_id/api_hash identify the *application*, not you, and a secret copied
into a file in every user's home is a secret spread around. The
environment keeps it in one place you control; a build you distribute can
carry its own pair compiled in, the way the official Telegram apps do.
Your *login* is a different thing - that is TDLib's database in
`~/.tbox/tdlib`, created by the QR scan and removed by `tbox logout`.

## Using this in your own project

Copy `src/core/td.c`, `src/core/td.h`, `src/core/json.h`,
`cmake/tdlib.cmake` and `third_party/cjson`; that is the whole bridge.
`src/tg/auth.c` is the login flow. `tests/fake_td.c` shows how to
unit-test code that talks to TDLib without a network or an account.

## License

Apache-2.0, see [LICENSE](LICENSE). Third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

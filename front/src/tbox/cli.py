# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""The tbox command line — turning engine events into something readable.

All Telegram work happens in the C engine; this file only decides what a
person sees and what they are asked. Effects are deliberately easy to follow:
one event in, at most one line out.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import getpass
import json
import sys
import threading
import time

from . import config, paths, qr
from .engine_client import EngineClient, EngineError
from .errors import TboxError

# States that would only add noise: each has a message of its own already.
_QUIET_STATES = {
    "WAIT_PARAMS",
    "WAIT_PHONE",
    "WAIT_QR",
    "WAIT_PASSWORD",
    "READY",
    "CLOSED",
}

#: The four places the catalog has, and how they are called on screen.
_FOLDER_TITLES = {
    "root": "Root",
    "inbox": "Inbox",
    "trash": "Trash",
    "lost": "Lost & Found",
}

#: How long the CLI waits for the engine to sign in.
AUTH_TIMEOUT = 120.0

#: How long a rescan may run before the CLI stops waiting for it.
RESCAN_TIMEOUT = 3600.0


def say(message: str = "") -> None:
    print(message, flush=True)


class _Session:
    """What the CLI watches while the engine works.

    The events arrive on the engine client's reader thread, so whatever they
    carry is kept under a lock. The engine may still be reading the whole
    history when the account becomes ready, and that is progress worth
    showing, so the scan events are watched all the time.
    """

    def __init__(self) -> None:
        self.ready = threading.Event()
        self.needs_login = False
        self.error: str | None = None
        self._done: list[dict] = []
        self._lock = threading.Lock()

    def handle(self, event: dict) -> None:
        name = event.get("event")
        data = event.get("data") or {}
        if name == "auth.ready":
            self.ready.set()
        elif name == "auth.qr":
            self.needs_login = True
            self.ready.set()
        elif name == "auth.closed":
            self.error = "the session was closed"
            self.ready.set()
        elif name == "auth.state":
            if str(data.get("state", "")) == "CLOSED":
                self.error = "the session was closed"
                self.ready.set()
        elif name == "engine.error":
            self.error = str(data.get("message", "unknown error"))
        elif name == "cooldown":
            say(f"  Telegram asked us to slow down for {data.get('seconds', 0)}s")
        elif name == "sync.progress":
            say(f"  scanning... {data.get('messages_scanned', 0)} messages")
        elif name == "sync.done":
            with self._lock:
                self._done.append(data)

    def done_for(self, op_id: str) -> dict | None:
        """The sync.done of one operation, if it has arrived."""
        with self._lock:
            for data in self._done:
                if str(data.get("op_id", "")) == op_id:
                    return data
        return None


@contextlib.contextmanager
def signed_in_engine(session: _Session):
    """Starts the engine, waits until the account is ready, yields the client.

    The session on disk is reused, so a machine that has signed in before
    signs in again without a QR code. A machine that has not is told to run
    ``tbox login`` rather than shown a code nobody asked for.
    """
    try:
        api_id = config.api_id()
        api_hash = config.api_hash()
    except TboxError as exc:
        raise TboxError(f"{exc} - run 'tbox setup' first") from exc

    client = EngineClient(str(paths.session_dir()), on_event=session.handle)
    client.start()
    try:
        client.auth_start(api_id, api_hash, config.db_key())
        if not session.ready.wait(AUTH_TIMEOUT):
            raise TboxError("the engine did not sign in in time")
        if session.needs_login:
            raise TboxError("this computer is not signed in - run 'tbox login'")
        if session.error is not None:
            raise TboxError(session.error)
        yield client
    finally:
        client.close()


def login_screen(
    core,
    verbose: bool = False,
    prompt_password=None,
) -> int:
    """Drive a login and render it, returning an exit code.

    Accepts either an :class:`~tbox.engine_client.EngineClient` (long-running
    ``serve``) or the legacy :class:`~tbox.core.Core` (used by tests with a
    scripted engine).
    """
    if prompt_password is None:
        prompt_password = lambda hint: getpass.getpass(
            "Your Telegram two-step password: "
        )

    say("Waiting for Telegram...")
    state = {"linked": False, "error": None}
    done = threading.Event()

    def handle_event(event: dict) -> None:
        if verbose:
            say("  " + json.dumps(event, ensure_ascii=False))
        name = event.get("event")
        data = event.get("data") or {}

        if name in ("auth.state", "auth_state"):
            value = str(data.get("state", event.get("state", "")))
            if value not in _QUIET_STATES:
                say(f"  state: {value}")
            if value == "CLOSED":
                done.set()

        elif name in ("auth.closed", "closed"):
            say("Session closed.")
            done.set()

        elif name in ("auth.qr", "qr_link"):
            link = data.get("link", event.get("link", ""))
            say()
            say(qr.render(str(link)))
            say()
            say("Scan this with your phone: Settings > Devices > Link Desktop Device")
            say("(the code refreshes by itself, so just scan the newest one)")

        elif name in ("auth.password_needed", "password_needed"):
            hint = str(data.get("hint", event.get("hint", "")))
            if hint:
                say(f"  hint: {hint}")
            try:
                if hasattr(core, "auth_password"):
                    core.auth_password(prompt_password(hint))
                else:
                    core.send(prompt_password(hint))
            except TboxError as exc:
                state["error"] = str(exc)
                done.set()

        elif name in ("auth.ready", "ready"):
            state["linked"] = True
            who = str(data.get("display_name", event.get("user", ""))).strip()
            say(f"Linked as {who}." if who else "Linked.")

        elif name in ("engine.error", "error"):
            message = str(data.get("message", event.get("message", "unknown error")))
            state["error"] = message
            say(f"error: {message}" + (f" (code {data.get('code', event.get('code'))})" if data.get("code") is not None or event.get("code") is not None else ""))
            done.set()

        elif name == "cancelled":
            say("Cancelled.")
            done.set()

    if hasattr(core, "set_event_handler"):
        core.set_event_handler(handle_event)
        while not done.is_set():
            done.wait(0.25)
    elif hasattr(core, "events"):
        # legacy Core with a blocking event stream (tests)
        try:
            for event in core.events():
                handle_event(event)
                if done.is_set():
                    break
        except Exception as exc:
            state["error"] = str(exc)
    else:
        raise TypeError("login_screen needs a Core or an EngineClient")

    if state["error"] is not None and not state["linked"]:
        return 1
    return 0 if state["linked"] else 1


def cmd_login(args: argparse.Namespace) -> int:
    try:
        api_id = config.api_id()
        api_hash = config.api_hash()
    except TboxError:
        if not sys.stdin.isatty():
            say("error: no Telegram credentials yet - run 'tbox setup' first")
            return 1
        say("First run: let's save your Telegram credentials.")
        say()
        if cmd_setup(args) != 0:
            return 1
        say()
        api_id = config.api_id()
        api_hash = config.api_hash()

    warn = config.migration_warning()
    if warn:
        say(f"warning: {warn}")

    session = paths.session_dir()
    say(f"session: {session}")
    with EngineClient(str(session)) as client:
        client.start()
        client.auth_start(api_id, api_hash, config.db_key())
        return login_screen(client, verbose=args.verbose)


def cmd_logout(args: argparse.Namespace) -> int:
    """Log out on Telegram, leaving this computer's data alone."""
    session = paths.session_dir()
    try:
        with EngineClient(str(session)) as client:
            client.start()
            client.auth_logout()
    except TboxError as exc:
        say(f"error: {exc}")
        return 1
    say("Logged out. Telegram sessions for this app are ended;")
    say("use 'tbox setup' then 'tbox login' to link again.")
    return 0


def _format_size(value: object) -> str:
    """A size in bytes, in words, or a dash when it is not known."""
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value < 0:
        return "-"
    size = float(value)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if size < 1024 or unit == "TB":
            return f"{int(size)} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return "-"


def _format_time(value: object) -> str:
    """A unix timestamp as a local date and time, or a dash."""
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
        return "-"
    stamp = datetime.datetime.fromtimestamp(float(value))
    return stamp.strftime("%Y-%m-%d %H:%M")


def _print_items(folder: str, items: list[dict]) -> None:
    """Print one folder's items the way a person reads a list."""
    title = _FOLDER_TITLES.get(folder, folder)
    if not items:
        say(f"{title}: nothing here")
        return
    names: list[str] = []
    for item in items:
        name = str(item.get("name") or item.get("orig_name") or "?")
        if item.get("kind") == "d":
            name += "/"
        names.append(name)
    width = max(len(name) for name in names)
    say(f"{title}:")
    for name, item in zip(names, items):
        if item.get("kind") == "d":
            size = "-"
        else:
            size = _format_size(item.get("size"))
        when = _format_time(item.get("mtime") or item.get("date"))
        say(f"  {name.ljust(width)}  {size:>9}  {when}")


def cmd_ls(args: argparse.Namespace) -> int:
    """List what is in a folder of the catalog."""
    folder = str(args.folder or "root")
    session = _Session()
    with signed_in_engine(session) as client:
        items = client.catalog_list(folder)
        _print_items(folder, items)
    return 0


def cmd_rescan(args: argparse.Namespace) -> int:
    """Read Saved Messages again, from the oldest message to the newest."""
    session = _Session()
    with signed_in_engine(session) as client:
        op_id = client.sync_rescan()
        deadline = time.monotonic() + RESCAN_TIMEOUT
        while time.monotonic() < deadline:
            data = session.done_for(op_id)
            if data is not None:
                objects = int(data.get("objects", 0) or 0)
                inbox = int(data.get("inbox_items", 0) or 0)
                say(f"done: {objects} objects, {inbox} in the Inbox")
                return 0
            if session.error:
                say(f"error: {session.error}")
                return 1
            time.sleep(0.2)
    say("the rescan did not finish in time")
    return 1


def ask(label: str, current: str, secret: bool = False, validate=None) -> str:
    """Ask for one value, re-asking until it validates.

    ``current`` is offered as the default (press Enter to keep it), and a
    secret is typed without echo.
    """
    hint = f" [{current[:4]}...]" if current else ""
    while True:
        prompt = f"{label}{hint}: "
        raw = (getpass.getpass(prompt) if secret else input(prompt)).strip()
        if not raw and current:
            raw = current
        try:
            return validate(raw) if validate else raw
        except TboxError as exc:
            say(f"  {exc}")


def cmd_setup(args: argparse.Namespace) -> int:
    """Ask for the Telegram app credentials and save them.

    The api_id goes to .env; the api_hash goes to the OS secure store
    (spec S3), so it never lands in a plain file.
    """
    say("tbox needs your own Telegram API credentials (they are free):")
    say("  1. open https://my.telegram.org/apps")
    say("  2. create (or open) an application")
    say("  3. copy its api_id and api_hash below")
    say()
    env_file = paths.env_path()
    existing = config.load_env(env_file)
    api_id = ask(
        "api_id", existing.get("TG_API_ID", ""), validate=config.validate_api_id
    )
    api_hash = ask(
        "api_hash",
        config.api_hash() if _hash_known() else "",
        secret=True,
        validate=config.validate_api_hash,
    )
    saved = config.save_env({"TG_API_ID": api_id, "TG_API_HASH": api_hash}, path=env_file)
    say()
    say(f"saved {saved}")
    say("the api_hash is kept in your operating system's secure store")
    return 0


def _hash_known() -> bool:
    """True when an api_hash is already available (env, store or .env)."""
    try:
        config.api_hash()
        return True
    except TboxError:
        return False


def build_parser() -> argparse.ArgumentParser:
    from . import __version__

    parser = argparse.ArgumentParser(
        prog="tbox",
        description="Personal file archive over Telegram Saved Messages.",
    )
    parser.add_argument("--version", action="version", version=f"tbox {__version__}")
    commands = parser.add_subparsers(dest="command", required=True)

    login = commands.add_parser(
        "login", help="link this computer to your Telegram account"
    )
    login.add_argument(
        "-v", "--verbose", action="store_true", help="also print raw engine events"
    )
    login.set_defaults(func=cmd_login)

    setup = commands.add_parser(
        "setup", help="save the Telegram API credentials tbox needs"
    )
    setup.set_defaults(func=cmd_setup)

    logout = commands.add_parser(
        "logout", help="log out on Telegram (local data is kept)"
    )
    logout.set_defaults(func=cmd_logout)

    ls = commands.add_parser("ls", help="list what is in a folder")
    ls.add_argument(
        "folder",
        nargs="?",
        default="root",
        help="root (the default), inbox, trash, lost or a folder id",
    )
    ls.set_defaults(func=cmd_ls)

    rescan = commands.add_parser(
        "rescan", help="read Saved Messages again, from scratch"
    )
    rescan.set_defaults(func=cmd_rescan)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except KeyboardInterrupt:
        say("Cancelled.")
        return 130
    except TboxError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""The tbox command line — turning engine events into something readable.

All Telegram work happens in the C engine; this file only decides what a
person sees and what they are asked. Effects are deliberately easy to follow:
one event in, at most one line out.
"""

from __future__ import annotations

import argparse
import getpass
import json
import sys
import threading

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


def say(message: str = "") -> None:
    print(message, flush=True)


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
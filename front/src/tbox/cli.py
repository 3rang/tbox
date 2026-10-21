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

from . import config, paths, qr
from .core import Core
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


def login_screen(core: Core, verbose: bool = False) -> int:
    """Render a login session driven by an already-started engine.

    Kept as a function of the event stream on purpose: tests drive it with a
    scripted engine instead of a real account.
    """
    say("Waiting for Telegram...")
    linked = False
    for event in core.events():
        if verbose:
            say("  " + json.dumps(event, ensure_ascii=False))
        name = event.get("event")

        if name == "auth_state":
            state = str(event.get("state", ""))
            if state not in _QUIET_STATES:
                say(f"  state: {state}")

        elif name == "closed":
            # the engine's own confirmation, and the last event of a session
            say("Session closed.")

        elif name == "qr_link":
            say()
            say(qr.render(str(event.get("link", ""))))
            say()
            say("Scan this with your phone: Settings > Devices > Link Desktop Device")
            say("(the code refreshes by itself, so just scan the newest one)")

        elif name == "password_needed":
            hint = str(event.get("hint", ""))
            if hint:
                say(f"  hint: {hint}")
            password = getpass.getpass("Your Telegram two-step password: ")
            core.send(password)

        elif name == "ready":
            linked = True
            who = str(event.get("user", "")).strip()
            say(f"Linked as {who}." if who else "Linked.")

        elif name == "error":
            code = event.get("code")
            message = str(event.get("message", "unknown error"))
            say(f"error: {message}" + (f" (code {code})" if code else ""))
            return 1

        elif name == "cancelled":
            say("Cancelled.")
            return 1

    return 0 if linked else 1


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

    session = paths.session_dir()
    say(f"session: {session}")
    with Core.start_login(str(session), api_id, api_hash) as core:
        return login_screen(core, verbose=args.verbose)


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
    """Ask for the Telegram app credentials and save them to .env."""
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
        existing.get("TG_API_HASH", ""),
        secret=True,
        validate=config.validate_api_hash,
    )
    saved = config.save_env({"TG_API_ID": api_id, "TG_API_HASH": api_hash}, path=env_file)
    say()
    say(f"saved {saved}")
    say("(that file stays on this computer; it is never committed)")
    return 0


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
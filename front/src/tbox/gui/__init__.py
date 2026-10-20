# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""The desktop app: the same engine, with a window instead of a terminal.

Nothing in here talks to Telegram directly — it drives the compiled engine
through :mod:`tbox.core` and only decides what a person sees.
"""
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Every tbox failure, under one base class.

So a caller can write ``except TboxError`` and mean "anything tbox could
not do", while still catching the specific kinds when it matters:
``ConfigError`` (bad or missing settings) and ``CoreError`` (the C engine
could not be started or spoke nonsense).
"""


class TboxError(Exception):
    """Base class for every error this package raises."""


class ConfigError(TboxError):
    """A setting is missing or malformed (usually in .env)."""


class CoreError(TboxError):
    """The compiled C engine is missing, or the conversation with it broke."""
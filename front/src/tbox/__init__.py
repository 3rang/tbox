# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""tbox front end — settings, session UX and (later) the local index.

Layering rule: nothing here talks to tdjson. Every Telegram action goes
through the compiled C engine as JSON lines, driven by :mod:`tbox.core`.
"""

__version__ = "0.1.0"
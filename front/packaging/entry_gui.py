# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""What PyInstaller freezes: the desktop app, and nothing else.

Kept out of the package on purpose — this file exists only for the build,
so the installed wheel stays a normal importable package.
"""

from tbox.gui.app import main

if __name__ == "__main__":
    raise SystemExit(main())

# -*- mode: python ; coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""
One window, one file: the desktop app with the C engine inside.

The engine and its DLLs (collected from front/src/tbox/bin by build.ps1) are
placed at the root of the bundle, which is exactly where
tbox.config._frozen_dirs() looks for them at runtime.
"""

import sys
from pathlib import Path

here = Path(SPECPATH)                     # front/packaging
front = here.parent
bundle = front / "src" / "tbox" / "bin"
icon = here / "tbox.ico"

# the compiled engine + tdjson and friends, side by side in the bundle root
if not bundle.is_dir() or not any(bundle.iterdir()):
    raise SystemExit(
        "front/src/tbox/bin is empty - build.ps1 fills it in step 2, "
        "so run build.ps1 before freezing the app"
    )
binaries = [(str(p), ".") for p in sorted(bundle.iterdir()) if p.is_file()]

# nothing in the app imports these; skipping them keeps the file much smaller
excludes = [
    "PySide6.QtWebEngineCore", "PySide6.QtWebEngineWidgets",
    "PySide6.Qt3DAnimation", "PySide6.Qt3DCore", "PySide6.Qt3DExtras",
    "PySide6.Qt3DInput", "PySide6.Qt3DLogic", "PySide6.Qt3DRender",
    "PySide6.QtBluetooth", "PySide6.QtCharts", "PySide6.QtDataVisualization",
    "PySide6.QtDesigner", "PySide6.QtHelp", "PySide6.QtLocation",
    "PySide6.QtMultimedia", "PySide6.QtMultimediaWidgets", "PySide6.QtNfc",
    "PySide6.QtPositioning", "PySide6.QtQuick", "PySide6.QtQuick3D",
    "PySide6.QtQuickWidgets", "PySide6.QtRemoteObjects", "PySide6.QtScxml",
    "PySide6.QtSensors", "PySide6.QtSerialBus", "PySide6.QtSerialPort",
    "PySide6.QtSql", "PySide6.QtTest", "PySide6.QtTextToSpeech",
    "PySide6.QtUiTools", "PySide6.QtWebChannel", "PySide6.QtWebSockets",
    "PySide6.QtXml",
    "tkinter", "unittest", "pydoc",
]

a = Analysis(
    [str(here / "entry_gui.py")],
    pathex=[str(front / "src")],
    binaries=binaries,
    datas=[],
    hiddenimports=[],
    hookspath=[],
    excludes=excludes,
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="tbox",
    debug=False,
    strip=False,
    upx=False,
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    icon=str(icon) if icon.is_file() else None,
)

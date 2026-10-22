# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""The Files screen: a folder tree on the left, the items on the right.

Read-only for now. It shows what the catalog holds (spec 7.6) and asks the
engine for it; it never touches the index itself, and it never waits for the
engine - the window hands the question to the worker thread and the answer
comes back as a signal.

The tree starts with the four places every catalog has (Root, Inbox, Trash,
Lost & Found) and grows: opening a folder asks the engine for what is in it,
and the folders among the answer become its children.
"""

from __future__ import annotations

import datetime

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QAbstractItemView,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

#: The four places the catalog has, in the order the tree shows them.
VIRTUAL_FOLDERS = (
    ("Root", "root"),
    ("Inbox", "inbox"),
    ("Trash", "trash"),
    ("Lost & Found", "lost"),
)


def format_size(value: object) -> str:
    """A size in bytes, in words, or a dash when it is not known."""
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value < 0:
        return "-"
    size = float(value)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if size < 1024 or unit == "TB":
            return f"{int(size)} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return "-"


def format_time(value: object) -> str:
    """A unix timestamp as a local date and time, or a dash."""
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
        return "-"
    return datetime.datetime.fromtimestamp(float(value)).strftime("%Y-%m-%d %H:%M")


class FilesPage(QWidget):
    """The Files screen: the tree of folders and the list of items."""

    #: The user picked a folder (or opened one): the window asks the engine.
    folder_chosen = Signal(str)
    #: The user wants the whole history read again.
    rescan_requested = Signal()
    #: The user wants to end the Telegram session.
    signout_requested = Signal()
    #: The user wants this computer's data gone.
    remove_data_requested = Signal()

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._nodes: dict[str, QTreeWidgetItem] = {}
        self._current = ""
        self._build()

    # ---- building -----------------------------------------------------

    def _build(self) -> None:
        layout = QVBoxLayout(self)

        header = QHBoxLayout()
        self.title = QLabel("Files")
        self.account = QLabel("")
        self.rescan_button = QPushButton("Rescan")
        self.rescan_button.setToolTip("Read Saved Messages again, from scratch")
        self.rescan_button.clicked.connect(self.rescan_requested.emit)
        self.signout_button = QPushButton("Sign out")
        self.signout_button.clicked.connect(self.signout_requested.emit)
        self.remove_button = QPushButton("Remove local data")
        self.remove_button.clicked.connect(self.remove_data_requested.emit)
        header.addWidget(self.title)
        header.addStretch(1)
        header.addWidget(self.account)
        header.addWidget(self.rescan_button)
        header.addWidget(self.signout_button)
        header.addWidget(self.remove_button)
        layout.addLayout(header)

        self.tree = QTreeWidget()
        self.tree.setHeaderLabel("Folders")
        self.tree.setMinimumWidth(170)
        for label, folder in VIRTUAL_FOLDERS:
            node = QTreeWidgetItem(self.tree, [label])
            node.setData(0, Qt.ItemDataRole.UserRole, folder)
            self._nodes[folder] = node
        self.tree.currentItemChanged.connect(self._on_current_changed)
        self.tree.itemExpanded.connect(self._on_expanded)

        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(["Name", "Size", "Date"])
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.itemDoubleClicked.connect(self._on_activated)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(self.tree)
        splitter.addWidget(self.table)
        splitter.setStretchFactor(1, 1)
        layout.addWidget(splitter, 1)

    # ---- what the window tells us --------------------------------------

    def set_account(self, name: str) -> None:
        """Shows who is signed in."""
        self.account.setText(f"Signed in as {name}" if name else "")

    def select(self, folder: str) -> None:
        """Puts the tree on a folder (which asks for its items)."""
        node = self._nodes.get(folder)
        if node is None:
            return
        if self.tree.currentItem() is node:
            # already there: the tree says nothing, so ask again
            self._current = folder
            self.folder_chosen.emit(folder)
            return
        self.tree.setCurrentItem(node)

    def current_folder(self) -> str:
        """The folder the list is showing."""
        return self._current

    def show_folder(self, folder: str, items: list[dict]) -> None:
        """Fills the tree (and, when it is the chosen folder, the list)."""
        if folder == self._current:
            self._fill_list(items)
        self._fill_tree(folder, items)

    def show_message(self, text: str) -> None:
        """Shows a line of text where the items would be."""
        self.table.setRowCount(1)
        self.table.setItem(0, 0, QTableWidgetItem(text))
        self.table.setItem(0, 1, QTableWidgetItem(""))
        self.table.setItem(0, 2, QTableWidgetItem(""))

    # ---- the list ------------------------------------------------------

    def _fill_list(self, items: list[dict]) -> None:
        self.table.setRowCount(len(items))
        for row, item in enumerate(items):
            name = str(item.get("name") or item.get("orig_name") or "?")
            kind = str(item.get("kind", ""))
            size = "-" if kind == "d" else format_size(item.get("size"))
            when = format_time(item.get("mtime") or item.get("date"))
            cell = QTableWidgetItem(name)
            cell.setData(Qt.ItemDataRole.UserRole, str(item.get("object_id") or ""))
            if kind == "d":
                cell.setToolTip("folder")
            self.table.setItem(row, 0, cell)
            self.table.setItem(row, 1, QTableWidgetItem(size))
            self.table.setItem(row, 2, QTableWidgetItem(when))
        self.table.resizeColumnsToContents()

    # ---- the tree ------------------------------------------------------

    def _fill_tree(self, folder: str, items: list[dict]) -> None:
        """Adds (or refreshes) the folders among a folder's items."""
        node = self._nodes.get(folder)
        if node is None:
            return
        wanted = {
            str(item.get("object_id") or ""): str(item.get("name") or "")
            for item in items
            if item.get("kind") == "d" and item.get("object_id")
        }
        for index in range(node.childCount()):
            child = node.child(index)
            child_id = str(child.data(0, Qt.ItemDataRole.UserRole) or "")
            if child_id in wanted:
                if child.text(0) != wanted[child_id]:
                    child.setText(0, wanted[child_id])
            else:
                node.removeChild(child)
                self._nodes.pop(child_id, None)
        for child_id, name in wanted.items():
            if child_id in self._nodes:
                continue
            child = QTreeWidgetItem(node, [name])
            child.setData(0, Qt.ItemDataRole.UserRole, child_id)
            self._nodes[child_id] = child

    # ---- the user's moves ----------------------------------------------

    def _on_current_changed(self, current, previous) -> None:
        if current is None:
            return
        folder = str(current.data(0, Qt.ItemDataRole.UserRole) or "")
        if not folder:
            return
        self._current = folder
        self.folder_chosen.emit(folder)

    def _on_expanded(self, node) -> None:
        folder = str(node.data(0, Qt.ItemDataRole.UserRole) or "")
        if not folder:
            return
        self.folder_chosen.emit(folder)

    def _on_activated(self, cell: QTableWidgetItem) -> None:
        """A double click on a folder goes into it."""
        first = self.table.item(cell.row(), 0)
        if first is None:
            return
        node = self._nodes.get(str(first.data(Qt.ItemDataRole.UserRole) or ""))
        if node is None:
            return
        self.tree.setCurrentItem(node)
        node.setExpanded(True)

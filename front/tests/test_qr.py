# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Tests for the QR rendering - pure text in, pure text out."""

from __future__ import annotations

import unittest

from tbox import qr


class BlocksTest(unittest.TestCase):
    def test_two_module_rows_become_one_line(self):
        # top row dark-then-light, bottom row light-then-dark
        self.assertEqual(qr.blocks([[True, False], [False, True]]), "▀▄")

    def test_full_and_empty_columns(self):
        self.assertEqual(qr.blocks([[True, False], [True, False]]), "█ ")
        self.assertEqual(qr.blocks([[False, False], [False, False]]), "  ")

    def test_an_odd_row_count_pads_the_last_line(self):
        # no bottom half, so the last module only has a dark top -> upper block
        self.assertEqual(qr.blocks([[True, True]]), "▀▀")

    def test_empty_input_is_empty_output(self):
        self.assertEqual(qr.blocks([]), "")


class AsciiTest(unittest.TestCase):
    def test_fallback_is_two_characters_per_module(self):
        self.assertEqual(qr.ascii_art([[True, False], [False, True]]), "##  \n  ##")


class RenderTest(unittest.TestCase):
    def test_matrix_is_square_with_a_quiet_zone(self):
        rows = qr.matrix("tg://login?token=hello")
        self.assertEqual(len(rows), len(rows[0]))
        self.assertTrue(rows[0][0] is False)  # border is light
        self.assertTrue(any(any(row) for row in rows))  # and something is dark

    def test_rendered_lines_are_all_the_same_width(self):
        lines = qr.render("tg://login?token=hello").splitlines()
        self.assertGreater(len(lines), 10)
        self.assertEqual(len({len(line) for line in lines}), 1)  # square-ish block

    def test_a_longer_link_needs_a_bigger_code(self):
        small = len(qr.matrix("tg://login?token=a"))
        large = len(qr.matrix("tg://login?token=" + "a" * 200))
        self.assertGreater(large, small)


if __name__ == "__main__":
    unittest.main()
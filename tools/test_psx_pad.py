#!/usr/bin/env python3
"""Tests for the one pad bit table and the .pad replay format (psx_pad.py).

This module exists because there were TWO copies of the bit table and they were not the same shape.
The consumer's copy was missing the shoulder and stick bits, and the way that was found is recorded
in its own comment: a round trip over the whole replay library met mask 0xFEFF (L2 held) and could
not name it. An incomplete table does not fail — it drops input silently and the rebuilt route
desyncs. So the refusals below matter more than the happy paths.
"""

from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

import psx_pad


def write(path: Path, masks) -> Path:
    path.write_bytes(b"".join(struct.pack("<H", value) for value in masks))
    return path


class PadTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_every_button_round_trips_through_the_active_low_word(self) -> None:
        """Each name alone, and all sixteen at once. A table missing a bit passes a test that only
        checks the buttons it knows about, which is why this iterates the table itself."""
        for name in psx_pad.PSX_BUTTON_BITS:
            held = frozenset({name})
            self.assertEqual(psx_pad.buttons_of(psx_pad.mask_of(held)), held, name)
        every = frozenset(psx_pad.PSX_BUTTON_BITS)
        self.assertEqual(psx_pad.buttons_of(psx_pad.mask_of(every)), every)
        self.assertEqual(psx_pad.mask_of(every), 0)
        self.assertEqual(psx_pad.buttons_of(psx_pad.NEUTRAL), frozenset())

    def test_the_table_covers_the_whole_word(self) -> None:
        """All sixteen bits are named. This is the property whose absence caused the original bug."""
        self.assertEqual(len(psx_pad.PSX_BUTTON_BITS), 16)
        self.assertEqual(sum(psx_pad.PSX_BUTTON_BITS.values()), 0xFFFF)

    def test_l2_held_is_nameable(self) -> None:
        """0xFEFF is the exact mask the incomplete copy could not name."""
        self.assertEqual(psx_pad.buttons_of(0xFEFF), frozenset({"l2"}))

    def test_a_schedule_is_one_entry_per_frame_in_order(self) -> None:
        path = write(self.dir / "r.pad", [psx_pad.NEUTRAL,
                                          psx_pad.mask_of(frozenset({"start"})),
                                          psx_pad.mask_of(frozenset({"left", "cross"}))])
        self.assertEqual(psx_pad.schedule(path),
                         [frozenset(), frozenset({"start"}), frozenset({"left", "cross"})])

    def test_a_suffix_starts_where_it_was_asked_to(self) -> None:
        path = write(self.dir / "r.pad", [psx_pad.NEUTRAL] * 3 + [psx_pad.mask_of(frozenset({"up"}))])
        self.assertEqual(psx_pad.schedule(path, 3), [frozenset({"up"})])
        self.assertEqual(len(psx_pad.schedule(path, 1)), 3)

    # --- refusals, each shown to fire -----------------------------------------------------

    def test_an_empty_replay_is_refused_not_read_as_an_empty_route(self) -> None:
        path = self.dir / "empty.pad"
        path.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "empty"):
            psx_pad.masks(path)

    def test_an_odd_length_file_is_refused(self) -> None:
        path = self.dir / "odd.pad"
        path.write_bytes(b"\xff\xff\xff")
        with self.assertRaisesRegex(ValueError, "whole number"):
            psx_pad.masks(path)

    def test_a_start_past_the_end_is_refused(self) -> None:
        path = write(self.dir / "r.pad", [psx_pad.NEUTRAL] * 2)
        for start in (2, 5, -1):
            with self.assertRaisesRegex(ValueError, "cannot start at frame"):
                psx_pad.schedule(path, start)

    def test_an_unnamed_cleared_bit_is_refused_rather_than_dropped(self) -> None:
        """Silently ignoring a bit it cannot name is the original defect, so it must raise. This
        needs the table to be temporarily incomplete, because the real one names every bit."""
        original = dict(psx_pad.PSX_BUTTON_BITS)
        try:
            del psx_pad.PSX_BUTTON_BITS["l2"]
            psx_pad.BITS_TO_NAME.pop(0x0100)
            with self.assertRaisesRegex(ValueError, "unnamed bit"):
                psx_pad.buttons_of(0xFEFF)
        finally:
            psx_pad.PSX_BUTTON_BITS.clear()
            psx_pad.PSX_BUTTON_BITS.update(original)
            psx_pad.BITS_TO_NAME.clear()
            psx_pad.BITS_TO_NAME.update({bit: name for name, bit in original.items()})
        # ... and the restored table names it again, so the test cannot leave the module broken.
        self.assertEqual(psx_pad.buttons_of(0xFEFF), frozenset({"l2"}))


if __name__ == "__main__":
    unittest.main()

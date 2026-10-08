#!/usr/bin/env python3
"""Tests for the one pad bit table and the .pad replay format (psx_pad.py).

This module exists because there were TWO copies of the bit table and they were not the same shape.
The consumer's copy was missing the shoulder and stick bits, and the way that was found is recorded
in its own comment: a round trip over the whole replay library met mask 0xFEFF (L2 held) and could
not name it. An incomplete table does not fail — it drops input silently and the rebuilt route
desyncs. So the refusals below matter more than the happy paths.
"""

from __future__ import annotations

import contextlib
import io
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


class PhaseKeyedFormatTests(unittest.TestCase):
    """The v1 container this module now owns: decode, its refusals, and the migration.

    The refusals are the tests that matter. A tool that flattened a phase-keyed recording to
    absolute frames, or that migrated one while quietly implying it was keyed, produces a route
    that runs and means something else — the defect issue 0116 is about.
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.segments = [(0x0D00000003, [(psx_pad.NEUTRAL, 8), (psx_pad.mask_of(frozenset({"start"})), 2)]),
                         (0x0D0001000F, [(psx_pad.NEUTRAL, 3), (psx_pad.mask_of(frozenset({"cross"})), 1)])]
        self.card = bytes(range(32))
        self.path = self.dir / "keyed.pad"
        self.path.write_bytes(psx_pad.encode(2, self.card, self.segments))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    @staticmethod
    def flatten(recording: psx_pad.Recording) -> list[int]:
        return [mask for _, runs in recording.segments for mask, frames in runs
                for mask in [mask] * frames]

    def test_a_phase_keyed_recording_decodes_with_its_denominators(self) -> None:
        recording = psx_pad.read(self.path)
        self.assertTrue(recording.keyed)
        self.assertEqual(recording.frames, 14)
        self.assertEqual(len(recording.segments), 2)
        self.assertEqual(recording.card, f"sha256 {self.card.hex()}")
        # The summary is what a reader acts on, so it names the frames that hold a button and not
        # just the total: 14 frames of which 3 press something is a different route from 14 of
        # which none do, and both would otherwise print "14 frames".
        self.assertIn("14 frame(s), 3 holding a button", recording.summary())
        self.assertIn("phase-keyed", recording.summary())

    def test_the_encoded_bytes_are_the_runtime_layout(self) -> None:
        """Byte-exact against the documented layout, so this module and pad_recording.h cannot
        disagree about the format while both still 'work' on their own files."""
        data = self.path.read_bytes()
        self.assertEqual(data[:8], b"PSXPADPH")
        self.assertEqual(struct.unpack_from("<I", data, 8)[0], 1)
        self.assertEqual(struct.unpack_from("<I", data, 12)[0], 2)
        self.assertEqual(data[16:48], self.card)
        self.assertEqual(data[48], ord("P"))
        self.assertEqual(struct.unpack_from("<Q", data, 49)[0], 0x0D00000003)
        self.assertEqual(data[57], ord("R"))
        self.assertEqual(struct.unpack_from("<HI", data, 58), (psx_pad.NEUTRAL, 8))
        self.assertEqual(len(data), psx_pad.HEADER_BYTES + 2 * psx_pad.PHASE_RECORD_BYTES
                         + 4 * psx_pad.RUN_RECORD_BYTES)

    def test_an_absolute_reader_refuses_a_phase_keyed_file_by_name(self) -> None:
        """The negative that matters most: masks()/schedule() must not flatten a keyed file into
        frame numbers that mean nothing without the phase source that produced them."""
        for reader in (psx_pad.masks, psx_pad.schedule):
            with self.subTest(reader=reader.__name__):
                with self.assertRaisesRegex(ValueError, "PHASE-KEYED"):
                    reader(self.path)
        with self.assertRaisesRegex(ValueError, "PSXPADPH"):
            psx_pad.decode(b"\xff\xff\xf7\xff")

    def test_a_raw_recording_is_refused_by_name_and_by_the_migration(self) -> None:
        raw = write(self.dir / "raw.pad", [psx_pad.NEUTRAL, psx_pad.NEUTRAL,
                                           psx_pad.mask_of(frozenset({"start"}))])
        with self.assertRaisesRegex(ValueError, "psx_pad.py migrate"):
            psx_pad.read(raw)
        with self.assertRaisesRegex(ValueError, "PSXPADPH"):
            psx_pad.decode(raw.read_bytes(), "raw")

    def test_every_malformed_v1_body_is_refused_not_truncated(self) -> None:
        good = self.path.read_bytes()
        header = good[:psx_pad.HEADER_BYTES]
        phase = b"P" + struct.pack("<Q", 3)
        run = b"R" + struct.pack("<HI", psx_pad.NEUTRAL, 1)
        cases = {
            "truncated run record": good[:-2],
            "truncated phase record": good[:psx_pad.HEADER_BYTES + 4],
            "version 2 is not supported": good[:8] + struct.pack("<I", 2) + good[12:],
            "unknown card identity kind": good[:12] + struct.pack("<I", 9) + good[16:],
            "truncated header": good[:20],
            "precedes any phase": header + run,
            "zero-length run": header + phase + b"R" + struct.pack("<HI", 0, 0),
            "unknown record tag": header + b"Z",
            # Three different bodies that all hold no frames, one refusal, because the answer is the
            # same and a reader must not be asked to tell them apart: a bare header, a trailing empty
            # phase, and a phase immediately followed by another phase.
            "holds no frames": header,
            "holds no frames (trailing phase)": header + phase,
            "holds no frames (empty phase)": header + phase + phase + run,
        }
        for why, body in cases.items():
            with self.subTest(why=why):
                with self.assertRaisesRegex(ValueError, why.split(" (")[0]):
                    psx_pad.decode(body, "case")

    def test_a_zero_length_run_cannot_be_written_either(self) -> None:
        """The encoder refuses what the decoder refuses, so a hand-built file cannot exist here that
        the runtime would reject on load."""
        with self.assertRaisesRegex(ValueError, "zero-length run"):
            psx_pad.encode(0, bytes(32), [(3, [(psx_pad.NEUTRAL, 0)])])
        with self.assertRaisesRegex(ValueError, "32 bytes of sha256"):
            psx_pad.encode(2, bytes(4), [(3, [(psx_pad.NEUTRAL, 1)])])

    def test_migration_preserves_the_from_boot_meaning_and_says_it_is_unkeyed(self) -> None:
        source = write(self.dir / "raw.pad", [psx_pad.NEUTRAL] * 4
                       + [psx_pad.mask_of(frozenset({"left", "cross"}))] * 2
                       + [psx_pad.NEUTRAL] * 3)
        out = self.dir / "migrated.pad"
        recording = psx_pad.migrate(source, out)
        # UNKEYED, and stated as such: a migrated file is NOT a keyed route. A caller that read it
        # as one would expect timing changes to be absorbed, which this file cannot do.
        self.assertFalse(recording.keyed)
        self.assertIn("UNKEYED", recording.summary())
        decoded = psx_pad.read(out)
        self.assertFalse(decoded.keyed)
        self.assertEqual(decoded.frames, 9)
        self.assertEqual(decoded.card, "unknown")
        # The run lengths are collapsed correctly AND the frame sequence survives the conversion,
        # which is the whole claim: 4 neutral, 2 held, 3 neutral.
        self.assertEqual(decoded.segments[0][1],
                         [(psx_pad.NEUTRAL, 4),
                          (psx_pad.mask_of(frozenset({"left", "cross"})), 2),
                          (psx_pad.NEUTRAL, 3)])
        self.assertEqual(self.flatten(decoded), psx_pad.masks(source))

    def test_the_migrate_cli_reports_that_the_result_is_not_keyed(self) -> None:
        source = write(self.dir / "raw.pad", [psx_pad.NEUTRAL, psx_pad.mask_of(frozenset({"up"}))])
        out = self.dir / "migrated.pad"
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(psx_pad.main(["migrate", str(source), str(out)]), 0)
        self.assertIn("UNKEYED", printed.getvalue())
        self.assertIn("replays ABSOLUTELY from boot", printed.getvalue())
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(psx_pad.main(["info", str(out)]), 0)
        self.assertIn("up", printed.getvalue())

    def test_the_info_cli_names_each_press_and_its_hold_length(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(psx_pad.main(["info", str(self.path)]), 0)
        text = printed.getvalue()
        self.assertIn("2 segment(s), 14 frame(s), 3 holding a button", text)
        self.assertIn("segment 1 phase 0xd00000003: 2 frame(s) start", text)
        self.assertIn("segment 2 phase 0xd0001000f: 1 frame(s) cross", text)


if __name__ == "__main__":
    unittest.main()

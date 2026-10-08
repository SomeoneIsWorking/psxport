#!/usr/bin/env python3
"""Asset-free falsifiers for the actual full-console host callback/protocol owners."""

from __future__ import annotations

import ctypes as ct
import gc
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

import console_abi as abi
from console import command
from console_build import SCRATCH
from console_capture import Framebuffer
from console_firmware import (IDENTITIES, FirmwareSelection, prepare_system_directory, select_firmware,
                              verify_loaded_firmware)
from console_session import ConsoleSession


class ConsoleHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = SCRATCH / "selftest"
        if cls.directory.exists():
            raise ValueError(f"selftest output already exists; inspect before cleanup: {cls.directory}")
        cls.directory.mkdir(parents=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.directory)

    def session(self):
        return ConsoleSession(None, self.directory / "system", self.directory / "saves")

    def test_native_callback_lifetime_and_pixel_stride(self):
        session = self.session()
        # Invoke the C ABI trampoline through an independently held native function address after GC.
        # The owning session must retain callbacks; Python must not collect the callable C will use.
        pointer = ct.cast(session.callbacks["video_refresh"], ct.c_void_p).value
        callback = abi.VideoRefresh(pointer)
        gc.collect()
        pixels = ct.create_string_buffer(struct.pack("=6I", 0xFF0000, 0x00FF00, 0xBADBAD,
                                                     0x0000FF, 0xFFFFFF, 0xBADBAD))
        session.pixel_format = 1
        callback(ct.addressof(pixels), 2, 2, 12)
        session._check_callbacks()
        self.assertEqual(session.framebuffer.rgb_rows(),
                         bytes([0, 255, 0, 0, 0, 255, 0, 0, 0, 0, 255, 255, 255, 255]))
        # C is allowed to reuse its framebuffer after the callback returns: the host owns a copy.
        ct.memset(pixels, 0, len(pixels))
        self.assertNotEqual(session.framebuffer.rgb_rows(), bytes(14))
        callback(None, 2, 2, 12)
        self.assertEqual(session.duplicate_frames, 1)
        output = self.directory / "pixel-stride.png"
        session.framebuffer.save_png(output)
        data = output.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        cursor, decoded = 8, b""
        while cursor < len(data):
            size = struct.unpack_from(">I", data, cursor)[0]
            kind = data[cursor + 4:cursor + 8]
            payload = data[cursor + 8:cursor + 8 + size]
            checksum = struct.unpack_from(">I", data, cursor + 8 + size)[0]
            self.assertEqual(checksum, zlib.crc32(kind + payload))
            if kind == b"IDAT":
                decoded += zlib.decompress(payload)
            cursor += size + 12
        self.assertEqual(decoded, session.framebuffer.rgb_rows())

    def test_16_bit_formats_and_invalid_buffers(self):
        for pixel_format, words in ((0, (0x7C00, 0x03E0, 0x001F)),
                                    (2, (0xF800, 0x07E0, 0x001F))):
            pixels = ct.create_string_buffer(struct.pack("=3H", *words))
            frame = Framebuffer.capture(ct.addressof(pixels), 3, 1, 6, pixel_format)
            self.assertEqual(frame.rgb_rows(), bytes([0, 255, 0, 0, 0, 255, 0, 0, 0, 255]))
        for arguments in ((1, 2, 1, 3, 1), (1, 0, 1, 4, 1), (1, 1, 1, 4, 99),
                          (1, 2049, 1, 10000, 1), (1, 1, 1025, 4, 1),
                          (ct.c_void_p(-1).value, 1, 1, 4, 1)):
            with self.assertRaises(ValueError):
                Framebuffer.capture(*arguments)

    def test_callback_failure_poisoning(self):
        session = self.session()
        session.pixel_format = 1
        session.callbacks["video_refresh"](None, 1, 1, 4)
        with self.assertRaisesRegex(RuntimeError, "before publishing"):
            session._check_callbacks()
        session.callbacks["audio_sample"](0, 0)
        self.assertEqual(session.audio_frames, 0, "poisoned session must not continue callback work")
        session = self.session()
        bad_format = ct.c_int(9)
        self.assertFalse(session.callbacks["environment"](10, ct.byref(bad_format)))
        with self.assertRaisesRegex(RuntimeError, "pixel format"):
            session._check_callbacks()

    def test_environment_abi_options_and_owned_geometry(self):
        session = self.session()
        environment = session.callbacks["environment"]
        version = ct.c_uint(999)
        self.assertTrue(environment(52, ct.byref(version)))
        self.assertEqual(version.value, 0)
        system = ct.c_char_p()
        self.assertTrue(environment(9, ct.byref(system)))
        self.assertEqual(system.value, session.system_directory)
        values = (abi.Variable * 2)(abi.Variable(b"beetle_psx_cpu_dynarec",
                                               b"CPU mode; execute|disabled"), abi.Variable())
        self.assertTrue(environment(16, values))
        variable = abi.Variable(b"beetle_psx_cpu_dynarec", None)
        self.assertTrue(environment(15, ct.byref(variable)))
        gc.collect()
        self.assertEqual(variable.value, b"disabled")
        incoming = abi.SystemAvInfo()
        incoming.timing.fps = 59.94
        self.assertTrue(environment(32, ct.byref(incoming)))
        incoming.timing.fps = 1.0
        self.assertEqual(session.av.timing.fps, 59.94, "AV info must not alias the C caller's storage")
        self.assertFalse(environment(0xFFFF, None))
        self.assertEqual(session.unsupported_environment[0xFFFF], 1)
        session._check_callbacks()

    def test_input_audio_and_strict_control(self):
        session = self.session()
        command(session, {"command": "buttons", "buttons": ["left", "cross"]}, self.directory)
        state = session.callbacks["input_state"]
        self.assertEqual(state(0, 1, 0, 6), 1)
        self.assertEqual(state(0, 1, 0, 0), 1)
        self.assertEqual(state(1, 1, 0, 0), 0)
        self.assertEqual(state(0, 1, 0, 256), 65)
        audio = (ct.c_int16 * 8)(*range(8))
        self.assertEqual(session.callbacks["audio_sample_batch"](audio, 4), 4)
        session.callbacks["audio_sample"](-1, 1)
        self.assertEqual(session.audio_frames, 5)
        for message in ({"command": []}, {"command": "capture"}, {"command": "buttons", "buttons": ["typo"]},
                        {"command": "step", "frames": 0}, {"command": "status", "extra": 1},
                        {"command": "read", "address": 0, "bytes": 257}):
            with self.assertRaises(ValueError):
                command(session, message, self.directory)
        self.assertEqual(session.button_mask, 65, "rejected command changed prior valid input")

    def test_core_firmware_log_hexadecimal_case(self):
        digest = "10155d8d6e6e832d6ea66db9bc098321fb5e8ebf"
        selected = FirmwareSelection("na", Path("synthetic-path"), digest)
        # Exact core warning shape: its SHA1 formatter uses uppercase hexadecimal.
        log = ("Firmware found but has invalid SHA1: system/scph5501.bin\n"
               "Expected SHA1: 0555C6FAE8906F3F09BAF5988F00E55F88E9F30B\n"
               "Obtained SHA1: 10155D8D6E6E832D6EA66DB9BC098321FB5E8EBF\n")
        observed = verify_loaded_firmware(selected, log)
        self.assertEqual(observed["observed_sha1"], digest)
        self.assertTrue(observed["core_revision_warning"])
        self.assertEqual(verify_loaded_firmware(selected, "Firmware SHA1: " + digest.upper())
                         ["observed_sha1"], digest)
        for invalid in (log.replace(digest.upper(), "A" * 40),
                        log.replace(digest.upper(), digest.upper() + "A")):
            with self.assertRaises(ValueError):
                verify_loaded_firmware(selected, invalid)
        with self.assertRaises(ValueError):
            verify_loaded_firmware(select_firmware("na", None, True), log)

    def test_firmware_admission_and_exclusive_system_directory(self):
        with self.assertRaises(ValueError):
            select_firmware("na", None, False)
        with self.assertRaises(ValueError):
            select_firmware("na", self.directory / "missing", True)
        wrong = self.directory / "wrong-bios.bin"
        wrong.write_bytes(bytes(512 * 1024))
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            select_firmware("na", wrong, False)
        openbios = select_firmware("na", None, True)
        self.assertFalse(openbios.authentic)
        self.assertIn("no authentic-BIOS", openbios.describe()["limitation"])
        system = prepare_system_directory(self.directory, openbios)
        self.assertEqual(list(system.iterdir()), [])
        (system / "foreign.bin").write_bytes(b"foreign")
        with self.assertRaisesRegex(ValueError, "foreign firmware"):
            prepare_system_directory(self.directory, openbios)
        # Admission calls the real file-size and digest path, with a separately injected trusted
        # identity only for this redistributable synthetic BIOS fixture.
        import hashlib
        with patch.dict(IDENTITIES, {hashlib.sha1(wrong.read_bytes()).hexdigest():
                                     ("na", "Synthetic", "test")}):
            accepted = select_firmware("na", wrong, False)
        self.assertTrue(accepted.authentic)
        self.assertEqual(accepted.describe()["model"], "Synthetic")
        self.assertEqual(accepted.describe()["core_search_filename"], "scph5501.bin")
        observed = verify_loaded_firmware(accepted, "Obtained SHA1: " + accepted.sha1)
        self.assertEqual(observed["observed_sha1"], accepted.sha1)
        for log in ("", "Firmware SHA1: " + "0" * 40,
                    "Obtained SHA1: " + accepted.sha1 + "\nBIOS file short read"):
            with self.assertRaises(ValueError):
                verify_loaded_firmware(accepted, log)
        with self.assertRaises(ValueError):
            verify_loaded_firmware(openbios, "Firmware SHA1: " + accepted.sha1)


if __name__ == "__main__":
    unittest.main()


class InsertCardTests(unittest.TestCase):
    """The reference's memory card decides a title's menu route, so the harness must be able to set
    it and must REFUSE every way of setting it that would quietly compare two different cards."""

    CARD = 128 * 1024

    class FakeLibrary:
        """The two libretro entry points a card insertion uses, over a real 128 KiB buffer."""

        def __init__(self, size: int = 128 * 1024, present: bool = True):
            self.buffer = ct.create_string_buffer(size) if present else None
            self.size = size

        def retro_get_memory_size(self, kind: int) -> int:
            return self.size if kind == 0 else 0

        def retro_get_memory_data(self, kind: int):
            if kind != 0 or self.buffer is None:
                return None
            return ct.addressof(self.buffer)

    def session(self, library=None, loaded: bool = True, frames: int = 0):
        session = ConsoleSession(None, Path("system"), Path("saves"))
        session.library = library if library is not None else self.FakeLibrary()
        session.loaded = loaded
        session.frames = frames
        return session

    def image(self, magic: bytes = b"MC") -> bytes:
        return magic + bytes(self.CARD - len(magic))

    def test_the_card_reaches_the_core_buffer_and_is_read_back(self):
        library = self.FakeLibrary()
        session = self.session(library)
        result = session.insert_card(self.image())
        self.assertEqual(ct.string_at(ct.addressof(library.buffer), self.CARD), self.image())
        self.assertEqual((result["card_bytes"], result["magic"]), (self.CARD, "MC"))
        # A DIFFERENT card must produce a different digest, or the digest proves nothing.
        other = self.session(self.FakeLibrary()).insert_card(self.image(b"\x00\x00"))
        self.assertNotEqual(other["card_sha256"], result["card_sha256"])

    def test_refuses_a_card_that_is_not_a_psx_card(self):
        for size in (self.CARD - 1, self.CARD + 1, 0):
            with self.assertRaises(ValueError) as raised:
                self.session().insert_card(bytes(size))
            self.assertIn("exactly", str(raised.exception))

    def test_refuses_after_the_console_has_already_run(self):
        with self.assertRaises(ValueError) as raised:
            self.session(frames=1).insert_card(self.image())
        self.assertIn("before the console is stepped", str(raised.exception))

    def test_refuses_without_loaded_content(self):
        with self.assertRaises(ValueError):
            self.session(loaded=False).insert_card(self.image())

    def test_refuses_a_core_that_exposes_no_card(self):
        for library in (self.FakeLibrary(present=False), self.FakeLibrary(size=1024)):
            with self.assertRaises(ValueError) as raised:
                self.session(library).insert_card(self.image())
            self.assertIn("memory card through save RAM", str(raised.exception))

    def test_the_protocol_command_carries_the_image_by_path(self):
        library = self.FakeLibrary()
        session = self.session(library)
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        card = directory / "card.mcr"
        card.write_bytes(self.image())
        message = {"command": "insert_card", "card": str(card)}
        # The card travels as a path because the control protocol bounds one command line at 65,536
        # characters and a 128 KiB card is 262,144 hex characters. Measured 2026-09-19: sending the
        # hex made the reference refuse the line and exit, and the controlling process saw only a
        # broken pipe. Keep the command far inside the bound.
        self.assertLess(len(json.dumps(message)), 65536)
        result = command(session, message, Path("unused"))
        self.assertEqual(result["card_bytes"], self.CARD)
        self.assertEqual(ct.string_at(ct.addressof(library.buffer), 2), b"MC")
        with self.assertRaises(ValueError):
            command(session, {"command": "insert_card", "card": 5}, Path("unused"))
        with self.assertRaises(ValueError) as raised:
            command(session, {"command": "insert_card", "card": str(directory / "absent.mcr")},
                    Path("unused"))
        self.assertIn("does not exist", str(raised.exception))

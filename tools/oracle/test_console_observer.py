#!/usr/bin/env python3
"""Synthetic host ABI/protocol and canonical-output hash discriminators."""

import ctypes as ct
import hashlib
from pathlib import Path
import struct
from types import SimpleNamespace
import unittest

from console import command
from console_capture import CaptureHashes, Framebuffer
from console_observer import Config, Observer, Record, Status, bind
from console_session import ConsoleSession


def fake_library():
    state = Status(scanned=123)
    records = (Record * 1)()
    records[0].ordinal = 7
    records[0].ram_bytes = 2
    records[0].ram[0], records[0].ram[1] = 11, 22
    calls = []

    def configure(pointer):
        copied = Config.from_buffer_copy(ct.string_at(pointer, ct.sizeof(Config)))
        calls.append(copied)
        return 1

    def status(pointer):
        ct.memmove(pointer, ct.byref(state), ct.sizeof(state))

    def drain(pointer, _capacity):
        ct.memmove(pointer, records, ct.sizeof(records))
        return 1

    functions = {
        "abi": ct.CFUNCTYPE(ct.c_uint32)(lambda: 1),
        "size": ct.CFUNCTYPE(ct.c_uint32, ct.c_uint32)(
            lambda kind: ct.sizeof((Config, Status, Record)[kind])),
        "configure": ct.CFUNCTYPE(ct.c_int, ct.POINTER(Config))(configure),
        "disable": ct.CFUNCTYPE(None)(lambda: None),
        "field": ct.CFUNCTYPE(None, ct.c_uint64)(lambda _field: None),
        "status": ct.CFUNCTYPE(None, ct.POINTER(Status))(status),
        "drain": ct.CFUNCTYPE(ct.c_uint32, ct.POINTER(Record), ct.c_uint32)(drain),
    }
    library = SimpleNamespace(**{"retro_psx_observer_" + key: value for key, value in functions.items()})
    return library, calls, state, records


class ConsoleObserverTests(unittest.TestCase):
    def test_abi_protocol_and_unreached_denominator(self):
        library, calls, state, records = fake_library()
        bind(library)
        session = ConsoleSession(library, Path("system"), Path("saves"))
        session.loaded = True
        result = command(session, {"command": "observe", "targets": [{"pc": "0x80001000", "return": True}],
                                   "ranges": [{"address": "0x80002000", "bytes": 2}], "capacity": 8}, Path("."))
        self.assertEqual(calls[0].targets[0].pc, 0x80001000)
        self.assertEqual(calls[0].ranges[0].bytes, 2)
        self.assertEqual(result["scanned"], 123)
        self.assertEqual(result["matched"], 0)
        self.assertEqual(result["observation"], "incomplete")
        self.assertEqual(result["targets"][0]["entries"], 0)
        state.matched, state.retained, state.entries[0] = 1, 1, 1
        record = command(session, {"command": "observe_read"}, Path("."))["records"][0]
        self.assertEqual(record["ram"], "0b16")
        records[0].ram[0] = 99
        self.assertEqual(record["ram"], "0b16", "returned records must own their bytes")

    def test_invalid_requests_preserve_configuration(self):
        library, calls, _, _ = fake_library()
        observer = Observer(library)
        observer.configure([(0x80001000, True)], [(0x2000, 4)], 8)
        previous = observer.configuration
        requests = [([], [], 8), ([(1, False)], [], 8), ([(0x1000, 1)], [], 8),
                    ([(0x1000, False)] * 2, [], 8), ([(0x1000, False)], [], 129),
                    ([(0x1000, False)], [(0x1fffff, 2)], 8),
                    ([(0x1000, False)], [(0x2000, 513)], 8),
                    ([(0x1000, False)], [(0x2000, 0)], 8)]
        requests.extend(([(0x1000, False)], [(address, 8)], 8) for address in
                        (0x20010000, 0x40010000, 0x60010000, 0xc0010000, 0xe0010000))
        for targets, ranges, capacity in requests:
            with self.assertRaises(ValueError):
                observer.configure(targets, ranges, capacity)
            self.assertIs(observer.configuration, previous)
        self.assertEqual(len(calls), 1)

    def test_physical_and_kernel_ram_aliases_are_admitted(self):
        library, calls, _, _ = fake_library()
        observer = Observer(library)
        for address in (0x10000, 0x80010000, 0xa0010000):
            observer.configure([(0x80001000, False)], [(address, 8)], 8)
            self.assertEqual(observer.configuration.ranges[0].address, address)
        self.assertEqual(len(calls), 3)

    def test_abi_mismatch_refuses(self):
        library, _, _, _ = fake_library()
        library.retro_psx_observer_size = ct.CFUNCTYPE(ct.c_uint32, ct.c_uint32)(lambda _kind: 1)
        with self.assertRaisesRegex(ValueError, "layout mismatch"):
            bind(library)
        with self.assertRaisesRegex(ValueError, "lacks"):
            bind(SimpleNamespace())

    def test_audio_hash_uses_actual_callback_sink_and_ignores_batching(self):
        session = ConsoleSession(None, Path("system"), Path("saves"))
        session.hashes = CaptureHashes()
        session.callbacks["audio_sample"](-2, 7)
        samples = (ct.c_int16 * 4)(4, -5, 6, -7)
        self.assertEqual(session.callbacks["audio_sample_batch"](samples, 2), 2)
        expected = struct.pack("<6h", -2, 7, 4, -5, 6, -7)
        self.assertEqual(session.hashes.status()["audio_sha256"], hashlib.sha256(expected).hexdigest())
        whole = CaptureHashes()
        whole.audio_batch(struct.pack("=6h", -2, 7, 4, -5, 6, -7))
        self.assertEqual(whole.status()["audio_sha256"], session.hashes.status()["audio_sha256"])
        changed = CaptureHashes()
        changed.audio_batch(struct.pack("=6h", -2, 8, 4, -5, 6, -7))
        self.assertEqual(changed.audio_frames, whole.audio_frames)
        self.assertNotEqual(changed.status()["audio_sha256"], whole.status()["audio_sha256"])

    def test_canonical_ram_frame_hashes_show_both_answers(self):
        ram = bytes(0x200000)
        rgb = Framebuffer(2, 1, 12, 1, struct.pack("=3I", 0xff0000, 0x00ff00, 0xdeadbeef))
        packed = Framebuffer(2, 1, 4, 2, struct.pack("=2H", 0xf800, 0x07e0))
        first, second = CaptureHashes(), CaptureHashes()
        for hashes, frame in ((first, rgb), (second, packed)):
            hashes.audio_sample(0, 0)
            hashes.field(ram, frame)
        self.assertEqual(first.status(), second.status())
        self.assertTrue(first.status()["complete"])
        changed = CaptureHashes()
        changed.field(b"\x01" + ram[1:], rgb)
        self.assertNotEqual(changed.status()["ram_sha256"], first.status()["ram_sha256"])
        blue = Framebuffer(2, 1, 4, 2, struct.pack("=2H", 0x001f, 0x07e0))
        changed = CaptureHashes()
        changed.field(ram, blue)
        self.assertNotEqual(changed.status()["frame_sha256"], first.status()["frame_sha256"])
        self.assertFalse(CaptureHashes().status()["complete"])


if __name__ == "__main__":
    unittest.main()

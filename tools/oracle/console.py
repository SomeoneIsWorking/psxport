#!/usr/bin/env python3
"""Build and drive the isolated full-console software reference through libretro."""

from __future__ import annotations

import argparse
import ctypes as ct
import json
import os
from pathlib import Path
import subprocess
import sys

from console_abi import load_library
from console_build import (SCRATCH, activity_lock, build_core, file_sha256, library_name,
                           verified_library)
from console_firmware import (FIRMWARE, prepare_system_directory, select_firmware,
                              verify_loaded_firmware)
from console_session import ConsoleSession, RAM_BYTES


class ProtocolOutput:
    """Keep JSON replies separate from the native core's stdout/stderr diagnostic output."""
    def __init__(self):
        self.stream = os.fdopen(os.dup(sys.stdout.fileno()), "w", buffering=1)

    def write(self, message: dict) -> None:
        self.stream.write(json.dumps(message, sort_keys=True) + "\n")
        self.stream.flush()

    def close(self) -> None:
        self.stream.close()


def integer(value) -> int:
    if type(value) is int:
        return value
    if isinstance(value, str):
        return int(value, 0)
    raise ValueError("expected an integer or prefixed integer string")


def observation_config(session: ConsoleSession, message: dict) -> dict:
    if not session.loaded:
        raise ValueError("PC observation requires loaded console content")
    targets, ranges = message["targets"], message["ranges"]
    if not isinstance(targets, list) or not isinstance(ranges, list):
        raise ValueError("observer targets/ranges must be lists")
    if any(not isinstance(item, dict) or set(item) != {"pc", "return"} for item in targets):
        raise ValueError("observer target fields are exactly pc and return")
    if any(not isinstance(item, dict) or set(item) != {"address", "bytes"} for item in ranges):
        raise ValueError("observer range fields are exactly address and bytes")
    return session.observer.configure(
        [(integer(item["pc"]), item["return"]) for item in targets],
        [(integer(item["address"]), integer(item["bytes"])) for item in ranges],
        integer(message["capacity"]))


def command(session: ConsoleSession, message: dict, output_directory: Path) -> dict:
    if not isinstance(message, dict):
        raise ValueError("command must be a JSON object")
    operation = message.get("command")
    allowed = {"status": {"command"}, "buttons": {"command", "buttons"},
               "step": {"command", "frames"}, "read": {"command", "address", "bytes"},
               "capture": {"command"}, "ram": {"command"}, "quit": {"command"},
               "observe": {"command", "targets", "ranges", "capacity"},
               "observe_read": {"command"}, "observe_off": {"command"},
               "hashes_begin": {"command"}, "hashes": {"command"},
               "insert_card": {"command", "card"}}
    if not isinstance(operation, str) or operation not in allowed or set(message) != allowed[operation]:
        raise ValueError("unknown command or missing/extra fields: " + ", ".join(allowed))
    if operation == "buttons":
        session.set_buttons(message["buttons"])
    elif operation == "step":
        return session.step(message["frames"])
    elif operation == "observe":
        return observation_config(session, message)
    elif operation == "observe_read":
        return session.observer.drain()
    elif operation == "observe_off":
        session.library.retro_psx_observer_disable()
        return session.observer.status()
    elif operation == "insert_card":
        # The image travels as a PATH, not as bytes: a 128 KiB card is 262,144 hex characters and
        # the control protocol bounds a command line at 65,536. Both ends of this protocol are the
        # same machine by construction (the controlling process spawns this one), so the file is
        # readable here.
        card = message["card"]
        if not isinstance(card, str):
            raise ValueError("insert_card takes the path of a memory-card image")
        path = Path(card)
        if not path.is_file():
            raise ValueError(f"memory-card image {path} does not exist")
        return session.insert_card(path.read_bytes())
    elif operation == "hashes_begin":
        return session.begin_hashes()
    elif operation == "hashes":
        if session.hashes is None:
            raise ValueError("scanned 0 hash fields; hashes_begin has not started an observation")
        return session.hashes.status()
    elif operation == "read":
        size = integer(message["bytes"])
        if not 1 <= size <= 256:
            raise ValueError("read requires 1..256 bytes; ram command captures complete main RAM")
        address = integer(message["address"])
        return {"address": address, "bytes": session.read_ram(address, size).hex(),
                "frame": session.frames}
    elif operation == "capture":
        if session.framebuffer is None:
            raise ValueError("scanned 0 video frames; no framebuffer is available to capture")
        path = output_directory / "frame.png"
        session.framebuffer.save_png(path)
        return {"capture": str(path), "sha256": file_sha256(path), "frame": session.frames}
    elif operation == "ram":
        path = output_directory / "ram.bin"
        path.write_bytes(session.read_ram(0, RAM_BYTES))
        return {"ram": str(path), "bytes": RAM_BYTES, "sha256": file_sha256(path),
                "frame": session.frames}
    return session.status()


def flush_native_output() -> None:
    libc = ct.CDLL(None)
    libc.fflush.argtypes = [ct.c_void_p]
    libc.fflush.restype = ct.c_int
    libc.fflush(None)


def run(args, output: ProtocolOutput) -> None:
    library_path, build_manifest = verified_library()
    firmware = select_firmware(args.region, args.bios, args.openbios)
    disc = args.disc.resolve(strict=True)
    if not disc.is_file() or disc.suffix.lower() != ".chd":
        raise ValueError("full-console input must be a self-contained CHD disc")
    if disc.with_suffix(".toc").exists():
        raise ValueError("sibling .toc would override the selected CHD inside Beetle; refuse it")
    digest = file_sha256(disc)
    if args.disc_sha256 is not None and digest != args.disc_sha256.lower():
        raise ValueError(f"disc SHA-256 mismatch: expected {args.disc_sha256}, got {digest}")
    SCRATCH.mkdir(parents=True, exist_ok=True)
    system_directory = prepare_system_directory(SCRATCH, firmware)
    save_directory = SCRATCH / "saves"
    save_directory.mkdir(exist_ok=True)
    existing_saves = sorted(path.name for path in save_directory.iterdir())
    removed_environment = [name for name in os.environ if name.startswith("PSXPORT_")]
    for name in removed_environment:
        del os.environ[name]  # The reference must not inherit product diagnostic/behavior overrides.
    manifest = {"disc": str(disc), "disc_sha256": digest,
                "expected_disc_identity_checked": args.disc_sha256 is not None,
                "firmware": firmware.describe(), "build": build_manifest,
                "existing_save_files": existing_saves,
                "removed_product_environment": sorted(removed_environment),
                "limitation": "State checkpoints must be aligned externally; fields alone are not parity."}
    (SCRATCH / "run.json").write_text(json.dumps(manifest, indent=2) + "\n")

    # Redirect before dlopen: static initializers and fallback variadic logging belong in the same
    # bounded-activity log. The saved protocol descriptor still reaches the controlling process.
    sys.stdout.flush()
    sys.stderr.flush()
    saved_stdout, saved_stderr = os.dup(1), os.dup(2)
    session = None
    try:
        with (SCRATCH / "core.log").open("w") as core_log:
            os.dup2(core_log.fileno(), 1)
            os.dup2(core_log.fileno(), 2)
            session = ConsoleSession(load_library(library_path), system_directory, save_directory,
                                     {"crop_overscan": "smart"} if args.crop_overscan else None)
            session.open(disc)
            flush_native_output()
            manifest["firmware_observation"] = verify_loaded_firmware(
                firmware, (SCRATCH / "core.log").read_text(errors="replace"))
            (SCRATCH / "run.json").write_text(json.dumps(manifest, indent=2) + "\n")
            output.write({"ready": True, "manifest": manifest, "state": session.status()})
            while True:
                line = sys.stdin.readline(65537)
                if not line:
                    break
                if len(line) > 65536:
                    raise ValueError("control command exceeds 65536 characters")
                message = json.loads(line)
                result = command(session, message, SCRATCH)
                output.write({"ok": True, "result": result})
                if message["command"] == "quit":
                    break
    finally:
        if session is not None:
            session.close()
        # Flush the native stdio streams before restoring their descriptors.
        flush_native_output()
        os.dup2(saved_stdout, 1)
        os.dup2(saved_stderr, 2)
        os.close(saved_stdout)
        os.close(saved_stderr)


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    build = commands.add_parser("build", help="build the pinned full software/Mednafen core")
    build.add_argument("--cc", default=os.environ.get("CC", "cc"))
    build.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    build.add_argument("--jobs", type=int, default=2)
    execute = commands.add_parser("run", help="load the full console and accept JSON commands on stdin")
    execute.add_argument("--disc", type=Path, required=True)
    execute.add_argument("--disc-sha256", help="refuse a disc differing from this externally verified hash")
    execute.add_argument("--region", choices=tuple(FIRMWARE), default="na")
    execute.add_argument("--crop-overscan", action="store_true",
                         help="publish the core's active display area instead of the padded "
                              "scanline: crop_overscan=smart, which crops horizontally AND "
                              "vertically (350x240 -> 320x224 on Tomba! 2). The vertical crop is the "
                              "point: the native path deliberately presents more rows than a console "
                              "scans out, so the product is cropped to the guest_scan count it "
                              "reports and the reference to its own active area, and the two agree "
                              "independently. Video only; RAM is unaffected")
    firmware = execute.add_mutually_exclusive_group(required=True)
    firmware.add_argument("--bios", type=Path)
    firmware.add_argument("--openbios", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    output = ProtocolOutput()
    try:
        library_name()  # Explicit platform refusal precedes platform-specific locking/loading.
        with activity_lock():
            if args.operation == "build":
                path = build_core(args.cc, args.cxx, args.jobs)
                output.write({"built": str(path)})
            else:
                run(args, output)
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        output.write({"error": str(error), "complete": False})
        return 1
    finally:
        output.close()


if __name__ == "__main__":
    raise SystemExit(main())

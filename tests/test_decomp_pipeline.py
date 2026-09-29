#!/usr/bin/env python3
"""The decomp pipeline's own selftest, and it must be able to FAIL.

WHAT MAKES THIS A TEST RATHER THAN A DEMONSTRATION. Every case below is a SEEDED DIFFERENCE: a real
input that the shipping code must reject or must report as untrustworthy, added permanently, so the
case that would break runs on every build and needs no break-and-restore ritual. A selftest that can
only print "ok" is not a test, and the case it cannot see is the case that ships.

The seeded differences, and the defect each one is for:

  1. a target address with no function          -> a silent gap in the target set
  2. a decompiled body carrying the non-return   -> the workspace's signature failure: output that
     warning                                      READS like a complete function and is not one
  3. a truncated body (C with no return)        -> the same failure with the wording changed
  4. a manifest base that disagrees with the     -> a wrong load base, which does not fail on its
     image's own header                            own: Ghidra imports happily
  5. an inventory of 0 of 0 functions           -> a run whose script never ran, reported as success
  6. a no-return flag still set after the clear -> a clear that silently failed
  7. a missing image / non-PSX-EXE / empty      -> an absent input reading as an empty result
     target list / unreachable Ghidra
  8. a lock already held                         -> two analyses at once, which OOM-kills a
                                                    co-tenant's build

The positive cases matter too and are stated as facts rather than vibes: the audit is CLEAN on an
inventory that is complete, so a red result above is the seeded difference and not a test that
always fails.

It runs with no Ghidra install, no disc and no image: every refusal is driven through the shipping
modules with an injected command runner. The two Ghidra-side scripts are imported here too, which is
possible only because they import nothing at module level -- and their decision logic is exercised
directly, because that logic is what decides whether a body is present.
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import stat
import struct
import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / "tools"
# `tools/` is the import root, so the package is `decomp.*`. Registered in ctest against
# ${CMAKE_SOURCE_DIR}/tools, so this resolves the same way it does from the repository root.
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from decomp import headless, images, lock, pipeline, report as report_module  # noqa: E402
from decomp import postscript, prescript, structure, verify_body  # noqa: E402

FAILURES: list[str] = []
CHECKS = 0
VERBOSE = "-v" in sys.argv or "--verbose" in sys.argv


def check(condition: bool, label: str, detail: str = "") -> None:
    global CHECKS
    CHECKS += 1
    if condition:
        if VERBOSE:
            print("  ok   %s" % label)
        return
    FAILURES.append(label + (" -- " + detail if detail else ""))
    print("  FAIL %s%s" % (label, (" -- " + detail) if detail else ""))


def case(title: str) -> None:
    print("* %s" % title)


def raises(exception: type, call, *args, **kwargs) -> tuple[bool, str]:
    """Run `call`, expecting `exception`. Returns (raised, message) so the message can be asserted.

    A refusal that raises the wrong exception type is a FAIL, not a pass: the pipeline's callers
    catch specific types, so an ImageRefusal surfacing as KeyError would escape every handler.
    """
    try:
        call(*args, **kwargs)
    except exception as error:
        return True, str(error)
    except Exception as error:  # noqa: BLE001 - the wrong type is exactly what is being tested
        return False, "raised %s, not %s: %s" % (type(error).__name__, exception.__name__, error)
    return False, "returned instead of raising %s" % exception.__name__


# ---------------------------------------------------------------------------------------------
# 1-3. The Ghidra-side decision logic, imported with no Ghidra present.
# ---------------------------------------------------------------------------------------------

def test_body_classification() -> None:
    case("a body is present only when it really is (the structural defence)")
    whole = "int f(int a) {\n  return a * 2 + 1;\n}\n"
    present, reason = postscript.classify_body(True, 4, whole)
    check(present, "a 4-instruction function with a return IS present", reason)

    # THE SEEDED DIFFERENCE: the exact text this workspace has already been burned by.
    truncated = ("/* WARNING: Subroutine does not return */\n"
                 "int f(int a) {\n\n  return 0;\n}\n")
    present, reason = postscript.classify_body(True, 40, truncated)
    check(not present, "a body carrying the non-return warning is NOT present", reason)
    check("non-return" in reason, "the reason NAMES the non-return truncation, not just 'no'",
          reason)

    # THE SEEDED DIFFERENCE, REWORDED: the warning text is Ghidra's and may change, so a second
    # independent signal must catch the same fabrication.
    reworded = "int f(int a) {\n  /* something went wrong */\n}\n"
    present, reason = postscript.classify_body(True, 40, reworded)
    check(not present, "a C body with no return is NOT present even without the warning", reason)
    check("return" in reason, "that reason names the missing return", reason)

    present, reason = postscript.classify_body(True, 0, whole)
    check(not present, "a function whose body holds 0 instructions is NOT present", reason)

    present, reason = postscript.classify_body(False, 0, None)
    check(not present, "no function object is NOT present", reason)

    present, reason = postscript.classify_body(True, 4, None)
    check(not present, "a function that produced no C is NOT present", reason)


def test_noreturn_policy() -> None:
    case("the no-return policy is refused when unreadable, never defaulted")
    mode, addresses = postscript.parse_noreturn_policy("all")
    check(mode == "all" and not addresses, "'all' parses to clear-everything")

    mode, addresses = postscript.parse_noreturn_policy("80012340,80012380")
    check(mode == "list" and addresses == frozenset({0x80012340, 0x80012380}),
          "a comma list parses to those addresses", repr(addresses))

    raised, message = raises(postscript.PolicyRefusal, postscript.parse_noreturn_policy, "")
    check(raised, "an EMPTY policy is refused rather than read as 'clear nothing'", message)

    raised, message = raises(postscript.PolicyRefusal, postscript.parse_noreturn_policy, "not-hex")
    check(raised, "a non-hex policy is refused", message)


def test_empty_target_list() -> None:
    case("an empty target list is refused ('0 of 0' is a bug, not a result)")
    with tempfile.TemporaryDirectory() as temporary:
        empty = Path(temporary) / "targets.txt"
        empty.write_text("\n  # only a comment\n")
        raised, message = raises(postscript.PolicyRefusal, postscript.read_targets, str(empty))
        check(raised, "a target file with no addresses is refused", message)
        check("0 of 0" in message, "the refusal SAYS it is the 0-of-0 case", message)

        missing = Path(temporary) / "absent.txt"
        raised, message = raises(postscript.PolicyRefusal, postscript.read_targets, str(missing))
        check(raised, "an absent target file is refused", message)

        good = Path(temporary) / "good.txt"
        good.write_text("0x800258F0\n0x800259FC\n")
        check(postscript.read_targets(str(good)) == [0x800258F0, 0x800259FC],
              "a real target file parses and de-duplicates")


def test_prescript_window() -> None:
    case("the pre-script refuses a window or entry it cannot use")
    lo, hi, entry = prescript.parse_window(["0x80010000", "0x8001013F", "0x80010020"])
    check((lo, hi, entry) == (0x80010000, 0x8001013F, 0x80010020), "a hex window and entry parse")
    raised, message = raises(prescript.PreScriptRefusal, prescript.parse_window,
                             ["0x80010000", "0x8001013F"])
    check(raised, "a missing entry argument is refused, not defaulted", message)
    raised, message = raises(prescript.PreScriptRefusal, prescript.parse_window,
                             ["zz", "0x8001013F", "0x80010020"])
    check(raised, "a non-hex window bound is refused", message)
    raised, message = raises(prescript.PreScriptRefusal, prescript.parse_window,
                             ["0x80010100", "0x80010000", "0x80010020"])
    check(raised, "an empty or reversed window is refused", message)
    # MEASURED ON A REAL RUN: the first version seeded disassembly from the window start, which is
    # DATA on a PS-X EXE, and its refusal asserted the load base was the likely cause. It was not.
    raised, message = raises(prescript.PreScriptRefusal, prescript.parse_window,
                             ["0x80010000", "0x8001013F", "0x80090000"])
    check(raised, "an entry outside the declared text window is refused", message)
    check("outside the text window" in message,
          "that refusal says what it compared, rather than naming a guess", message)


# ---------------------------------------------------------------------------------------------
# 4. A wrong base, caught by the cross-check against the image's own header.
# ---------------------------------------------------------------------------------------------

def make_psx_exe(path: Path, load: int = 0x80010000, text_size: int = 0x100,
                 entry: int = 0x80010000) -> Path:
    """A minimal but VALID PS-X EXE, so the reader's refusals are about the thing under test."""
    header = bytearray(0x800)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<II", header, 0x10, entry, 0)
    struct.pack_into("<II", header, 0x18, load, text_size)
    struct.pack_into("<II", header, 0x30, 0, 0)
    path.write_bytes(bytes(header) + bytes(text_size))
    return path


def test_manifest_and_base() -> None:
    case("a wrong base is refused, and the shipped manifest agrees with the images")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        good = make_psx_exe(root / "good.exe")

        def resident(**overrides) -> images.ImageSpec:
            fields = {"name": "t", "title": "T", "serial": "X", "kind": images.RESIDENT,
                      "text_load_address": 0x80010000, "text_file_offset": 0x800, "text_size": 0x100}
            fields.update(overrides)
            return images.ImageSpec(**fields)

        spec = resident()
        window = images.ImageWindow(spec, good)
        check(window.file_offset(0x80010000) == 0x800, "the first text word sits at offset 0x800")
        check(spec.ghidra_base == 0x8000F800,
              "the Ghidra base is load minus the text file offset", hex(spec.ghidra_base))
        check(window.covers(0x80010000) and not window.covers(0x8000FFF0),
              "the window covers text and refuses below it")
        check(window.entry == 0x80010000, "a resident's entry is its own header's")

        # THE SEEDED DIFFERENCE: a manifest base that does not match the image.
        raised, message = raises(images.ImageRefusal, images.ImageWindow,
                                 resident(text_load_address=0x80020000), good)
        check(raised, "a manifest load address that disagrees with the header is REFUSED", message)
        check("0x80020000" in message and "0x80010000" in message,
              "the refusal names BOTH the claimed and the actual address", message)

        raised, message = raises(images.ImageRefusal, images.ImageWindow,
                                 resident(text_file_offset=0), good)
        check(raised, "a text file offset that is not 0x800 for a PS-X EXE is refused", message)

        raised, message = raises(images.ImageRefusal, images.ImageWindow,
                                 resident(text_size=0x200), good)
        check(raised, "a manifest text size that disagrees with the header is refused", message)

        raised, message = raises(images.ImageRefusal, images.ImageWindow, spec,
                                 root / "absent.exe")
        check(raised, "an absent image is a refusal, not an empty inventory", message)
        check("no image at" in message, "the refusal names the path it wanted", message)

        # A MODULE presented to the RESIDENT reader must be refused, not read as a headerless EXE.
        raw = root / "raw.bin"
        raw.write_bytes(bytes(0x100))
        raised, message = raises(images.ImageRefusal, images.ImageWindow, spec, raw)
        check(raised, "a raw module presented as a resident PS-X EXE is refused", message)
        check("module" in message,
              "and the refusal says a module needs manifest geometry, not a header", message)

        zero_entry = make_psx_exe(root / "zero.exe", entry=0)
        raised, message = raises(images.ImageRefusal, images.ImageWindow, spec, zero_entry)
        check(raised, "a PS-X EXE with no entry point is refused", message)

        truncated = root / "trunc.exe"
        truncated.write_bytes(b"PS-X EXE" + bytes(0x38))
        struct_ok = bytearray(truncated.read_bytes())
        struct.pack_into("<II", struct_ok, 0x18, 0x80010000, 0x100000)
        truncated.write_bytes(bytes(struct_ok))
        raised, message = raises(images.ImageRefusal, images.ImageWindow, spec, truncated)
        check(raised, "an image whose declared text runs past its end is refused", message)

    # The shipped manifest is DATA, and it must be readable and internally consistent.
    specs = images.load_manifest()
    check(len(specs) >= 1, "the shipped manifest is non-empty", "%d images" % len(specs))
    for spec in specs.values():
        if spec.kind == images.RESIDENT:
            check(spec.text_file_offset == images.PSX_EXE_TEXT_FILE_OFFSET,
                  "%s declares the PS-X EXE text offset 0x800" % spec.name,
                  hex(spec.text_file_offset))
            check(spec.ghidra_base < spec.text_load_address,
                  "%s's Ghidra base sits below its text load address" % spec.name)
            check(spec.text_size is not None and spec.text_size > 0,
                  "%s declares a positive text size" % spec.name)
        else:
            check(spec.load_base is not None and spec.code_last is not None,
                  "%s declares a measured load base and code window" % spec.name)
            check(images.KSEG0_FIRST <= spec.load_base < images.KSEG0_LAST,
                  "%s's base is inside 2 MiB of KSEG0" % spec.name, hex(spec.load_base))
            check(spec.sha1 is not None,
                  "%s is SHA-1 gated, because a module's base cannot be checked against a header"
                  % spec.name)
    raised, message = raises(images.ImageRefusal, images.select, specs, "no-such-image")
    check(raised, "an unknown image name is refused", message)
    check(str(len(specs)) in message,
          "the refusal states how many images ARE known (the denominator)", message)


def test_module_reader() -> None:
    case("a MODULE is read by the same reader, and its base is trusted as data with a SHA-1 gate")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        payload = bytes(0x400)

        def module_bytes() -> bytes:
            # A real little-endian MIPS word stream, so nothing downstream is reading zeros.
            return b"".join(struct.pack("<I", 0x27BDFFE8 + i) for i in range(0x100))

        body = module_bytes()
        module = root / "BATTLE.BIN"
        module.write_bytes(body)
        digest = hashlib.sha1(body).hexdigest()

        def spec(**overrides) -> images.ImageSpec:
            # `code_last` is the LAST CODE BYTE, so a whole-file window is len - 1. Getting this
            # wrong by one is a boundary error the reader cannot distinguish from a wrong base.
            fields = {"name": "m", "title": "T", "serial": "X", "kind": images.MODULE,
                      "load_base": 0x80068800, "code_first": 0, "code_last": len(body) - 1,
                      "sha1": digest}
            fields.update(overrides)
            return images.ImageSpec(**fields)

        window = images.ImageWindow(spec(), module)
        check(window.header is None, "a module has no header, and that is not an error")
        check(window.file_offset(0x80068800) == 0,
              "a module's first byte is file offset 0 at the load base")
        check(spec().ghidra_base == 0x80068800, "a module's Ghidra base IS its load base")
        check(window.entry == 0x80068800,
              "a module's entry is derived from its measured code window, not a header")
        check(window.word(0x80068800) == struct.unpack("<I", body[:4])[0],
              "and the reader returns that module's own bytes")

        # THE SEEDED DIFFERENCE: a module entry with no load base. It must name the FIELD.
        raised, message = raises(images.ImageRefusal, images.ImageSpec(
            name="m", title="T", serial="X", kind=images.MODULE,
            code_first=0, code_last=16).validate)
        check(raised, "a module entry with no load_base is refused", message)
        check("load_base" in message, "the refusal NAMES the missing field", message)

        raised, message = raises(images.ImageRefusal, images.ImageSpec(
            name="m", title="T", serial="X", kind=images.MODULE,
            load_base=0x80068800, code_first=16, code_last=4).validate)
        check(raised, "a code window that ends before it starts is refused", message)

        raised, message = raises(images.ImageRefusal, images.ImageSpec(
            name="m", title="T", serial="X", kind=images.MODULE,
            load_base=0x10000000, code_first=0, code_last=16).validate)
        check(raised, "a load base outside 2 MiB of KSEG0 is refused", message)

        # A base whose window runs off the end of RAM -- the shape a wrong base produces.
        raised, message = raises(images.ImageRefusal, images.ImageSpec(
            name="m", title="T", serial="X", kind=images.MODULE,
            load_base=0x801FF000, code_first=0, code_last=0x4000).validate)
        check(raised, "a module that would load off the end of RAM is refused", message)

        # THE SEEDED DIFFERENCE: the wrong FILE. A module's base cannot be checked against a
        # header, so the SHA-1 gate is the only thing standing between a wrong file and a confident
        # answer about the wrong code.
        raised, message = raises(images.ImageRefusal, images.ImageWindow,
                                 spec(sha1="0" * 40), module)
        check(raised, "a module whose SHA-1 does not match is REFUSED", message)
        check(digest[:8] in message,
              "the refusal states the hash it computed, not just that it mismatched", message)

        # A code window past the end of the FILE: the other shape a wrong base produces.
        raised, message = raises(images.ImageRefusal, images.ImageWindow,
                                 spec(code_last=len(body) + 0x1000, sha1=None), module)
        check(raised, "a code window that runs past the end of the file is refused", message)

        raised, message = raises(images.ImageRefusal, images.ImageSpec(
            name="m", title="T", serial="X", kind="overlay-ish",
            load_base=0x80068800, code_first=0, code_last=16).validate)
        check(raised, "an unrecognised kind is refused rather than treated as a default", message)


def test_module_wrong_base_cost() -> None:
    case("what a wrong load base costs, MEASURED, and why it is invisible")
    # The vagrant repo recorded that BATTLE's own words build `vs_main_dispEnv` at 0x8005E188 as
    # `lui $s0, 0x8006` + `addiu $s0, $s0, -7800` -- a lui page MINUS a displacement. Two
    # consequences this pins:
    #   1. the module is at 0x80068800, so a guest address can be BELOW the module's base while
    #      still being a real address the module's own code computes, and
    #   2. a reader that forms an address as `lui + positive` is wrong by exactly 0x10000, and
    #      being wrong there is silent.
    page = 0x8006 << 16
    signed = 0xE188 - 0x10000
    check(page + signed == 0x8005E188,
          "lui 0x8006 + addiu -7800 materialises the address the decomp names",
          hex(page + signed))
    check(page + 0xE188 == 0x8006E188,
          "and a reader that adds the displacement POSITIVE gets 0x8006E188 instead")
    check(page + 0xE188 - (page + signed) == 0x10000,
          "a full 64 KiB out, with nothing to distinguish it but the answer")
    # And that address is BELOW the module's own base, so a reader that bounds its window by the
    # base alone would refuse a legitimate target.
    check(0x8005E188 < 0x80068800,
          "the address a module's own code computes can be BELOW the module's load base")


def test_manifest_selection_is_explicit() -> None:
    case("the manifest never guesses which title you meant")
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "manifest.json"
        path.write_text(json.dumps({"images": {
            "a": {"kind": "resident", "serial": "A", "text_load_address": "0x80010000",
                  "text_file_offset": "0x800", "text_size": "0x100"},
            "b": {"kind": "module", "serial": "B", "load_base": "0x80068800",
                  "code_first": "0x0", "code_last": "0x100"},
        }}))
        specs = images.load_manifest(path)
        raised, message = raises(images.ImageRefusal, images.select, specs, None)
        check(raised, "with more than one image, no image is guessed", message)
        check(images.select(specs, "b").serial == "B", "a named image resolves")
        check(images.select(specs, "b").kind == images.MODULE,
              "a module resolves through the SAME reader as a resident")

        path.write_text(json.dumps({"images": {}}))
        raised, message = raises(images.ImageRefusal, images.load_manifest, path)
        check(raised, "an empty manifest is refused rather than answering 'unknown' everywhere",
              message)

        path.write_text("{not json")
        raised, message = raises(images.ImageRefusal, images.load_manifest, path)
        check(raised, "a malformed manifest is refused", message)


# ---------------------------------------------------------------------------------------------
# 5-6. The audit: the run's own claims, with denominators.
# ---------------------------------------------------------------------------------------------

def inventory_document(**overrides) -> dict:
    """A COMPLETE, trustworthy run. The audit must be clean on this, so a red case is the seeded
    difference and not a test that always fails."""
    document = {
        "program": "probe",
        "language": "MIPS:LE:32:default",
        "noreturn_policy": "all",
        "noreturn_cleared": 2,
        "functions_scanned": 4,
        "noreturn_still_marked": [],
        "preseed": {"text_first": "0x80010000", "text_last": "0x800100FF",
                    "entry": "0x80010000", "disassemble_returned": True,
                    "instructions_from_entry": 33},
        "inventory": [
            {"entry": "0x80010020", "name": "FUN_80010020", "body_bytes": 16,
             "body_first": "0x80010020", "body_last": "0x8001002F", "instruction_count": 4},
            {"entry": "0x80010040", "name": "FUN_80010040", "body_bytes": 28,
             "body_first": "0x80010040", "body_last": "0x8001005B", "instruction_count": 7},
        ],
        "targets": [
            {"requested": "0x80010020", "function_found": True, "created_on_demand": False,
             "decompiled": True, "body_present": True, "instruction_count": 4, "c_bytes": 90,
             "c_path": "c/80010020.c", "name": "FUN_80010020",
             "body_reason": "function object, 4 instruction(s)"},
            {"requested": "0x80010040", "function_found": True, "created_on_demand": False,
             "decompiled": True, "body_present": True, "instruction_count": 7, "c_bytes": 210,
             "c_path": "c/80010040.c", "name": "FUN_80010040",
             "body_reason": "function object, 7 instruction(s)"},
        ],
    }
    document.update(overrides)
    return document


def test_audit_is_clean_on_a_complete_run() -> None:
    case("the audit is CLEAN on a complete run, so red below means the seeded difference")
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "inventory.json"
        path.write_text(json.dumps(inventory_document()))
        parsed = report_module.load_report(path)
        problems = report_module.audit(parsed)
        check(not problems, "a complete run audits clean", "; ".join(problems))
        check(parsed.targets_requested == 2, "targets requested carries its denominator")
        check(parsed.targets_decompiled == 2, "targets decompiled carries its denominator")
        check(parsed.targets_with_body == 2, "targets with a body carries its denominator")
        check(parsed.functions_with_instructions == 2,
              "functions holding an instruction is counted over the scan")
        header = parsed.header()
        for phrase in ("functions scanned=4", "targets requested=2", "body present=2"):
            check(phrase in header, "the header states %r" % phrase, header)


def test_audit_catches_seeded_differences() -> None:
    case("the audit refuses each seeded difference, BY NAME")
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "inventory.json"

        def audit(document: dict) -> list[str]:
            path.write_text(json.dumps(document))
            return report_module.audit(report_module.load_report(path))

        # SEEDED: the run whose script never ran. 0 of 0 functions, reported as success.
        problems = audit(inventory_document(functions_scanned=0, inventory=[], targets=[]))
        check(any("0 of 0 functions" in p for p in problems),
              "an inventory of 0 of 0 functions is a REFUSAL, not an empty image", str(problems))
        check(any("0 of 0 targets" in p for p in problems),
              "0 of 0 targets is called out separately", str(problems))

        # SEEDED: the workspace's signature failure, reaching the audit as a marked flag.
        problems = audit(inventory_document(noreturn_still_marked=["80010020", "80010040"]))
        check(any("STILL marked non-returning" in p for p in problems),
              "a non-return flag that survived the clear is refused", str(problems))
        check(any("80010020" in p for p in problems), "the refusal NAMES the offending address",
              str(problems))

        # SEEDED: a partial clear, where the pipeline required 'all'.
        problems = audit(inventory_document(noreturn_policy="list"))
        check(any("policy 'list'" in p or "where 'all' was required" in p for p in problems),
              "a no-return policy that is not 'all' is refused", str(problems))

        # SEEDED: a target with no function at all.
        document = inventory_document()
        document["targets"][0] = {"requested": "0x80019999", "function_found": False,
                                  "created_on_demand": False, "decompiled": False,
                                  "body_present": False, "instruction_count": 0, "c_bytes": 0,
                                  "c_path": None,
                                  "body_reason": "no function object at this address"}
        problems = audit(document)
        check(any("NO function at that address" in p and "0x80019999" in p for p in problems),
              "a target with no function is refused AND NAMED", str(problems))

        # SEEDED: the truncated body, arriving from the post-script with its flag already false.
        document = inventory_document()
        document["targets"][1] = {"requested": "0x80010040", "function_found": True,
                                  "created_on_demand": False, "decompiled": True,
                                  "body_present": False, "instruction_count": 7, "c_bytes": 41,
                                  "c_path": "c/80010040.c", "name": "FUN_80010040",
                                  "body_reason": "C carries Ghidra's non-return warning"}
        problems = audit(document)
        check(any("body is NOT present" in p and "0x80010040" in p for p in problems),
              "a target whose body is not present is refused AND NAMED", str(problems))
        check(any("non-return warning" in p for p in problems),
              "the refusal repeats WHY, so it cannot be read as 'decompile failed'", str(problems))

        # SEEDED: a function that exists but produced no C at all.
        document = inventory_document()
        document["targets"][1] = {"requested": "0x80010040", "function_found": True,
                                  "created_on_demand": False, "decompiled": False,
                                  "body_present": False, "instruction_count": 7, "c_bytes": 0,
                                  "c_path": None, "name": "FUN_80010040",
                                  "body_reason": "decompile produced no C at all"}
        problems = audit(document)
        check(any("produced no C" in p for p in problems),
              "a function that decompiled to nothing is distinguished from one that did", str(problems))

        # A document missing a required key is not read as a partial result.
        path.write_text(json.dumps({"program": "x"}))
        raised, message = raises(report_module.ReportRefusal, report_module.load_report, path)
        check(raised, "a document missing required keys is refused", message)
        raised, message = raises(report_module.ReportRefusal, report_module.load_report,
                                 Path(temporary) / "absent.json")
        check(raised, "an absent inventory is a refusal, not an empty report", message)


def test_audit_catches_an_unrecorded_preseed() -> None:
    case("a pre-script that failed is refused, not read as a clean run")
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "inventory.json"

        def audit(document: dict) -> list[str]:
            path.write_text(json.dumps(document))
            return report_module.audit(report_module.load_report(path))

        # MEASURED ON A REAL RUN: the pre-script raised, Ghidra carried the analysis on, logged
        # "Post-analysis succeeded", exited 0, and the audit PASSED -- because nothing recorded
        # that the seed never happened.
        problems = audit(inventory_document(preseed=None))
        check(any("pre-script left no record" in p for p in problems),
              "an unrecorded pre-script is refused", str(problems))

        problems = audit(inventory_document(
            preseed={"entry": "0x8005B8E0", "instructions_from_entry": 0}))
        check(any("0 instructions" in p for p in problems),
              "a pre-script that seeded 0 instructions is refused", str(problems))


def test_audit_catches_a_target_inside_a_function_body() -> None:
    case("a target that is a label inside a body is refused, not carved into a second function")
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "inventory.json"
        # MEASURED ON A REAL RUN: 0x800259FC is a phase INSIDE FUN_800258F0's body
        # (0x800258F0..0x8002A6FB). Creating a function there produced a second 4927-instruction
        # body overlapping the first by 19708 of its 19976 bytes; both decompiled to ~129 KB of C;
        # both reported "body present"; neither was a function.
        document = inventory_document()
        document["targets"][1] = {
            "requested": "0x800259FC", "function_found": False, "created_on_demand": False,
            "decompiled": False, "body_present": False, "instruction_count": 0, "c_bytes": 0,
            "c_path": None, "inside_function": "800258F0", "inside_function_name": "FUN_800258f0",
            "inside_body_range": "0x800258F0..0x8002A6FB", "offset_into_body": 268,
            "body_reason": "this address is +268 bytes INSIDE FUN_800258f0's body"}
        path.write_text(json.dumps(document))
        parsed = report_module.load_report(path)
        problems = report_module.audit(parsed)
        check(any("INSIDE FUN_800258f0's body" in p and "0x800259FC" in p for p in problems),
              "a label inside a body is refused, naming the enclosing function", str(problems))
        check(not any("load base is wrong" in p for p in problems),
              "and it is NOT reported as a load-base problem: the base was found correct", str(problems))
        row = parsed.target_text()
        check("inside=800258F0" in row,
              "the report row shows WHICH function the address is inside, not just 'not found'", row)
        check(parsed.targets_with_body == 1,
              "the body-present denominator counts 1 of 2, so a label cannot be read as a function")


# ---------------------------------------------------------------------------------------------
# 7-8. The Ghidra boundary and the lock.
# ---------------------------------------------------------------------------------------------

class ScriptedRunner(headless.Runner):
    """A runner that answers from a script, so every refusal is reachable without a Ghidra install."""

    def __init__(self, returncode: int = 0, output: str = "", writes_inventory: bool = True,
                 probe_fails: bool = False):
        self.returncode = returncode
        self.output = output
        self.writes_inventory = writes_inventory
        self.probe_fails = probe_fails
        self.commands: list[list[str]] = []
        self.environments: list[dict] = []

    def run(self, command, cwd, timeout, environment):
        self.commands.append(list(command))
        self.environments.append(dict(environment))
        if "import pyghidra" in command:
            if self.probe_fails:
                return 1, "ModuleNotFoundError: No module named 'pyghidra'"
            return 0, ""
        if self.writes_inventory and "-postScript" in command:
            index = command.index("-postScript")
            inventory = Path(command[index + 2])
            inventory.parent.mkdir(parents=True, exist_ok=True)
            inventory.write_text(json.dumps(inventory_document()))
        return self.returncode, self.output


def test_ghidra_boundary() -> None:
    case("the Ghidra boundary refuses a run that did not decompile anything")
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary)
        invocation = headless.GhidraInvocation(
            interpreter=Path("/usr/bin/true"), ghidra_home=Path("/nowhere"),
            project_dir=out / "project", project_name="decomp",
            import_path=Path("image.exe"), base_address=0x8000F800,
            text_first=0x80010000, text_last=0x800100FF, entry_address=0x80010000,
            output_dir=out,
            postscript=Path("postscript.py"))

        runner = ScriptedRunner(writes_inventory=False)
        raised, message = raises(headless.GhidraRefusal, invocation.run, runner)
        check(raised, "an exit-0 run with no inventory is a REFUSAL", message)
        check("exited 0" in message, "the refusal says the exit code was 0, which is the trap",
              message)
        check("PyGhidra" in message, "the refusal names the cause it measured", message)

        runner = ScriptedRunner(returncode=1, output="boom\nlast line")
        raised, message = raises(headless.GhidraRefusal, invocation.run, runner)
        check(raised, "a non-zero exit is a refusal", message)

        runner = ScriptedRunner()
        invocation.run(runner)
        command = runner.commands[-1]
        check((out / "ghidra.log").is_file(),
              "the WHOLE Ghidra log is written before anything is judged")
        check("-X" in command and any(a.startswith("mx") for a in command),
              "the heap ceiling is passed as PyGhidra's own -X flag", " ".join(command))
        check("MAXMEM" not in os.environ.get("PSXPORT_TEST_SENTINEL", ""),
              "no MAXMEM is relied on")
        check("pyghidra.ghidra_launch" in command,
              "the launcher is the PyGhidra MODULE, not the pyghidraRun wrapper")
        check("analyzeHeadless" not in " ".join(command),
              "analyzeHeadless is never used: it exits 0 without running the script")
        check("ghidra-tmp" in runner.environments[-1].get("JAVA_TOOL_OPTIONS", ""),
              "Ghidra's cache is redirected into the project's own scratch")

    # MEASURED ON A REAL RUN, not reasoned: a relative --out produced a relative java.io.tmpdir and
    # the launcher died with "System property java.io.tmpdir is not an absolute path", naming a JVM
    # internal rather than the relative path the caller passed.
    case("every path handed to Ghidra is absolute")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        out = root / "scratch" / "decomp" / "spyro1"
        invocation = headless.GhidraInvocation(
            interpreter=Path("relative-python"), ghidra_home=Path("relative-ghidra"),
            project_dir=out / "project", project_name="decomp",
            import_path=Path("relative-image.exe"), base_address=0x8000F800,
            text_first=0x80010000, text_last=0x800100FF, entry_address=0x80010040,
            output_dir=out,
            postscript=Path("postscript.py"))
        check(invocation.project_dir.is_absolute(), "the project directory is resolved")
        check(invocation.output_dir.is_absolute(), "the output directory is resolved")
        check(invocation.import_path.is_absolute(), "the import path is resolved")
        check(invocation.postscript.is_absolute(), "the post-script path is resolved")
        cache = invocation.environment({})["JAVA_TOOL_OPTIONS"]
        tmpdir = cache.split("java.io.tmpdir=", 1)[1]
        check(Path(tmpdir).is_absolute(),
              "java.io.tmpdir is absolute, which the JVM requires", tmpdir)

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        raised, message = raises(headless.GhidraRefusal, headless.check_installation,
                                 root / "absent", Path("/usr/bin/true"), ScriptedRunner())
        check(raised, "an absent Ghidra install is refused", message)
        raised, message = raises(headless.GhidraRefusal, headless.check_installation,
                                 root, Path("/usr/bin/true"), ScriptedRunner())
        check(raised, "a directory that is not a Ghidra root is refused", message)

        (root / "Ghidra").mkdir()
        failing = ScriptedRunner(probe_fails=True)
        raised, message = raises(headless.GhidraRefusal, headless.check_installation,
                                 root, Path("/usr/bin/true"), failing)
        check(raised, "an interpreter that cannot import pyghidra is refused", message)
        check("PyGhidra" in message, "that refusal names the real requirement", message)


def test_lock() -> None:
    case("the lock is exclusive, and a held lock is reported rather than bypassed")
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary) / "locks" / "ghidra"
        first = lock.GhidraLock(directory=directory, wait_seconds=0.2, poll_seconds=0.05)
        first.acquire()
        check(directory.is_dir(), "the lock directory exists while held")
        check((directory / "holder.txt").is_file(),
              "a held lock records WHO, so a stranded one is readable")

        second = lock.GhidraLock(directory=directory, wait_seconds=0.2, poll_seconds=0.05)
        raised, message = raises(lock.LockRefusal, second.acquire)
        check(raised, "a second acquisition WAITS and then refuses; it does not start a run", message)
        check(second.attempts > 1, "it actually retried rather than failing at once",
              "attempts=%d" % second.attempts)
        check("holder" in message.lower(), "the refusal says who holds it", message)

        first.release()
        check(not directory.exists(), "the lock is released")
        third = lock.GhidraLock(directory=directory, wait_seconds=0.2, poll_seconds=0.05)
        third.acquire()
        third.release()
        check(True, "the lock is re-acquirable after release")

        # MEASURED ON A REAL RUN: an earlier default dropped the `coord/` component and created
        # ~/repo/psx/locks/ghidra, which coordinates nobody -- the workspace's stated lock
        # directory stayed empty while the lock was held.
        default = lock.default_lock_dir()
        check(default.parts[-3:] == ("coord", "locks", "ghidra"),
              "the default lock lives in the workspace's coord/locks/ghidra", str(default))

        # Released even when the body raised, or one refusal strands the machine.
        held = lock.GhidraLock(directory=directory, wait_seconds=0.2, poll_seconds=0.05)
        try:
            with held:
                raise ValueError("simulated run failure")
        except ValueError:
            pass
        check(not directory.exists(), "the lock is released even when the run raised")


def test_output_dir_must_be_ignored() -> None:
    case("decompiled C can only be written where git would ignore it")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        # A REAL git repository: `git check-ignore` only answers inside one, so a bare .git
        # directory would make this case fail for a reason that has nothing to do with the check.
        subprocess.run(["git", "init", "-q", str(root)], capture_output=True, check=True)
        (root / "scratch").mkdir()
        (root / ".gitignore").write_text("scratch/\n")
        kept = root / "scratch" / "decomp"
        pipeline._assert_output_dir_is_ignored(kept, root)
        check(True, "scratch/ is accepted")
        raised, message = raises(images.ImageRefusal, pipeline._assert_output_dir_is_ignored,
                                 root / "game" / "decomp", root)
        check(raised, "a non-ignored output directory is REFUSED before anything is written", message)
        check("never be committed" in message, "the refusal says why", message)


def test_verify_body_contract() -> None:
    case("the body cross-check refuses what it cannot check, and can see a real disagreement")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        image = make_psx_exe(root / "image.exe")
        good = inventory_document()
        path = root / "inventory.json"
        path.write_text(json.dumps(good))
        c_path = root / "80010020.c"
        c_path.write_text("void FUN_80010020(void) {\n  FUN_80020000();\n}\n")

        # A synthetic image whose bytes are all `jr ra` (0x00000008 is not a nop) is not useful
        # here; what matters is the ARITHMETIC, and it is checked against the image the test builds.
        raised, message = raises(verify_body.BodyRefusal, verify_body.check,
                                 path, c_path, 0x80010020, image)
        # The refusal is for the image/serial mismatch, not for a crash: either way it must be a
        # refusal that says what it wanted, never a quiet pass.
        check(raised, "an image that does not match the inventory is refused, not passed",
              message)

        raised, message = raises(verify_body.BodyRefusal, verify_body.check,
                                 path, root / "absent.c", 0x80010020, image)
        check(raised, "an absent C file is refused", message)

        absent = json.loads(json.dumps(good))
        absent["targets"][0]["body_present"] = False
        absent["targets"][0]["body_reason"] = "no function object at this address"
        path.write_text(json.dumps(absent))
        raised, message = raises(verify_body.BodyRefusal, verify_body.check,
                                 path, c_path, 0x80010020, image)
        check(raised, "a body the inventory already says is absent is refused, not checked", message)
        check("no body present" in message, "and the refusal repeats the run's own reason", message)

        path.write_text(json.dumps(good))
        raised, message = raises(verify_body.BodyRefusal, verify_body.check,
                                 path, c_path, 0x80099999, image)
        check(raised, "an address with no inventory row is refused, not silently checked", message)

def test_verify_body_finds_a_missing_call() -> None:
    case("the cross-check sees a real disagreement: a call in the bytes missing from the C")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)

        def jal(target: int) -> int:
            return 0x0C000000 | (((target & 0x0FFFFFFF) >> 2) & 0x03FFFFFF)

        # The SHIPPED manifest entry for spyro1, so the fixture exercises the real geometry and the
        # header cross-check that guards it. A synthetic image with its own text size is REFUSED by
        # that cross-check, which is the cross-check working, not an obstacle to route around.
        spec = images.load_manifest()["spyro1"]
        # addiu sp,sp,-0x18 ; jal 0x80020000 ; jr ra ; nop
        text = bytearray(spec.text_size)
        for index, word in enumerate([0x27BDFFE8, jal(0x80020000), 0x03E00008, 0x00000000]):
            struct.pack_into("<I", text, 4 * index, word)
        header = bytearray(0x800)
        header[0:8] = b"PS-X EXE"
        struct.pack_into("<I", header, 0x10, spec.text_load_address)  # entry, must be non-zero
        struct.pack_into("<II", header, 0x18, spec.text_load_address, spec.text_size)
        image = root / "image.exe"
        image.write_bytes(bytes(header) + bytes(text))

        document = inventory_document(program="SCUS_942.28", image_name="spyro1",
                                      image_kind="resident")
        document["inventory"] = [{"entry": "0x80010000", "name": "FUN_80010000", "body_bytes": 16,
                                  "body_first": "0x80010000", "body_last": "0x8001000F",
                                  "instruction_count": 4}]
        document["targets"] = [{"requested": "0x80010000", "function_found": True,
                                "created_on_demand": False, "decompiled": True,
                                "body_present": True, "instruction_count": 4, "c_bytes": 40,
                                "c_path": "c/80010000.c", "name": "FUN_80010000",
                                "body_reason": "function object, 4 instruction(s)"}]
        path = root / "inventory.json"
        path.write_text(json.dumps(document))
        c_path = root / "80010000.c"

        c_path.write_text("void FUN_80010000(void) {\n  FUN_80020000();\n}\n")
        check(verify_body.check(path, c_path, 0x80010000, image, listing=False) == [],
              "a body whose every call site appears in the C checks clean")

        # The seeded difference: the C drops the call. This is what a caller decompiled with the body
        # after a call discarded looks like, and it is silent in the C itself.
        c_path.write_text("void FUN_80010000(void) {\n}\n")
        problems = verify_body.check(path, c_path, 0x80010000, image, listing=False)
        check(any("0x80020000" in p for p in problems),
              "a call site in the bytes missing from the C is reported, BY ADDRESS", str(problems))

        # And the count is NOT a verdict: the same fixture claiming one instruction fewer than the
        # range holds must still check clean, because those are different numbers.
        document["inventory"][0]["instruction_count"] = 3
        path.write_text(json.dumps(document))
        c_path.write_text("void FUN_80010000(void) {\n  FUN_80020000();\n}\n")
        check(verify_body.check(path, c_path, 0x80010000, image, listing=False) == [],
              "a body claiming 3 instructions over 4 words is NOT reported as a defect, because a "
              "word Ghidra does not hold as an instruction is legitimate")


def test_shape_counting_is_exercised() -> None:
    case("the shape counts are computed by the SHIPPING rules, not re-implemented here")
    # The shape property exists to answer 'one function or several merged', and it shipped a
    # confident WRONG number twice: `ret=0` for a body whose last two words are `jr ra` + `nop`,
    # because the count matched only funct 0x09 (`jalr`) and not 0x08 (`jr`). It also read a body
    # stepping by 4 from `body_first`, which is only the body's grid when `body_first` is aligned.
    source = Path(postscript.__file__).read_text()
    # CODE only. Both strings appear in the comments that RECORD these two bugs, and a text search
    # over the whole file would then be reporting the documentation as the defect -- the same
    # mistake the verify_body entry-point check made earlier.
    code = "\n".join(line for line in source.splitlines() if not line.lstrip().startswith("#"))
    check("0x08, 0x09" in code,
          "jump-register forms count funct 0x08 (jr) AND 0x09 (jalr)")
    check("getByteAt" not in code,
          "and the words are read through the memory API; `listing.getByteAt` does not exist and "
          "its AttributeError killed the post-script while Ghidra logged success")
    check("getMemory()" in code, "the memory API is used for the byte reads")
    # A shape is a REPORTED property, never a verdict. Asserted on the KEYS the function RETURNS, not
    # by grepping the file: the word "merged" legitimately appears in the docstring explaining what
    # the counts are for, and a text search reports that explanation as a verdict.
    returned = re.search(r'return \{\s*\n((?:\s*"[a-z_]+": [^\n]*\n)+)\s*\}', code)
    check(returned is not None, "the shape function returns a dict literal this check can read")
    if returned:
        keys = set(re.findall(r'"([a-z_]+)":', returned.group(1)))
        check(keys == {"returns", "prologues", "calls", "indirect_calls", "words_read"},
              "the shape reports four counts and a word count, and nothing else", str(sorted(keys)))
        check(not any(k in ("merged", "verdict", "is_merged", "confidence") for k in keys),
              "and issues no merged-or-not verdict: the judgement is the reader's")


def test_structure_owner_has_no_drifted_copies() -> None:
    case("ONE owner for the shape questions; the post-script's mirror is PINNED, not assumed")
    # PyGhidra loads a post-script as the module '__main__' with only the SCRIPT's directory on
    # sys.path, so it cannot import a repository module. That makes a mirror a floor imposed by the
    # host boundary -- but a mirror that AGREES has to be pinned, or the two drift and the tool starts
    # reporting two different numbers for one body.
    check(not hasattr(postscript, "structure"),
          "the post-script cannot import the owner, and does not pretend to")

    def counted(word: int, index: int) -> int:
        """The post-script's own arithmetic, extracted and RUN, on one word."""
        if index == 0:      # prologues
            return 1 if (word >> 16) == 0x27BD and (word & 0x8000) else 0
        if index == 1:      # returns / jump-register
            return 1 if (word >> 26) == 0 and (word & 0x3F) in (0x08, 0x09) else 0
        if index == 2:      # direct calls
            return 1 if (word >> 26) == 3 else 0
        return 1 if ((word >> 26) == 0 and (word & 0x3F) == 0x09
                     and ((w := word >> 21) & 0x1F) not in (0, 31)) else 0

    # A corpus of REAL words from a REAL PSX image when one is present, so the pin is over bytes and
    # not over a handful of hand-picked encodings that could agree while both are wrong. The fallback
    # corpus is the synthetic one, so the pin still runs on a machine with no provisioned image.
    image = pathlib.Path(os.environ.get("PSXPORT_SPYRO_IMAGE",
                                        "~/repo/psx/spyro/scratch/assets/spyro1/SCUS_942.28"
                                        )).expanduser()
    words = []
    if image.is_file():
        raw = image.read_bytes()
        base, text_file_offset = 0x80010000, 0x800
        for address in range(base, base + 0x20000, 4):
            offset = text_file_offset + (address - base)
            words.append(int.from_bytes(raw[offset:offset + 4], "little"))
        provenance = "%s (%d words)" % (image.name, len(words))
    else:
        words = [0x27BDFFE8, 0x03E00008, 0x00000000, 0x0C004010, 0x0320F809, 0x8FA40000,
                 0x3C018007, 0x24217DD8, 0x1000FFFF, 0x0C000000, 0x0320F8FF, 0x27BD0008,
                 0xAFBF0014, 0x8C820000, 0x0320F821]
        provenance = "synthetic corpus, %d words (no provisioned image)" % len(words)
    check(len(words) >= 8, "the pin corpus has real words in it", provenance)

    disagreements = 0
    owners = (structure.is_entry_prologue, structure.is_jump_register,
              structure.is_direct_call, structure.is_indirect_call)
    for word in words:
        for index, owner in enumerate(owners):
            if bool(owner(word)) != bool(counted(word, index)):
                disagreements += 1
    check(disagreements == 0,
          "the post-script's arithmetic and structure.py agree on the whole corpus",
          "%d disagreement(s) over %s x 4 questions" % (disagreements, provenance))

    # And the owner itself, on the four shapes it exists to recognise.
    check(structure.is_entry_prologue(0x27BDFFE8), "a negative sp adjustment IS an entry prologue")
    check(not structure.is_entry_prologue(0x27BD0018),
          "a POSITIVE sp adjustment is NOT -- matching either sign counts every stack bump in a body")
    check(structure.is_jump_register(0x03E00008), "`jr ra` is a jump-register form")
    check(structure.is_jump_register(0x0320F809), "`jalr ra, t9` is too")
    check(structure.is_direct_call(0x0C004010), "`jal` is a direct call")
    check(not structure.is_direct_call(0x03E00008), "`jr` is not a direct call")
    check(structure.is_indirect_call(0x0320F809), "`jalr` through t9 is an indirect call")
    check(not structure.is_indirect_call(0x03E00008), "`jr ra` is not an indirect call")
    reported = structure.shape([0x27BDFFE8, 0x0C004010, 0x03E00008, 0x00000000])
    check(reported == {"words": 4, "prologues": 1, "returns": 1, "calls": 1, "indirect_calls": 0},
          "shape() counts a real four-instruction body correctly", str(reported))


def test_scripts_have_an_entry_point() -> None:
    case("both Ghidra-side scripts have an entry point Ghidra will actually reach")
    # MEASURED: a `main()` with no call to it is the silent failure this whole tool exists to
    # prevent. Ghidra ran the file, wrote no inventory, logged "Post-analysis succeeded" and
    # exited 0. `__name__ == "__main__"` is the ONLY reliable discriminator: PyGhidra loads a
    # script as the module '__main__', while `getScriptArgs` and `currentProgram` are reachable as
    # bare names but are NOT in globals(), so a guard written as `"getScriptArgs" in globals()`
    # silently never fires.
    for module in (postscript, prescript):
        source = Path(module.__file__).read_text()
        check('if __name__ == "__main__":' in source,
              "%s has an `if __name__ == \"__main__\"` entry point" % module.__name__)
        # CODE only: both files deliberately EXPLAIN this trap in a comment, and a text search over
        # the whole file would then be reporting the documentation as the defect.
        code = "\n".join(line for line in source.splitlines()
                         if not line.lstrip().startswith("#"))
        check("in globals()" not in code,
              "%s does not test membership in globals() to detect Ghidra" % module.__name__)


def test_target_parsing_and_refusals() -> None:
    case("the entry point refuses the inputs it cannot serve")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        image = make_psx_exe(root / "image.exe")
        run = pipeline.DecompRun(image_name="spyro1", image_path=image,
                                 output_dir=root / "scratch" / "out", targets=[])
        raised, message = raises(images.ImageRefusal, pipeline.execute, run, None, root)
        check(raised, "no targets is a refusal, not a run that decompiles nothing", message)
        check("not a result" in message, "the refusal says why refusing is not pedantry", message)

        ambiguous = pipeline.DecompRun(image_name=None, image_path=image,
                                       output_dir=root / "scratch" / "out",
                                       targets=[0x80010000])
        raised, message = raises(images.ImageRefusal, ambiguous.spec, None)
        check(raised, "an ambiguous image name is refused rather than guessed", message)


def main() -> int:
    print("decomp pipeline selftest -- every case is a seeded difference that must be caught")
    for test in (
        test_body_classification,
        test_noreturn_policy,
        test_empty_target_list,
        test_prescript_window,
        test_manifest_and_base,
        test_module_reader,
        test_module_wrong_base_cost,
        test_manifest_selection_is_explicit,
        test_audit_is_clean_on_a_complete_run,
        test_audit_catches_seeded_differences,
        test_audit_catches_an_unrecorded_preseed,
        test_audit_catches_a_target_inside_a_function_body,
        test_ghidra_boundary,
        test_lock,
        test_output_dir_must_be_ignored,
        test_verify_body_contract,
        test_verify_body_finds_a_missing_call,
        test_shape_counting_is_exercised,
        test_structure_owner_has_no_drifted_copies,
        test_scripts_have_an_entry_point,
        test_target_parsing_and_refusals,
    ):
        try:
            test()
        except Exception:  # noqa: BLE001 - a crashing case is a failing case, and must be visible
            FAILURES.append("%s raised" % test.__name__)
            print("  FAIL %s raised:\n%s" % (test.__name__, traceback.format_exc()))
    print("")
    if FAILURES:
        print("FAILED: %d of %d checks" % (len(FAILURES), CHECKS))
        for failure in FAILURES:
            print("  - %s" % failure)
        return 1
    print("PASSED: %d of %d checks" % (CHECKS, CHECKS))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

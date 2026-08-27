---
id: 33
title: GetlocP acknowledges success without a Sub-Q position
status: fix-verified
symptom: Mega Man X4 repeatedly seeks to LBA 175155 because native CDC command 0x11 returns a successful all-zero response, so its XA position check concludes that the drive never advanced.
tags: cdc,getlocp,sub-q,chd,mmx4,xa
created: 2026-08-27
updated: 2026-08-27
---

Root cause boundary: the native controller implemented `GetlocP` as an acknowledgement-only command,
and the disc backend discarded the CHD track metadata needed to synthesize the eight Sub-Q position
bytes. The title's repeated seek was a valid reaction to the false successful position, not a title
timing defect.

### Resolution (2026-08-27)

`disc_open` now parses CHD metadata1/metadata2 into the physical track layout and cleans up a failed
open instead of leaving a poisoned partially-open state. `disc_get_subq_position` produces BCD
track, index, relative MM:SS:FF, and absolute MM:SS:FF values from that layout. Native CDC command
`0x11` returns those eight bytes, or reports `INT5` when no position is available; it can no longer
acknowledge success with zeros.

The Clang-built `test_cdc_getlocp` exercises both CHD metadata formats, virtual pregap accumulation,
multi-track selection, the real eight-byte command response, malformed metadata refusal, and the
failure-to-`INT5` path. The title-level falsifier is a bounded MMX4 run in which the former
175155/SeekL/ReadS/GetlocP restart disappears and XA continues advancing; that real-product evidence
is still pending integration of the next STR startup boundary.

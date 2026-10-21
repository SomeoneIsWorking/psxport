# 0170: disc open and hunk decode on the guest thread made load fields present late

Status: fixed. Owner: `runtime/psx/cd/disc.cpp` `Disc::residentHunk`, `runtime/psx/cd/hunk_reader.*` `HunkReader`,
`runtime/psx/host/title_session.cpp` (disc open at boot).

## Observed
Spyro 1 from boot at real speed (psx-spyro issue 0196, `scratch/judder-boot/boot1`, `presentseq`): fence 8 (first
logo field) presented 3.4-10.5 ms late in every run, and the logo and load fences 9 and 226-228 4-16 ms late.

## Cause
- `Disc::open` ran lazily on the first CD read, inside field 8: `chd_open` reads the image's hunk map (1.3 ms warm,
  6.7 ms cold).
- `psx::cd::Disc::residentHunk` decoded every CHD hunk synchronously on the guest thread into a one-hunk cache. An
  LZMA hunk costs about 0.45 ms, and one load stage reads 17-19 hunks, 9-16 ms inside a field.

## Fix
- `TitleSession` opens the bound disc when the title boots, before the first field.
- `HunkReader` owns the image's decoded hunks: 32 slots, four worker threads each with their own `chd_open`, and
  the 12 hunks after every requested one queued for decode. The caller decodes a miss itself with the image it
  opened and waits only for a hunk a worker is already decoding. `dischunk` logs hits, fills and the time spent
  waiting.

## Proof
Real-speed boots `boot3`..`boot6` (load average 14.8-20 on 16 cores): over the whole boot the caller decoded or
waited for 8 hunks (0.1% of requests), 2.8-3.9 ms in all, against about 100 ms before. Fences 9 and 226-229
present on their deadlines. The open takes 10.5 ms at boot, before the first present. Spyro 1 `final1` (load 5-10,
140 s route): fences 8, 9 and 226-229 within 0.33 ms of their deadlines; 180 of 13789 hunks decoded or waited on by
the caller, 58 ms in all. Under load 13-17 fence 8 is still 2.9-3.9 ms late from the first two cold hunks.

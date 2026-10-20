# 0165: B2 (CD, XA, audio, movie) changes shipped behaviour on purpose

Status: landed in B2 (`refactor(b2)`, `fix(b2)`); open items below

## Intentional changes
- A register-path Setmode that clears STRSND stops the XA stream (`CdController::setMode`); before, only the
  HLE CdControl path did.
- Mega Man X4's Setfilter reaches the drive; the XaStream copy of the filter is gone.
- An open-ended pull stream classifies sectors through `CdController::sectorRoute` (Mode2, submode
  `& 0x64 == 0x64`, then the filter), where it took any sector with the audio bit.
- A Setloc inside the lead-in (negative LBA) sets no position and logs an error.
- The register data FIFO honours Setmode 0x10 (2328 bytes from offset 24) as well as whole-sector framing.
- The XA ring drops its oldest frames once the backlog leaves less than `kXaMaxSectorFrames` (4032) free.
- Native movies respect `HostAudioOutput`'s claim, so an unclaimed session's movie is silent.
- `MdecDevice` is powered by `Fmv::begin`, not by `title_session`.
- PCM summaries are computed only with the `audiofield` channel on.
- `fmv_export` writes frames from the MDEC's 24-bit output rather than a 15-bit decode.
- `cd_ready_delivery` runs the ready callback under `Hle::InterruptScope`: `ra` is
  `Hle::kInterruptReturnSentinel` on the exception stack, then the handler buffer's registers when one is
  installed. Before, with a handler buffer, the callback kept the interrupted `ra`.
- A state save or load opens the configured memory card when the guest has not yet (Spyro 1 at its title
  refused the save with "the memory card is not open").
- REPL `xadump <channel> <lba> <path> [seconds]` plays channel `channel` of file 1 through a clip
  `XaStream` and writes the 44.1 kHz output; it skips other channels' EOFs, where it used to stop at one.

## Proof (hidden window, `PSXPORT_VK_HEADLESS=1` read from the process environment)
- Spyro 1 title with its XA music streaming (`[xa] start @ LBA 113448`): state save, 120 fields, load;
  the title and music continue (`psx-spyro/scratch/b2fix/run/spyro1-rt2*`).
- Spyro 2 and 3 title round trips, and Spyro 3 CdReads after the load delivering their callbacks
  (`spyro2-rt1`, `spyro3-rt1`).
- Tomba! 2 boot movies `LOGO.STR` (255 frames, 8.53 s of XA) and `OP.STR` (2026 frames, 67.57 s) decode
  and play to their ends at 30 fps against their audio (`psx-tomba/scratch/b2fix/movie-full*`).

## Open
- A save taken while Spyro 2 or 3's frame driver has its main-loop call in flight resumes the guest at the
  wrong PC: psx-spyro issue 0203. That, not the CD read path, is the Spyro 3 load failure and the Spyro 2
  Lightrec fault B2 reported.
- Spyro 2 and 3 have no MDEC movies (their discs carry only `SPEECH.STR`, all XA audio sectors), so the
  plan's "Spyro 2 and 3 boot movies" proof has nothing to run; Tomba! 2's boot movies stand in.
- A movie's Start skip is not reachable from the control channel: issue 0166.

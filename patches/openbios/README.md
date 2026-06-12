# OpenBIOS (psxport fastboot build)

`bios/openbios-fast.bin` is OpenBIOS (PCSX-Redux, MIT) built with `FASTBOOT=1`:
no shell — the BIOS bootstraps the disc executable directly. Game EXEC at
emulated frame ~50 (vs ~700 for the stock shell, ~4000 for retail-style boot).
Rebuild with `scripts/build-openbios.sh` (needs the mips64-linux-gnu cross gcc;
its mips1/-mabi=32/-EL output is R3000-compatible).

`0001-psxport-boot-logs.patch` adds psxprintf breadcrumbs around the
startShell/initCDRom boot path (visible via the runtime's TTY capture).

Use in the runtime: `wide60rt <chd> -bios bios/` after copying/symlinking
`openbios-fast.bin` to `scph5501.bin`, or point -bios at a dir containing it
under that name. (Embedding it in the binary is planned.)

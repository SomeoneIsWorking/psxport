# RAM disassembly

From the framework checkout:

```sh
uv run --frozen python tools/disasm.py scratch/oracle-console/ram.bin 80012340 80012380
uv run --frozen python tests/test_disasm.py
```

Capstone 5 is a required, locked maintainer dependency in `pyproject.toml` and `uv.lock`.
The same Python interpreter runs the `ram_disassembler_cli` test in the normal CMake suite.

The dump must contain exactly 2 MiB. Start and exclusive end addresses are hexadecimal, word aligned,
nonempty and contained within one physical, KSEG0 or KSEG1 mapping of that RAM. Ranges cannot wrap,
cross mappings, or silently truncate at a file boundary. The displayed address preserves the chosen
mapping.

Every requested 4-byte word is decoded separately. A successful invocation prints a scanned/decoded
denominator and exits zero. Each unsupported word prints its exact address and raw bits as `UNKNOWN`;
the tool continues visiting the remaining words, reports incomplete coverage, and exits nonzero.
Missing dumps, wrong sizes and invalid ranges refuse with zero scanned words.

Capstone's MIPS32 text is a diagnostic, not validation that an encoding belongs to the PSX ISA.
Some legitimate PSX COP2/GTE instructions are unsupported, including the synthetic CFC2 and GTE cases
in the CLI regression. Their unknown results must not be used as proof that code is absent or invalid.
Use independently corroborated binary evidence for those instructions; this tool does not substitute
a second decoder or invent a mnemonic to make its coverage appear complete.

# ABI extraction tool scope

`tools/abi_extract.py` parses the current generated-C corpus to recover stack-frame and call-boundary
facts. It remains present only while existing consumers still contain that corpus. It is not part of
the native/Lightrec product architecture, must not be used to generate a new port, and is removed with
the offline translator under `docs/issues/0051-*`.

Still-useful ABI facts must be confirmed against the original binary or runtime evidence and recorded
in the owning title repository. Generated C is not the ground truth for new executor work. Lightrec
calls use the production state bridge and image-scoped runtime dispatch described in
`docs/faithful-execution.md`; they do not reproduce host stack frames inferred from emitted functions.

Until deletion, the tool's output is diagnostic only:

- it may help locate existing consumer assumptions that must be migrated;
- it cannot establish instruction, delay-slot, exception, interrupt, or cycle correctness;
- it cannot authorize a native override without independent binary/runtime evidence; and
- its parser tests remain relevant only to safe removal of the current generated-call dependencies.

Do not extend the parser for a new corpus shape. A needed PSX ABI/runtime contract belongs in the
Lightrec state bridge, bounded executor exit, or native-dispatch owner.

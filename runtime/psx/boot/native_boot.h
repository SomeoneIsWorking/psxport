#pragma once

class Core;

// native_boot_run — the framework's own boot spine: the declared crt0, the boot movies, the guest
// boot prologue, and the product frame loop.
//
// It is DECLARED here because four consuming repositories hand-declared this exact prototype at
// their call site, each in a comment admitting that no framework header declared it. A hand-declared
// prototype is a contract nothing maintains: it survives a signature change until a build breaks, and
// it is invisible to anyone reading the framework.
//
// A title that needs to choose its own composition — two-stage boot, a picker session, a field step
// the framework does not know how to host — uses `psx::Machine` (machine.h) for the parts that are
// the same everywhere and calls this only when the whole spine, boot movies and all, is what it wants.
void native_boot_run(Core *c);
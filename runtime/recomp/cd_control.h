#pragma once

class Core;

// Apply one controller command through the native synchronous CD model and
// report blocking-control success. Game-specific libds wrappers may call this
// after validating which command/result contract they own; it is not a blanket
// command acceptor.
void cd_control_sync(Core *c);

// Complete stock Sony libcd data reads through the existing synchronous native disc owner. These
// are narrow binding targets for direct runtimes that measured their CdRead/CdReadSync entries;
// callers supply the original guest ABI in a0/a1/a2 and receive the original result in v0.
void cd_read_stock_sync(Core *c);
void cd_readsync_stock_sync(Core *c);

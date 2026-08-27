#pragma once

#include <cstdint>

class Core;

// Resolve the measured guest RAM slot holding the current stock CD-ready callback. Direct runtimes
// publish a typed layout; legacy runtimes retain GameConfig::cdReadyCbPtr. Zero means neither owner
// has declared the fact.
std::uint32_t cd_ready_callback_pointer(const Core &core);

// Whether the runtime has declared that the framework owns stock Sony CdRead synchronously. This
// is the typed finite-vs-continuous ReadN discriminator for both direct and adapter consumers.
bool cd_native_stock_read_owned(const Core &core);

// Apply one controller command through the native synchronous CD model and
// report blocking-control success. Game-specific libds wrappers may call this
// after validating which command/result contract they own; it is not a blanket
// command acceptor.
void cd_control_sync(Core *c);

// Complete stock Sony libcd CdSync(noblock, result) through the synchronous native disc owner.
// Direct runtimes bind their measured wrapper/body to this owner so the guest's VSync timeout loop
// is never entered.
void cd_sync_stock_sync(Core *c);

// Complete stock Sony libcd data reads through the existing synchronous native disc owner. These
// are narrow binding targets for direct runtimes that measured their CdRead/CdReadSync entries;
// callers supply the original guest ABI in a0/a1/a2 and receive the original result in v0.
void cd_read_stock_sync(Core *c);
void cd_readsync_stock_sync(Core *c);

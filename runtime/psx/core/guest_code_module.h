// guest_code_module.h — a title's runtime-loaded code, as an executable residency.
//
// WHAT THIS IS. Some PlayStation titles do not keep all their code in the executable. They load a
// relocatable module from the disc into main RAM with their own loader, relocate it, and call its
// entry — C-12: Final Resistance reads `RELOCS/GT.LVB` into its heap and enters at blob+0xA8 after
// its own relocator runs. The framework's dispatcher admits a guest call only when an ACTIVE IMAGE
// owns the address (`resolveHostDispatch`), so without this the title's own module call is refused
// with `ambiguous code-image identity` while the identical call works for resident text.
//
// WHY THE TITLE DECLARES THE WINDOW. "Guest RAM can hold code" is not a fact the framework can
// recover from hardware: what is executable depends entirely on what the title's loader put there.
// The runtime declares the arena it loads modules into (`GameRuntime::guestCodeModuleWindow`), and
// the framework owns the mechanics: activate the residency when a CD sector actually lands inside
// it, and let the ordinary executable-write invalidation keep translations honest while the rest of
// the module is still arriving.
//
// THE STATED LIMIT. The identity is established by the FIRST landing and then covers the window.
// A later load of different bytes into the same window keeps that identity while the ordinary
// executable-write path invalidates every translation over the new bytes, so execution is correct;
// what is NOT yet meaningful is keying a native override on the module's content identity, which
// needs the load's boundaries measured. That is recorded, not guessed around.
//
// PER-READ PUBLICATION. A loader that goes through the stock `CdRead` (Spyro 2/3, Spider-Man 1) calls
// `publishStockReadLanding` from `GameRuntime::stockCdReadLanded`: each whole read becomes an image
// named by the SHA-256 of its bytes, with a new generation per read, so a replaced overlay stops
// resolving. A title may restrict it to the arena its loader allocates from.
#pragma once

#include "cd_stock_read_completion.h"
#include "guest_program_image.h"
#include "image_identity.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

class Core;

class GameRuntime;

namespace psx::code_module {

// The authenticated identity of one complete byte range: the SHA-256 that names it and the 64-bit
// prefix of that digest the image catalog carries as its content identity.
struct Digest {
  std::string hex;
  std::uint64_t identity = 0;
};

[[nodiscard]] Digest digest(std::span<const std::uint8_t> bytes);

// Call AFTER the bytes are written through the canonical memory writer: that write's invalidation
// subtracts its bytes from any image already covering them. Every call mints a new generation, even
// for identical bytes, so a reload at the same address is a replacement.
psx::cpu::ImageIdentity activate(Core &core, std::string_view kind, GuestAddressRange physical, const Digest &content);

// Publish one finished stock `CdRead` as an image, after its bytes are written. A landing that is
// not wholly inside a valid `arena` is not code the title's loader placed and publishes nothing; an
// empty `arena` admits all of main RAM. A landing that cannot be published (empty or outside RAM) requests a runtime
// fault rather than leaving guest code to run under no identity.
void publishStockReadLanding(Core &core, const psx::cd::StockReadLanding &landing, GuestAddressRange arena = {});

} // namespace psx::code_module

// The physical window this title loads runtime code modules into, or an empty range when it has
// none. One window, because one measured arena is what a title has measured; a runtime that needs
// several disjoint arenas grows this into a list rather than approximating one with a span that
// covers unrelated RAM.
[[nodiscard]] GuestAddressRange guestCodeModuleWindow(const Core &core);

// A CD sector transfer landed `landed` bytes of guest RAM. If that range is inside the declared
// window and the window has no active residency, the window becomes one, so the title's own
// module entry is dispatchable. Idempotent: repeated sectors of the same load activate once.
void publishGuestCodeModuleLanding(Core &core, GuestAddressRange landed);
# 0150: guest stores bind emission keys only on the Lightrec slow path

Status: fixed

## Observed
Toy Story 2 opened a producer scope around its object renderer and let guest drawers run inside it.
Packets those guest drawers wrote were bound to the scope's key only when Lightrec routed the store
through `Core::writeGuestMemory`; stores Lightrec writes directly to RAM never reach `EmissionScope`.
The result was 13,047 duplicate `(renderer, actor, 0)` keys on the Andy's House route (toystory2
issue 0040). Whether a guest-written packet is keyed therefore depends on Lightrec's store path.

## Contract today
`EmissionScope::noteStore` is called from `Core::writeGuestMemory` only. Native producers write
through `mem_w*` and always bind; guest code inside a scope binds some words and not others.

## Proper fix
Decide one rule and enforce it: either guest stores never bind (a scope covers native writes only,
and a title that keeps a guest body names its packets natively), or every guest store inside a
scope binds (Lightrec must report stores while a scope is open). Until then producers must not rely
on guest code inside their scope being keyed.

## Resolution
Every guest store inside a scope binds. Lightrec's optimizer flagged a store whose address constant
propagation could resolve as a direct RAM access even though the RAM map carries callbacks, so those
stores skipped `Core::mem_w*`. The maintained fork now routes a known-address store through a map's
callbacks whenever the map has them (`optimizer.c:lightrec_flag_io`); known-address loads stay direct.
`tests/test_guest_store_binding.cpp` binds a store through a constant base and one through a register.

#pragma once

#include <cstdint>

class Core;

namespace psx::cpu {

using HostTurnFunction = void (*)(Core *core);

// The host field clock runs on EMULATED guest time. A display field is owed once one field period
// of Timing::emulatedCpuTicks has elapsed since the last delivered field, whether that time was
// spent executing guest instructions or waiting out fields the native frame loop delivered. Host
// speed never decides how many fields a long guest update observes.
void registerHostTurn(Core &core, HostTurnFunction function, unsigned fieldRateMillihertz);
// Raise the pending host turn when the registered core's guest clock has reached its field deadline.
// Called from the executor's instruction accounting; the turn itself is taken at the next eligible
// dispatch boundary by serviceHostTurn.
void requestHostTurnWhenDue(Core &core);
// Guest ticks until the registered core's next field is owed: 0 when it is already due, and no
// bound at all for a core without a host clock. The executor ends a translated segment there so a
// guest loop that waits on the field's work cannot outrun the clock inside one segment.
std::uint64_t hostTurnTicksUntilDue(const Core &core);
// A display field was delivered on this core: the next field is owed one period from now and any
// latched request for this same field is cancelled. Timing::advanceDisplayFields is the caller;
// titles advance display fields through it and never acknowledge fields themselves.
void notifyDisplayField(Core &core);
void serviceHostTurn(Core &core);
void shutdownHostTurn();

} // namespace psx::cpu

#pragma once

class Core;

namespace psx::cpu {

using HostTurnFunction = void (*)(Core *core);

void registerHostTurn(Core &core, HostTurnFunction function, unsigned fieldRateMillihertz);
void notifyDisplayField(Core &core);
void serviceHostTurn(Core &core);
void shutdownHostTurn();

} // namespace psx::cpu

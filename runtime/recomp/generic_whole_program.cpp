#include "generic_whole_program.h"

#include "core.h"
#include "game.h"
#include "recomp_iface.h"

#include <cstdlib>

#include <lucent/log.h>

GenericWholeProgramFrameDriver::GenericWholeProgramFrameDriver(Game &game, GenericWholeProgramProfile profile)
    : game_(&game) {
  (void)profile;
  const RecompRegistry *const rec = psxport_recomp();
  if (!rec || !rec->wholeProgram || !rec->wholeProgram->entryAddress) {
    lucent::error("generic-loop",
                  "generic whole-program execution requires emitted RecWholeProgramMetadata::entryAddress");
    std::abort();
  }
  entryAddress_ = rec->wholeProgram->entryAddress;
}

void GenericWholeProgramFrameDriver::stepFrame(Core &core, uint32_t frame) {
  if (game_ != core.game) {
    lucent::error("generic-loop", "generic whole-program driver received an unbound Core");
    std::abort();
  }
  if (!psxport_recomp() || !psxport_recomp()->main_dispatch) {
    lucent::error("generic-loop", "generic whole-program execution requires a generated main dispatcher");
    std::abort();
  }

  core.game->timing.logicFrame = frame;
  core.game->pad.serviceFrame();
  waitingAtVSync_ = false;
  if (!started_) {
    started_ = true;
    guest_.start([this] {
      rec_dispatch(&game_->core, entryAddress_);
    });
  }
  guest_.resume();
  if (!waitingAtVSync_) {
    lucent::error("generic-loop",
                  "generated program returned before its next VSync; the declared entry 0x{:08X} is "
                  "not a whole-program loop",
                  entryAddress_);
    std::abort();
  }

  // The guest has produced one complete interval ending at its real VSync. Presentation owns the
  // actual display field; only after that field exists can the suspended VSync return on the next
  // host turn. The neutral presenter is intentional: a bare port has no title temporal policy.
  core.game->presentation.commit(&core, 1, nullptr);
  core.game->timing.frameTick();
  if (core.game->diff_mode) {
    core.game->spu_audio.frameLogic();
  } else {
    core.game->spu_audio.frame();
  }
}

void GenericWholeProgramFrameDriver::yieldAtVSync(Core &core) {
  if (&core != &game_->core || !started_) {
    lucent::error("generic-loop", "generic VSync arrived outside its active generated program");
    std::abort();
  }
  waitingAtVSync_ = true;
  guest_.yield();
  // Return the counter from the field the host just delivered while this exact generated VSync call
  // was suspended. No guest state or return PC is reconstructed; the coroutine retained both.
  core.r[2] = core.game->timing.vblank;
}

void generic_whole_program_vsync(Core *core) {
  if (!core || !core->game || !core->game->frameDriver) {
    lucent::error("generic-loop", "generic VSync has no active product frame driver");
    std::abort();
  }
  auto *driver = dynamic_cast<GenericWholeProgramFrameDriver *>(core->game->frameDriver.get());
  if (!driver) {
    lucent::error("generic-loop", "generic VSync was selected for a non-generic frame driver");
    std::abort();
  }
  driver->yieldAtVSync(*core);
}

bool is_generic_whole_program_driver(const FrameDriver &driver) {
  return dynamic_cast<const GenericWholeProgramFrameDriver *>(&driver) != nullptr;
}

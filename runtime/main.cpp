// psxport runtime: minimal headless/libretro host statically linked against
// the Beetle PSX core (vendor/beetle-psx, GPL-2). This is the base of the
// interpreter+overrides PC port: we own the run/present loop, and core
// internals are hooked by patching the vendored sources directly.
//
// Usage: wide60rt <disc.chd> [-frames N] [-dumpdir DIR] [-dumpinterval N]
//                 [-inputscript FILE] [-bios DIR] [-play]
// -play opens an SDL window with keyboard/gamepad input and audio:
//   arrows = d-pad, Z/X/A/S = Cross/Circle/Square/Triangle,
//   Enter/RShift = Start/Select, Q/W E/R = L1/L2 R1/R2,
//   Tab = hold to fast-forward, Esc quits.
// Instant CD (data reads/seeks at PC speed, RE'd into the imported cdc.c)
// is enabled by default; -slowcd reverts to native CD timing. -fastboot
// (Beetle skip_bios) requires a retail BIOS: it intercepts the retail shell
// and hangs the boot under OpenBIOS, so it is opt-in.
// Input script lines: "<start_frame> <end_frame> <Button>" (digital pad).

#include "libretro.h"
#include "psxport_hooks.h"
#include "hle_bios.h"
#include "wide60.h"
#include "games/tomba2.h"

#include <SDL2/SDL.h>

#include <csignal>
#include <execinfo.h>
#include <unistd.h>

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string g_system_dir = ".";
std::string g_dump_dir;
unsigned g_dump_interval = 0;
unsigned g_frame = 0;
// RE aid: PSXPORT_FRAMEDUMP="frame:path.ppm;frame:path.ppm" — capture the exact
// framebuffer at specific frames (works headless; honors PSXPORT_WIDE/_INTERNAL_RES
// so the same moment can be captured under different display configs for analysis).
std::vector<std::pair<unsigned, std::string>> g_frame_dumps;

// Last presented framebuffer, cached so the REPL `shot` command can dump the
// current screen on demand while driving interactively (libretro reuses its own
// buffer, so we copy). Tightly packed (pitch == width*bpp), pixel format in
// g_pixel_format.
std::vector<uint8_t> g_last_fb;
unsigned g_last_fb_w = 0, g_last_fb_h = 0;

retro_pixel_format g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

// --- play mode (SDL window/input/audio) ---
bool g_play = false;
bool g_quit = false;
bool g_fast_boot = false; // skip_bios hangs with OpenBIOS (no retail shell)
bool g_fast_cd = true;
bool g_ff_hold = false;      // Tab held
bool g_repl = false;
bool g_present_this_run = true;
SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
unsigned g_tex_w = 0, g_tex_h = 0;
SDL_AudioDeviceID g_audio = 0;
SDL_GameController* g_pad = nullptr;
int16_t g_buttons = 0; // bitmask by RETRO_DEVICE_ID_JOYPAD_*

// per-game module (only Tomba 2 for now), driven per frame with RAM access
uint8_t* g_ram = nullptr;
bool g_tomba2 = false;
uint16_t g_inject_buttons = 0; // game-module scoped injections
uint16_t g_repl_buttons = 0;   // REPL-held pad bits (press/release), for driving in
bool g_module_turbo = false;   // game module requests fast-forward (Start-held intro skip)
bool g_wide60 = false;         // reprojecting 60fps renderer (PSXPORT_WIDE60)

// Display enhancements fed to the Beetle core as libretro options. The software
// renderer honors internal_resolution (libretro.c: upscale_shift applies when no
// hw renderer), and widescreen_hack scales GTE geometry (CPU-side), so both work
// without a GL/Vulkan context.
const char* g_internal_res = "4x"; // beetle_psx_internal_resolution; PSXPORT_INTERNAL_RES
bool g_widescreen = true;          // beetle_psx_widescreen_hack; PSXPORT_NOWIDE disables
const char* g_ws_aspect = "16:9";  // beetle_psx_widescreen_hack_aspect_ratio
float g_aspect = 4.0f / 3.0f;      // core-reported display aspect (updated from av_info)

// --- runtime logging -----------------------------------------------------
// Frame-stamped events to stderr. The emulated TTY (BIOS A0:3C / B0:3D
// putchar, used by OpenBIOS for its entire boot narrative and by many games
// for debug prints) is captured via PC hooks on the kernel dispatchers.
void RtLog(const char* tag, const char* msg)
{
  fprintf(stderr, "[%6u] %-4s %s\n", g_frame, tag, msg);
}

char g_tty_line[256];
unsigned g_tty_len = 0;

int BiosPutcharHook(uint32_t pc, uint32_t* gpr, uint32_t*)
{
  // A0 dispatcher: function id in t1 (reg 9); putchar arg in a0 (reg 4)
  const uint32_t fn = gpr[9];
  const bool is_putchar = (pc == 0xA0 && (fn == 0x3C || fn == 0x3E)) || (pc == 0xB0 && (fn == 0x3D || fn == 0x3F));
  if (is_putchar)
  {
    const char ch = static_cast<char>(gpr[4]);
    if (ch == '\n' || g_tty_len >= sizeof(g_tty_line) - 1)
    {
      g_tty_line[g_tty_len] = 0;
      if (g_tty_len)
        RtLog("tty", g_tty_line);
      g_tty_len = 0;
    }
    else if (ch >= 0x20)
    {
      g_tty_line[g_tty_len++] = ch;
    }
  }
  return PSXPORT_HOOK_CONTINUE;
}

// ---- HLE BIOS (pure native BIOS, no MIPS ROM) -------------------------------
// When enabled (PSXPORT_HLE_BIOS=1), the emulated CPU runs the game's EXE
// directly and every A0/B0/C0 syscall is serviced natively here instead of by
// BIOS-ROM code. There is NO dispatcher code at 0x000000A0/B0/C0 in this mode,
// so the trap MUST consume the call (set $v0, return to $ra) — falling through
// would execute whatever happens to sit at the vector. Bring-up flag: default
// OFF until the kernel (events/exceptions) is complete.
bool g_hle_bios = false;
int HleSyscallHook(uint32_t pc, uint32_t* gpr, uint32_t* redirect_pc)
{
  BiosPutcharHook(pc, gpr, redirect_pc); // keep TTY capture (A0:3C/3E, B0:3D/3F)
  const char table = (pc == 0xA0) ? 'A' : (pc == 0xB0) ? 'B' : 'C';
  const uint32_t fnum = gpr[9]; // $t1 selects the function
  // putchar variants: TTY already captured above; return the char and exit.
  const bool is_putchar = (pc == 0xA0 && (fnum == 0x3C || fnum == 0x3E)) ||
                          (pc == 0xB0 && (fnum == 0x3D || fnum == 0x3F));
  if (!is_putchar && psxport_hle_syscall(table, fnum, gpr, g_ram))
  {
    *redirect_pc = gpr[31];
    return PSXPORT_HOOK_REDIRECT;
  }
  if (!is_putchar)
  {
    // Surface unimplemented syscalls (deduped on the immediate repeat, like the
    // bios tracer) so bring-up shows exactly what each game still needs.
    static uint32_t last_key = 0xFFFFFFFFu;
    const uint32_t key = (uint32_t(uint8_t(table)) << 8) | (fnum & 0xFF);
    if (key != last_key)
    {
      fprintf(stderr, "[hle] UNIMPL %c0(%02X) a0=%08X a1=%08X a2=%08X ra=%08X\n",
              table, fnum, gpr[4], gpr[5], gpr[6], gpr[31]);
      last_key = key;
    }
    gpr[2] = 0;
  }
  *redirect_pc = gpr[31];
  return PSXPORT_HOOK_REDIRECT;
}

// HLE BIOS: native override of OpenBIOS cdromBlockReading(count, sector, buffer)
// at 0xBFC03A9C (sig 0x24A30096 = `addiu v1,a1,0x96`, i.e. sector += 150). The
// real routine reads `count` 2048-byte sectors from filesystem LBA `sector` into
// `buffer` by driving the CDC one sector at a time (Setloc->SeekL->ReadN->wait),
// blocking on TestEvent spins gated by per-sector disc pacing -- this is the
// dominant cost of Tomba2's intro/FMV loads (the CPU sits 100% in this BIOS code,
// 0% in the game). We service it natively from the CD image at host speed and
// return, removing the PSX-side cadence entirely. The events it waits on are
// internal to this routine (the caller never sees them), so returning the sector
// count is a complete emulation. On any read failure we fall through to the real
// BIOS path. Disable with PSXPORT_BIOS_HLE=0 for RE/oracle runs.
bool g_bios_hle = true;
int BiosHleCdBlockRead(uint32_t /*pc*/, uint32_t* gpr, uint32_t* redirect_pc)
{
  if (!g_bios_hle || !g_ram)
    return PSXPORT_HOOK_CONTINUE;
  const int32_t count  = static_cast<int32_t>(gpr[4]); // a0
  const int32_t sector = static_cast<int32_t>(gpr[5]); // a1 (filesystem LBA)
  const uint32_t buf   = gpr[6] & 0x1FFFFF;             // a2 -> main RAM offset
  if (count <= 0 || buf + static_cast<uint32_t>(count) * 2048u > 0x200000u)
    return PSXPORT_HOOK_CONTINUE; // out-of-range: let the real BIOS handle it
  if (psxport_cd_read_sectors(sector, count, g_ram + buf) != count)
    return PSXPORT_HOOK_CONTINUE; // read failed: fall back to the real path
  if (psxport_bios_log)
    fprintf(stderr, "[hle f%u] cdromBlockReading lba=%d count=%d -> buf=%08X (%d KiB)\n",
            psxport_frame, sector, count, gpr[6], count * 2);
  gpr[2] = static_cast<uint32_t>(count); // v0 = sectors read
  *redirect_pc = gpr[31];                // return to caller (ra)
  return PSXPORT_HOOK_REDIRECT;
}

struct InputEvent
{
  unsigned start, end;
  unsigned button;
};
std::vector<InputEvent> g_input_script;

const struct
{
  const char* name;
  unsigned id;
} kButtons[] = {
  {"Up", RETRO_DEVICE_ID_JOYPAD_UP},         {"Down", RETRO_DEVICE_ID_JOYPAD_DOWN},
  {"Left", RETRO_DEVICE_ID_JOYPAD_LEFT},     {"Right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
  {"Start", RETRO_DEVICE_ID_JOYPAD_START},   {"Select", RETRO_DEVICE_ID_JOYPAD_SELECT},
  {"Cross", RETRO_DEVICE_ID_JOYPAD_B},       {"Circle", RETRO_DEVICE_ID_JOYPAD_A},
  {"Square", RETRO_DEVICE_ID_JOYPAD_Y},      {"Triangle", RETRO_DEVICE_ID_JOYPAD_X},
  {"L1", RETRO_DEVICE_ID_JOYPAD_L},          {"R1", RETRO_DEVICE_ID_JOYPAD_R},
  {"L2", RETRO_DEVICE_ID_JOYPAD_L2},         {"R2", RETRO_DEVICE_ID_JOYPAD_R2},
};

bool LoadInputScript(const char* path)
{
  FILE* fp = fopen(path, "r");
  if (!fp)
    return false;
  char name[32];
  unsigned s, e;
  while (fscanf(fp, "%u %u %31s", &s, &e, name) == 3)
  {
    for (const auto& b : kButtons)
    {
      if (strcmp(name, b.name) == 0)
      {
        g_input_script.push_back({s, e, b.id});
        break;
      }
    }
  }
  fclose(fp);
  return true;
}

// RETRO_DEVICE_ID_JOYPAD_* bit for a button name, or -1.
int ButtonBit(const char* name)
{
  for (const auto& b : kButtons)
  {
    if (strcmp(name, b.name) == 0)
      return static_cast<int>(b.id);
  }
  return -1;
}

// Savestates via the libretro serialize API (full machine state incl. VRAM).
// Used to checkpoint a hand-driven gameplay position so RE / interpolation
// work doesn't re-drive the boot+intro each session.
bool SaveState(const char* path)
{
  const size_t sz = retro_serialize_size();
  std::vector<uint8_t> buf(sz);
  if (!retro_serialize(buf.data(), sz))
    return false;
  FILE* fp = fopen(path, "wb");
  if (!fp)
    return false;
  const bool ok = fwrite(buf.data(), 1, sz, fp) == sz;
  fclose(fp);
  return ok;
}

bool LoadState(const char* path)
{
  FILE* fp = fopen(path, "rb");
  if (!fp)
    return false;
  fseek(fp, 0, SEEK_END);
  const long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> buf(n > 0 ? n : 0);
  const bool read_ok = n > 0 && fread(buf.data(), 1, n, fp) == static_cast<size_t>(n);
  fclose(fp);
  return read_ok && retro_unserialize(buf.data(), n);
}

void LogCb(retro_log_level level, const char* fmt, ...)
{
  if (level < RETRO_LOG_WARN)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

bool EnvironmentCb(unsigned cmd, void* data)
{
  switch (cmd)
  {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *static_cast<const char**>(data) = g_system_dir.c_str();
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      g_pixel_format = *static_cast<const retro_pixel_format*>(data);
      return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      static_cast<retro_log_callback*>(data)->log = LogCb;
      return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *static_cast<bool*>(data) = true;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
      auto* var = static_cast<retro_variable*>(data);
      if (g_fast_boot && strcmp(var->key, "beetle_psx_skip_bios") == 0)
        return (var->value = "enabled"), true;
      // whole-disc RAM precache: removes the threaded CD reader, whose
      // read-ahead cond-wait wedges under instant-CD request rates
      if (g_fast_cd && strcmp(var->key, "beetle_psx_cd_access_method") == 0)
        return (var->value = "precache"), true;
      if (g_internal_res && strcmp(var->key, "beetle_psx_internal_resolution") == 0)
        return (var->value = g_internal_res), true;
      if (strcmp(var->key, "beetle_psx_widescreen_hack") == 0)
        return (var->value = g_widescreen ? "enabled" : "disabled"), true;
      if (g_widescreen && strcmp(var->key, "beetle_psx_widescreen_hack_aspect_ratio") == 0)
        return (var->value = g_ws_aspect), true;
      return false; // everything else at core defaults
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
      const auto* g = static_cast<const retro_game_geometry*>(data);
      if (g && g->aspect_ratio > 0.0f)
        g_aspect = g->aspect_ratio;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
      const auto* av = static_cast<const retro_system_av_info*>(data);
      if (av && av->geometry.aspect_ratio > 0.0f)
        g_aspect = av->geometry.aspect_ratio;
      return true;
    }
    default:
      return false;
  }
}

void WriteFramePPMPath(const void* data, unsigned width, unsigned height, size_t pitch,
                       const char* path)
{
  FILE* fp = fopen(path, "wb");
  if (!fp)
    return;
  fprintf(fp, "P6\n%u %u\n255\n", width, height);
  const uint8_t* row = static_cast<const uint8_t*>(data);
  for (unsigned y = 0; y < height; y++, row += pitch)
  {
    for (unsigned x = 0; x < width; x++)
    {
      uint8_t rgb[3];
      if (g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888)
      {
        const uint32_t px = reinterpret_cast<const uint32_t*>(row)[x];
        rgb[0] = (px >> 16) & 0xFF;
        rgb[1] = (px >> 8) & 0xFF;
        rgb[2] = px & 0xFF;
      }
      else // 0RGB1555 / RGB565 close enough for diagnostics
      {
        const uint16_t px = reinterpret_cast<const uint16_t*>(row)[x];
        rgb[0] = ((px >> 10) & 0x1F) << 3;
        rgb[1] = ((px >> 5) & 0x1F) << 3;
        rgb[2] = (px & 0x1F) << 3;
      }
      fwrite(rgb, 1, 3, fp);
    }
  }
  fclose(fp);
}

void WriteFramePPM(const void* data, unsigned width, unsigned height, size_t pitch)
{
  char path[1024];
  snprintf(path, sizeof(path), "%s/frame_%06u.ppm", g_dump_dir.c_str(), g_frame);
  WriteFramePPMPath(data, width, height, pitch, path);
}

void VideoCb(const void* data, unsigned width, unsigned height, size_t pitch)
{
  if (data)
  {
    // Cache a tightly-packed copy of the current frame for the REPL `shot`
    // command (pitch may exceed width*bpp; pack rows so shot can use width*bpp).
    const unsigned bpp = (g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
    g_last_fb.resize(static_cast<size_t>(width) * height * bpp);
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (unsigned y = 0; y < height; y++)
      memcpy(g_last_fb.data() + static_cast<size_t>(y) * width * bpp, src + y * pitch, static_cast<size_t>(width) * bpp);
    g_last_fb_w = width;
    g_last_fb_h = height;
  }
  if (data && !g_dump_dir.empty() && g_dump_interval && (g_frame % g_dump_interval) == 0)
    WriteFramePPM(data, width, height, pitch);
  if (data)
    for (const auto& fd : g_frame_dumps)
      if (fd.first == g_frame)
        WriteFramePPMPath(data, width, height, pitch, fd.second.c_str());

  if (!g_play || !data || !g_present_this_run)
    return;
  if (!g_texture || g_tex_w != width || g_tex_h != height)
  {
    if (g_texture)
      SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer,
                                  g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? SDL_PIXELFORMAT_ARGB8888 :
                                                                                  SDL_PIXELFORMAT_ARGB1555,
                                  SDL_TEXTUREACCESS_STREAMING, width, height);
    g_tex_w = width;
    g_tex_h = height;
  }
  SDL_UpdateTexture(g_texture, nullptr, data, static_cast<int>(pitch));
  SDL_RenderClear(g_renderer);
  // Scale the framebuffer to the current window/screen size, preserving the
  // core-reported display aspect (4:3 native, or the widescreen-hack aspect when
  // enabled). Letterbox/pillarbox the remainder. Recomputed each present, so it
  // adapts live to window resizes, fullscreen, and aspect changes.
  int ow = 0, oh = 0;
  SDL_GetRendererOutputSize(g_renderer, &ow, &oh);
  int tw = ow, th = (int)(ow / g_aspect);
  if (th > oh)
  {
    th = oh;
    tw = (int)(oh * g_aspect);
  }
  SDL_Rect dst = {(ow - tw) / 2, (oh - th) / 2, tw, th};
  SDL_RenderCopy(g_renderer, g_texture, nullptr, &dst);
  SDL_RenderPresent(g_renderer);
}

void AudioSampleCb(int16_t, int16_t) {}
size_t AudioBatchCb(const int16_t* data, size_t frames)
{
  if (g_audio && g_present_this_run)
  {
    // keep latency bounded: drop if more than ~100ms is already queued
    if (SDL_GetQueuedAudioSize(g_audio) < 44100 * 4 / 10)
      SDL_QueueAudio(g_audio, data, static_cast<Uint32>(frames * 4));
  }
  return frames;
}

const struct
{
  SDL_Scancode key;
  unsigned id;
} kKeyMap[] = {
  {SDL_SCANCODE_UP, RETRO_DEVICE_ID_JOYPAD_UP},       {SDL_SCANCODE_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
  {SDL_SCANCODE_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},   {SDL_SCANCODE_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
  {SDL_SCANCODE_RETURN, RETRO_DEVICE_ID_JOYPAD_START}, {SDL_SCANCODE_RSHIFT, RETRO_DEVICE_ID_JOYPAD_SELECT},
  {SDL_SCANCODE_Z, RETRO_DEVICE_ID_JOYPAD_B},         {SDL_SCANCODE_X, RETRO_DEVICE_ID_JOYPAD_A},
  {SDL_SCANCODE_A, RETRO_DEVICE_ID_JOYPAD_Y},         {SDL_SCANCODE_S, RETRO_DEVICE_ID_JOYPAD_X},
  {SDL_SCANCODE_Q, RETRO_DEVICE_ID_JOYPAD_L},         {SDL_SCANCODE_E, RETRO_DEVICE_ID_JOYPAD_R},
  {SDL_SCANCODE_W, RETRO_DEVICE_ID_JOYPAD_L2},        {SDL_SCANCODE_R, RETRO_DEVICE_ID_JOYPAD_R2},
};

const struct
{
  SDL_GameControllerButton btn;
  unsigned id;
} kPadMap[] = {
  {SDL_CONTROLLER_BUTTON_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP},
  {SDL_CONTROLLER_BUTTON_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
  {SDL_CONTROLLER_BUTTON_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
  {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
  {SDL_CONTROLLER_BUTTON_START, RETRO_DEVICE_ID_JOYPAD_START},
  {SDL_CONTROLLER_BUTTON_BACK, RETRO_DEVICE_ID_JOYPAD_SELECT},
  {SDL_CONTROLLER_BUTTON_A, RETRO_DEVICE_ID_JOYPAD_B},      // Cross
  {SDL_CONTROLLER_BUTTON_B, RETRO_DEVICE_ID_JOYPAD_A},      // Circle
  {SDL_CONTROLLER_BUTTON_X, RETRO_DEVICE_ID_JOYPAD_Y},      // Square
  {SDL_CONTROLLER_BUTTON_Y, RETRO_DEVICE_ID_JOYPAD_X},      // Triangle
  {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, RETRO_DEVICE_ID_JOYPAD_L},
  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, RETRO_DEVICE_ID_JOYPAD_R},
};

void InputPollCb()
{
  if (!g_play)
    return;
  SDL_Event ev;
  while (SDL_PollEvent(&ev))
  {
    if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
      g_quit = true;
    else if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F11)
    {
      const bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
      SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    else if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_TAB)
      g_ff_hold = true;
    else if (ev.type == SDL_KEYUP && ev.key.keysym.scancode == SDL_SCANCODE_TAB)
      g_ff_hold = false;
    else if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F5)
      fprintf(stderr, "quicksave %s\n", SaveState("scratch/bin/quick.state") ? "ok" : "FAILED");
    else if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F9)
      fprintf(stderr, "quickload %s\n", LoadState("scratch/bin/quick.state") ? "ok" : "FAILED");
    else if (ev.type == SDL_CONTROLLERDEVICEADDED && !g_pad)
      g_pad = SDL_GameControllerOpen(ev.cdevice.which);
  }
  g_buttons = 0;
  const Uint8* keys = SDL_GetKeyboardState(nullptr);
  for (const auto& m : kKeyMap)
  {
    if (keys[m.key])
      g_buttons |= (1 << m.id);
  }
  if (g_pad)
  {
    for (const auto& m : kPadMap)
    {
      if (SDL_GameControllerGetButton(g_pad, m.btn))
        g_buttons |= (1 << m.id);
    }
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192)
      g_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_L2);
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192)
      g_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_R2);
  }
}

int16_t InputStateCb(unsigned port, unsigned device, unsigned, unsigned id)
{
  if (port != 0 || device != RETRO_DEVICE_JOYPAD)
    return 0;
  if (id < 16 && ((g_buttons | g_inject_buttons | g_repl_buttons) & (1 << id)))
    return 1;
  for (const InputEvent& ev : g_input_script)
  {
    if (ev.button == id && g_frame >= ev.start && g_frame < ev.end)
      return 1;
  }
  return 0;
}

} // namespace

namespace {
int TraceHookFn(uint32_t pc, uint32_t* gpr, uint32_t*)
{
  fprintf(stderr, "[%6u] hook pc=%08X a0=%08X a1=%08X v0=%08X ra=%08X\n", g_frame, pc, gpr[4], gpr[5], gpr[2],
          gpr[31]);
  return PSXPORT_HOOK_CONTINUE;
}
} // namespace

namespace {
// Watchdog: SIGALRM every few seconds; if the frame counter hasn't advanced
// since the previous alarm, the emulated machine (or the host loop) is stuck
// — report where and die instead of hanging silently.
volatile unsigned g_watchdog_last_frame = ~0u;
void WatchdogAlarm(int)
{
  if (g_watchdog_last_frame == g_frame)
  {
    fprintf(stderr, "WATCHDOG: no progress since frame %u — dumping and killing\n", g_frame);
    void* frames[24];
    const int n = backtrace(frames, 24);
    fprintf(stderr, "host backtrace (%d frames):\n", n);
    backtrace_symbols_fd(frames, n, 2);
    if (g_ram)
      psxport_dump_cpu_state(g_ram);
    _exit(2);
  }
  g_watchdog_last_frame = g_frame;
  alarm(5);
}
} // namespace

int main(int argc, char** argv)
{
  const char* disc = nullptr;
  const char* inputscript = nullptr;
  const char* loadstate = nullptr;
  const char* savestate = nullptr; // saved on exit
  unsigned frames = 600;
  for (int i = 1; i < argc; i++)
  {
    const auto arg = [&](const char* name) {
      return strcmp(argv[i], name) == 0 && i + 1 < argc ? argv[++i] : nullptr;
    };
    if (const char* v = arg("-frames"))
      frames = static_cast<unsigned>(strtoul(v, nullptr, 10));
    else if (const char* v = arg("-dumpdir"))
      g_dump_dir = v;
    else if (const char* v = arg("-dumpinterval"))
      g_dump_interval = static_cast<unsigned>(strtoul(v, nullptr, 10));
    else if (const char* v = arg("-inputscript"))
      inputscript = v;
    else if (const char* v = arg("-loadstate"))
      loadstate = v;
    else if (const char* v = arg("-savestate"))
      savestate = v;
    else if (const char* v = arg("-bios"))
      g_system_dir = v;
    else if (strcmp(argv[i], "-play") == 0)
      g_play = true;
    else if (strcmp(argv[i], "-repl") == 0)
      g_repl = true;
    else if (strcmp(argv[i], "-fastboot") == 0)
      g_fast_boot = true;
    else if (strcmp(argv[i], "-slowcd") == 0)
      g_fast_cd = false;

    else if (argv[i][0] != '-')
      disc = argv[i];
  }
  if (!disc)
  {
    fprintf(stderr,
            "usage: %s <disc.chd> [-frames N] [-dumpdir DIR] [-dumpinterval N] [-inputscript FILE] [-bios DIR]\n"
            "          [-loadstate FILE] [-savestate FILE] [-play] [-repl]\n"
            "  -repl drive commands (stdin): run N | tap <Button> [N] | press/release <Button> | shot PATH\n"
            "                                save/load <path> | r/w/w8 <addr> [..] | trace <pc> | bt | state\n",
            argv[0]);
    return 1;
  }
  if (inputscript && !LoadInputScript(inputscript))
  {
    fprintf(stderr, "failed to load input script %s\n", inputscript);
    return 1;
  }

  if (g_play)
  {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
    {
      fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
    // Size the window to the screen: a 4:3 box at ~85% of the desktop height
    // (falls back to 960x720 if the display mode is unavailable). The output is
    // then scaled to whatever the window/screen size is, in VideoCb.
    int win_w = 960, win_h = 720;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.h > 0)
    {
      win_h = (dm.h * 85) / 100;
      win_w = (win_h * 4) / 3;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest"); // sharp pixels, no bilinear
    g_window = SDL_CreateWindow("wide60rt", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                                SDL_WINDOW_RESIZABLE);
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_AudioSpec want = {}, have = {};
    want.freq = 44100;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio)
      SDL_PauseAudioDevice(g_audio, 0);
  }

  // Instant-CD default for the PC port: there is no physical drive on PC, so disc
  // operations are instant. Mask 0x3F = seek(1)+reset(2)+startup(4)+ReadN
  // pacing(8)+ReadTOC(16)+Pause(32). bit8 already delivers data sectors consumer-
  // paced: it reschedules the next sector +7000 cyc the instant the consumer acks
  // the prior DATA_READY (IRQBuffer&0xF clear), falling back to native rate only
  // while the consumer is still draining -- so delivery tracks how fast the game's
  // IRQ handler consumes, not beetle's cd_2x_speedup cap, with no deadlock risk.
  // (A bit64 "defer-until-acked" variant was tried and removed: it deadlocks BIOS
  // CD init by deferring forever when the consumer legitimately isn't draining, and
  // gains nothing real -- the residual intro loads are CPU/consumer-paced, not
  // delivery-paced.) Verified on Tomba2 through the whole pipeline (logos -> FMV ->
  // title -> New Game -> in-level 3D) with no desync. Streaming (XA/STRSND) audio
  // keeps native real-time pacing. Override with PSXPORT_CD_INSTANT=0 for native HW
  // timing (RE / oracle runs). -fastcd is now redundant. env wins when set.
  if (psxport_cd_instant < 0)
    psxport_cd_instant = 0x3F;

  // Display enhancements (widescreen + internal upscale) default ON for
  // interactive play, OFF for headless/RE runs — the wide60 reproject harness
  // and GTE/GP0 capture assume NATIVE coordinates, which both the upscale and
  // the widescreen hack change. Env vars override in either mode. (Read before
  // the core loads options.)
  if (!g_play)
  {
    g_widescreen = false;
    g_internal_res = nullptr; // native 1x
  }
  if (const char* v = std::getenv("PSXPORT_INTERNAL_RES"))
    g_internal_res = (*v && strcmp(v, "1x") != 0) ? v : nullptr;
  if (std::getenv("PSXPORT_WIDE"))
    g_widescreen = true;
  if (std::getenv("PSXPORT_NOWIDE"))
    g_widescreen = false;
  if (const char* v = std::getenv("PSXPORT_WS_ASPECT"))
    g_ws_aspect = v;

  retro_set_environment(EnvironmentCb);
  retro_set_video_refresh(VideoCb);
  retro_set_audio_sample(AudioSampleCb);
  retro_set_audio_sample_batch(AudioBatchCb);
  retro_set_input_poll(InputPollCb);
  retro_set_input_state(InputStateCb);

  signal(SIGALRM, WatchdogAlarm);
  alarm(5);

  retro_init();

  retro_game_info game = {};
  game.path = disc;
  if (!retro_load_game(&game))
  {
    fprintf(stderr, "retro_load_game failed for %s\n", disc);
    return 1;
  }

  // Pick up the core's display aspect (widescreen hack changes it from 4:3).
  {
    retro_system_av_info av = {};
    retro_get_system_av_info(&av);
    if (av.geometry.aspect_ratio > 0.0f)
      g_aspect = av.geometry.aspect_ratio;
    char msg[96];
    snprintf(msg, sizeof(msg), "internal_res=%s widescreen=%s aspect=%.3f base=%ux%u",
             g_internal_res ? g_internal_res : "1x", g_widescreen ? g_ws_aspect : "off", g_aspect,
             av.geometry.base_width, av.geometry.base_height);
    RtLog("disp", msg);
  }

  g_ram = static_cast<uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));

  // HLE BIOS (pure native, no MIPS ROM). Bring-up flag PSXPORT_HLE_BIOS=1.
  if (const char* v = std::getenv("PSXPORT_HLE_BIOS"))
    g_hle_bios = (*v && *v != '0');

  if (g_hle_bios && g_ram)
  {
    fprintf(stderr,
            "[hle] *** PSXPORT_HLE_BIOS is WORK-IN-PROGRESS and NOT PLAYABLE yet ***\n"
            "[hle] *** boots the EXE then black-screens (no IRQ/event delivery, stage 3).\n"
            "[hle] *** to play, run WITHOUT PSXPORT_HLE_BIOS (uses the BIOS ROM).\n");
    // Trap every A0/B0/C0 syscall to the native BIOS; no ROM dispatcher exists.
    psxport_add_hook(0xA0, 0, HleSyscallHook);
    psxport_add_hook(0xB0, 0, HleSyscallHook);
    psxport_add_hook(0xC0, 0, HleSyscallHook);
    // Boot: load the disc's EXE into RAM and jump the CPU into it (skip the ROM).
    // The BIOS/boot policy lives here in the runtime; beetle only exposes the
    // generic CPU primitives (set_pc + the gpr file).
    uint32_t pc = 0, sp = 0, gp = 0;
    if (psxport_hle_boot(g_ram, &pc, &sp, &gp))
    {
      uint32_t* r = psxport_cpu_gpr();
      r[28] = gp;                     // $gp
      if (sp) { r[29] = sp; r[30] = sp; } // $sp, $fp
      r[31] = 0;                      // $ra (EXE entry shouldn't return)
      r[4] = 1; r[5] = 0;             // $a0=argc, $a1=argv (mirrors retail boot)
      psxport_cpu_set_pc(pc);
      fprintf(stderr, "[hle] booted EXE pc=%08X sp=%08X gp=%08X\n", pc, sp, gp);
    }
    else
      fprintf(stderr, "[hle] BOOT FAILED — falling back to BIOS ROM\n");
  }
  else
  {
    // TTY capture on the kernel A0/B0 dispatchers (fixed kernel addresses)
    psxport_add_hook(0xA0, 0, BiosPutcharHook);
    psxport_add_hook(0xB0, 0, BiosPutcharHook);
  }

  // HLE BIOS: native-service OpenBIOS's blocking per-sector CD read. Generic
  // (OpenBIOS, game-independent); signature-checked so a different BIOS can't
  // misfire. PSXPORT_BIOS_HLE=0 opts out for RE/oracle runs.
  if (const char* v = std::getenv("PSXPORT_BIOS_HLE"))
    g_bios_hle = (*v && *v != '0');
  if (g_bios_hle && g_ram)
    psxport_add_hook(0xBFC03A9C, 0x24A30096, BiosHleCdBlockRead);

  // Detect Tomba!2 by filename, tolerant of punctuation/case ("Tomba! 2",
  // "Tomba 2", "tomba2", ...). The per-game hooks are instruction-signature
  // gated, so a false positive on another disc is inert — match broadly.
  {
    std::string low(disc);
    for (char& c : low) c = static_cast<char>(tolower((unsigned char)c));
    g_tomba2 = low.find("tomba") != std::string::npos;
  }
  if (g_tomba2 && g_ram)
    Tomba2_Install();

  // wide60 reprojecting renderer registers the GTE/RTP/GPU capture hooks last,
  // so it owns them when enabled (overrides the per-game RE consumers).
  g_wide60 = std::getenv("PSXPORT_WIDE60") != nullptr;
  if (g_wide60)
    Wide60_Install();

  if (loadstate)
  {
    if (LoadState(loadstate))
      RtLog("state", "loaded");
    else
      fprintf(stderr, "WARN: failed to load state %s\n", loadstate);
  }

  // RE aid: PSXPORT_RAMDUMP="frame:path;frame:path" — 2MB RAM snapshots
  struct RamDump
  {
    unsigned frame;
    std::string path;
  };
  std::vector<RamDump> ram_dumps;
  if (const char* env = std::getenv("PSXPORT_RAMDUMP"))
  {
    const char* p = env;
    while (*p)
    {
      char* end;
      const unsigned f = static_cast<unsigned>(strtoul(p, &end, 10));
      if (*end != ':')
        break;
      const char* path_start = end + 1;
      const char* sep = strchr(path_start, ';');
      ram_dumps.push_back({f, sep ? std::string(path_start, sep) : std::string(path_start)});
      p = sep ? sep + 1 : path_start + ram_dumps.back().path.size();
    }
  }
  // RE aid: PSXPORT_WATCHW="hexaddr" — log every CPU store to that word (the PC
  // that wrote it + value). Finds the code that owns a variable.
  if (const char* env = std::getenv("PSXPORT_WATCHW"))
    psxport_watch_addr = (static_cast<uint32_t>(strtoul(env, nullptr, 16)) & 0x1FFFFC);
  // RE aid: PSXPORT_POKE="frame:addr=val;A..B:addr=val" — write a 32-bit word into
  // RAM at a frame (single) or every frame in [A,B] (range, to latch a gate against
  // the game rewriting it). addr/val are hex (PSX addr masked to 2MB, word-aligned).
  // Lets gate hypotheses be tested purely via env, then seen via PSXPORT_FRAMEDUMP.
  struct Poke
  {
    unsigned start, end;
    uint32_t word, val;
  };
  std::vector<Poke> pokes;
  if (const char* env = std::getenv("PSXPORT_POKE"))
  {
    const char* p = env;
    while (*p)
    {
      char* e;
      const unsigned a = static_cast<unsigned>(strtoul(p, &e, 10));
      unsigned b = a;
      if (e[0] == '.' && e[1] == '.')
        b = static_cast<unsigned>(strtoul(e + 2, &e, 10));
      if (*e != ':')
        break;
      const uint32_t addr = static_cast<uint32_t>(strtoul(e + 1, &e, 16)) & 0x1FFFFF;
      if (*e != '=')
        break;
      const uint32_t val = static_cast<uint32_t>(strtoul(e + 1, &e, 16));
      pokes.push_back({a, b, addr >> 2, val});
      p = (*e == ';') ? e + 1 : e;
    }
  }
  // RE aid: PSXPORT_FRAMEDUMP="frame:path.ppm;frame:path.ppm" — exact-frame framebuffer
  if (const char* env = std::getenv("PSXPORT_FRAMEDUMP"))
  {
    const char* p = env;
    while (*p)
    {
      char* end;
      const unsigned f = static_cast<unsigned>(strtoul(p, &end, 10));
      if (*end != ':')
        break;
      const char* path_start = end + 1;
      const char* sep = strchr(path_start, ';');
      g_frame_dumps.push_back({f, sep ? std::string(path_start, sep) : std::string(path_start)});
      p = sep ? sep + 1 : path_start + g_frame_dumps.back().second.size();
    }
  }
  // RE aid: PSXPORT_MEMWATCH="hexaddr,hexaddr:path" — per-frame CSV of 32-bit words
  // (aligned, hex). Gates are words, not bytes; reading bytes hides word values and
  // wrap (a +1/frame counter's low byte looks like it "resets" at 256).
  std::vector<uint32_t> watch_addrs;
  FILE* watch_fp = nullptr;
  if (const char* env = std::getenv("PSXPORT_MEMWATCH"))
  {
    const char* colon = strrchr(env, ':');
    if (colon)
    {
      const char* p = env;
      while (p < colon)
      {
        char* end;
        watch_addrs.push_back((static_cast<uint32_t>(strtoul(p, &end, 16)) & 0x1FFFFF) >> 2);
        p = (*end == ',') ? end + 1 : colon;
      }
      watch_fp = fopen(colon + 1, "w");
    }
  }
  // RE aid: PSXPORT_PCCOV="start-end:path;start-end:path" — executed-PC
  // bitmaps (1 bit per RAM word) collected over frame ranges
  struct CovRange
  {
    unsigned start, end;
    std::string path;
    std::vector<uint8_t> bitmap;
  };
  std::vector<CovRange> cov_ranges;
  if (const char* env = std::getenv("PSXPORT_PCCOV"))
  {
    const char* p = env;
    while (*p)
    {
      char* end;
      const unsigned s = static_cast<unsigned>(strtoul(p, &end, 10));
      if (*end != '-')
        break;
      const unsigned e = static_cast<unsigned>(strtoul(end + 1, &end, 10));
      if (*end != ':')
        break;
      const char* path_start = end + 1;
      const char* sep = strchr(path_start, ';');
      cov_ranges.push_back({s, e, sep ? std::string(path_start, sep) : std::string(path_start), {}});
      p = sep ? sep + 1 : path_start + cov_ranges.back().path.size();
    }
  }
  // debug: PSXPORT_GAMELOG=path — per-frame CSV: frame, render-hook hits
  FILE* gamelog_fp = nullptr;
  if (const char* env = std::getenv("PSXPORT_GAMELOG"))
    gamelog_fp = fopen(env, "w");
  // debug: PSXPORT_RAMHASH="interval:path" — FNV1a of RAM every N frames
  FILE* ramhash_fp = nullptr;
  unsigned ramhash_interval = 0;
  if (const char* env = std::getenv("PSXPORT_RAMHASH"))
  {
    char* end;
    ramhash_interval = static_cast<unsigned>(strtoul(env, &end, 10));
    if (*end == ':')
      ramhash_fp = fopen(end + 1, "w");
  }
  const auto per_frame = [&]() {
    if (g_wide60)
      Wide60_FrameEnd(g_frame); // finalize the previous run's capture
    psxport_frame = g_frame;
    if (gamelog_fp && g_tomba2)
      fprintf(gamelog_fp, "%u,%u\n", g_frame, Tomba2_GetAndResetRenderHits());
    if (ramhash_fp && g_ram && ramhash_interval && (g_frame % ramhash_interval) == 0)
    {
      uint64_t h = 0xcbf29ce484222325ULL;
      for (size_t i = 0; i < 2 * 1024 * 1024; i++)
      {
        h ^= g_ram[i];
        h *= 0x100000001b3ULL;
      }
      fprintf(ramhash_fp, "%u,%016llx\n", g_frame, (unsigned long long)h);
    }
    for (CovRange& cr : cov_ranges)
    {
      if (g_frame == cr.start)
      {
        cr.bitmap.assign(64 * 1024, 0);
        psxport_cov_bitmap = cr.bitmap.data();
      }
      else if (g_frame == cr.end)
      {
        psxport_cov_bitmap = nullptr;
        if (FILE* fp = fopen(cr.path.c_str(), "wb"))
        {
          fwrite(cr.bitmap.data(), 1, cr.bitmap.size(), fp);
          fclose(fp);
        }
      }
    }
    if (g_ram)
    {
      if (watch_fp)
      {
        fprintf(watch_fp, "%u", g_frame);
        for (const uint32_t a : watch_addrs)
          fprintf(watch_fp, ",%08x", reinterpret_cast<const uint32_t*>(g_ram)[a]);
        fprintf(watch_fp, "\n");
      }
      for (const RamDump& rd : ram_dumps)
      {
        if (rd.frame == g_frame)
        {
          if (FILE* fp = fopen(rd.path.c_str(), "wb"))
          {
            fwrite(g_ram, 1, 2 * 1024 * 1024, fp);
            fclose(fp);
          }
        }
      }
      for (const Poke& pk : pokes)
        if (g_frame >= pk.start && g_frame <= pk.end)
          reinterpret_cast<uint32_t*>(g_ram)[pk.word] = pk.val;
      g_inject_buttons = g_tomba2 ? Tomba2_FrameTick(g_ram) : 0;
      // Hold Start during the intro to SKIP the logos (SCEA + Whoopee Camp) via
      // the PC-native intro override (runtime/games/tomba2.cpp), which advances
      // each logo through its OWN exit path — a true native skip, not emulator
      // fast-forward. See docs/tomba2-intro.md. Button-driven only.
      const bool skip_held =
        ((g_buttons | g_repl_buttons) & (1 << RETRO_DEVICE_ID_JOYPAD_START)) != 0;
      if (g_tomba2)
        Tomba2_SetSkipHeld(skip_held);
      g_module_turbo = false; // native skip replaced the Start-held fast-forward
    }
  };

  if (g_repl)
  {
    char* line = nullptr;
    size_t cap = 0;
    fprintf(stderr, "[repl] ready\n");
    fflush(stderr);
    while (getline(&line, &cap, stdin) > 0)
    {
      char cmd[16] = {};
      uint32_t a = 0, b = 0;
      char argbuf[64] = {};
      char argbuf2[1024] = {};
      if (sscanf(line, "%15s", cmd) != 1)
        continue;
      if (strcmp(cmd, "quit") == 0)
        break;
      else if (strcmp(cmd, "run") == 0 && sscanf(line, "%*s %u", &a) == 1)
      {
        const unsigned until = g_frame + a;
        alarm(15);
        for (; g_frame < until; g_frame++)
        {
          per_frame();
          retro_run();
        }
        alarm(0);
        fprintf(stderr, "[repl] at frame %u\n", g_frame);
      }
      else if (strcmp(cmd, "prof") == 0 && sscanf(line, "%*s %u", &a) == 1)
      {
        // Sample the CPU PC over `a` frames and report where time goes. Per-
        // instruction histogram (psxport_prof) is heavy, so give the watchdog
        // more slack than `run`.
        const unsigned until = g_frame + a;
        psxport_prof_reset();
        psxport_prof = 1;
        alarm(60);
        for (; g_frame < until; g_frame++)
        {
          per_frame();
          retro_run();
        }
        alarm(0);
        psxport_prof = 0;
        fprintf(stderr, "[repl] profiled to frame %u\n", g_frame);
        psxport_prof_report(25);
      }
      else if (strcmp(cmd, "r") == 0 && sscanf(line, "%*s %x %u", &a, &b) >= 1)
      {
        if (!b)
          b = 16;
        fprintf(stderr, "[repl] %08X:", a);
        for (uint32_t i = 0; i < b && i < 256; i++)
          fprintf(stderr, " %02X", g_ram[(a + i) & 0x1FFFFF]);
        fprintf(stderr, "\n");
      }
      else if (strcmp(cmd, "w") == 0 && sscanf(line, "%*s %x %x", &a, &b) == 2)
      {
        memcpy(g_ram + (a & 0x1FFFFF), &b, 4);
        fprintf(stderr, "[repl] ok\n");
      }
      else if (strcmp(cmd, "w8") == 0 && sscanf(line, "%*s %x %x", &a, &b) == 2)
      {
        g_ram[a & 0x1FFFFF] = static_cast<uint8_t>(b);
        fprintf(stderr, "[repl] ok\n");
      }
      else if (strcmp(cmd, "cd") == 0 && sscanf(line, "%*s %i", &a) == 1)
      {
        psxport_cd_instant = static_cast<int>(a);
        fprintf(stderr, "[repl] cd_instant=%d\n", psxport_cd_instant);
      }
      else if (strcmp(cmd, "cdclog") == 0 && sscanf(line, "%*s %u", &a) == 1)
      {
        psxport_cdc_log = static_cast<int>(a);
        fprintf(stderr, "[repl] cdclog=%d\n", psxport_cdc_log);
      }
      else if (strcmp(cmd, "bioslog") == 0 && sscanf(line, "%*s %u", &a) == 1)
      {
        psxport_bios_log = static_cast<int>(a);
        fprintf(stderr, "[repl] bioslog=%d\n", psxport_bios_log);
      }
      else if (strcmp(cmd, "trace") == 0 && sscanf(line, "%*s %x", &a) == 1)
      {
        psxport_add_hook(a, 0, TraceHookFn);
        fprintf(stderr, "[repl] tracing %08X\n", a);
      }
      else if (strcmp(cmd, "bt") == 0)
      {
        psxport_dump_cpu_state(g_ram);
      }
      else if (strcmp(cmd, "state") == 0)
      {
        fprintf(stderr, "[repl] frame=%u last_pc=%08X repl_buttons=%04X\n", g_frame, psxport_last_pc, g_repl_buttons);
      }
      else if (strcmp(cmd, "shot") == 0 && sscanf(line, "%*s %1023s", argbuf2) == 1)
      {
        // Dump the last presented framebuffer (cached in VideoCb) to a PPM so the
        // current screen can be eyeballed while driving interactively.
        if (g_last_fb.empty())
          fprintf(stderr, "[repl] no frame captured yet\n");
        else
        {
          const unsigned bpp = (g_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
          WriteFramePPMPath(g_last_fb.data(), g_last_fb_w, g_last_fb_h, static_cast<size_t>(g_last_fb_w) * bpp, argbuf2);
          fprintf(stderr, "[repl] shot %ux%u -> %s\n", g_last_fb_w, g_last_fb_h, argbuf2);
        }
      }
      else if ((strcmp(cmd, "press") == 0 || strcmp(cmd, "release") == 0) && sscanf(line, "%*s %31s", argbuf) == 1)
      {
        const int bit = ButtonBit(argbuf);
        if (bit < 0)
          fprintf(stderr, "[repl] ? button %s\n", argbuf);
        else
        {
          if (cmd[1] == 'r') // pRess
            g_repl_buttons |= (1 << bit);
          else
            g_repl_buttons &= ~(1 << bit);
          fprintf(stderr, "[repl] buttons=%04X\n", g_repl_buttons);
        }
      }
      else if (strcmp(cmd, "tap") == 0 && sscanf(line, "%*s %31s %u", argbuf, &a) >= 1)
      {
        // hold <button> for `a` frames (default 4), then release — one press
        const int bit = ButtonBit(argbuf);
        if (bit < 0)
        {
          fprintf(stderr, "[repl] ? button %s\n", argbuf);
        }
        else
        {
          const unsigned hold = a ? a : 4;
          g_repl_buttons |= (1 << bit);
          alarm(15);
          for (unsigned i = 0; i < hold; i++, g_frame++)
          {
            per_frame();
            retro_run();
          }
          g_repl_buttons &= ~(1 << bit);
          alarm(0);
          fprintf(stderr, "[repl] tapped %s, at frame %u\n", argbuf, g_frame);
        }
      }
      else if ((strcmp(cmd, "save") == 0 || strcmp(cmd, "load") == 0) && sscanf(line, "%*s %63s", argbuf) == 1)
      {
        const bool ok = (cmd[0] == 's') ? SaveState(argbuf) : LoadState(argbuf);
        fprintf(stderr, "[repl] %s %s %s\n", cmd, argbuf, ok ? "ok" : "FAILED");
      }
      else
      {
        fprintf(stderr, "[repl] ? %s", line);
      }
      fflush(stderr);
      (void)argbuf;
    }
    free(line);
  }
  else if (g_play)
  {
    for (g_frame = 0; !g_quit; g_frame++)
    {
      const unsigned steps = (g_ff_hold || g_module_turbo) ? 8 : 1;
      for (unsigned s = 0; s < steps; s++, g_frame += (s < steps ? 1 : 0))
      {
        g_present_this_run = (s == steps - 1); // present/queue only the last
        per_frame();
        retro_run();
      }
      g_present_this_run = true;
    }
  }
  else
  {
    for (g_frame = 0; g_frame < frames; g_frame++)
    {
      per_frame();
      retro_run();
    }
  }

  if (savestate)
  {
    if (SaveState(savestate))
      fprintf(stderr, "saved state to %s\n", savestate);
    else
      fprintf(stderr, "WARN: failed to save state %s\n", savestate);
  }

  retro_unload_game();
  retro_deinit();
  fprintf(stderr, "ran %u frames\n", frames);
  return 0;
}

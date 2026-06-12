// psxport runtime: minimal headless/libretro host statically linked against
// the Beetle PSX core (vendor/beetle-psx, GPL-2). This is the base of the
// interpreter+overrides PC port: we own the run/present loop, and core
// internals are hooked by patching the vendored sources directly.
//
// Usage: wide60rt <disc.chd> [-frames N] [-dumpdir DIR] [-dumpinterval N]
//                 [-inputscript FILE] [-bios DIR] [-play]
// -play opens an SDL window with keyboard/gamepad input and audio:
//   arrows = d-pad, Z/X/A/S = Cross/Circle/Square/Triangle,
//   Enter/RShift = Start/Select, Q/W E/R = L1/L2 R1/R2, Esc quits.
// Input script lines: "<start_frame> <end_frame> <Button>" (digital pad).

#include "libretro.h"

#include <SDL2/SDL.h>

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

retro_pixel_format g_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

// --- play mode (SDL window/input/audio) ---
bool g_play = false;
bool g_quit = false;
SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
unsigned g_tex_w = 0, g_tex_h = 0;
SDL_AudioDeviceID g_audio = 0;
SDL_GameController* g_pad = nullptr;
int16_t g_buttons = 0; // bitmask by RETRO_DEVICE_ID_JOYPAD_*

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
      return false; // all core options at defaults
    default:
      return false;
  }
}

void WriteFramePPM(const void* data, unsigned width, unsigned height, size_t pitch)
{
  char path[1024];
  snprintf(path, sizeof(path), "%s/frame_%06u.ppm", g_dump_dir.c_str(), g_frame);
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

void VideoCb(const void* data, unsigned width, unsigned height, size_t pitch)
{
  if (data && !g_dump_dir.empty() && g_dump_interval && (g_frame % g_dump_interval) == 0)
    WriteFramePPM(data, width, height, pitch);

  if (!g_play || !data)
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
  SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
  SDL_RenderPresent(g_renderer);
}

void AudioSampleCb(int16_t, int16_t) {}
size_t AudioBatchCb(const int16_t* data, size_t frames)
{
  if (g_audio)
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
  if (g_play && id < 16 && (g_buttons & (1 << id)))
    return 1;
  for (const InputEvent& ev : g_input_script)
  {
    if (ev.button == id && g_frame >= ev.start && g_frame < ev.end)
      return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char** argv)
{
  const char* disc = nullptr;
  const char* inputscript = nullptr;
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
    else if (const char* v = arg("-bios"))
      g_system_dir = v;
    else if (strcmp(argv[i], "-play") == 0)
      g_play = true;
    else if (argv[i][0] != '-')
      disc = argv[i];
  }
  if (!disc)
  {
    fprintf(stderr,
            "usage: %s <disc.chd> [-frames N] [-dumpdir DIR] [-dumpinterval N] [-inputscript FILE] [-bios DIR]\n",
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
    g_window = SDL_CreateWindow("wide60rt", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
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

  retro_set_environment(EnvironmentCb);
  retro_set_video_refresh(VideoCb);
  retro_set_audio_sample(AudioSampleCb);
  retro_set_audio_sample_batch(AudioBatchCb);
  retro_set_input_poll(InputPollCb);
  retro_set_input_state(InputStateCb);

  retro_init();

  retro_game_info game = {};
  game.path = disc;
  if (!retro_load_game(&game))
  {
    fprintf(stderr, "retro_load_game failed for %s\n", disc);
    return 1;
  }

  if (g_play)
  {
    for (g_frame = 0; !g_quit; g_frame++)
      retro_run(); // paced by vsync in the present
  }
  else
  {
    for (g_frame = 0; g_frame < frames; g_frame++)
      retro_run();
  }

  retro_unload_game();
  retro_deinit();
  fprintf(stderr, "ran %u frames\n", frames);
  return 0;
}

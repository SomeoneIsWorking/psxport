// psxport runtime: minimal headless/libretro host statically linked against
// the Beetle PSX core (vendor/beetle-psx, GPL-2). This is the base of the
// interpreter+overrides PC port: we own the run/present loop, and core
// internals are hooked by patching the vendored sources directly.
//
// Usage: wide60rt <disc.chd> [-frames N] [-dumpdir DIR] [-dumpinterval N]
//                 [-inputscript FILE] [-bios DIR]
// Input script lines: "<start_frame> <end_frame> <Button>" (digital pad).

#include "libretro.h"

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
}

void AudioSampleCb(int16_t, int16_t) {}
size_t AudioBatchCb(const int16_t*, size_t frames)
{
  return frames;
}
void InputPollCb() {}

int16_t InputStateCb(unsigned port, unsigned device, unsigned, unsigned id)
{
  if (port != 0 || device != RETRO_DEVICE_JOYPAD)
    return 0;
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

  for (g_frame = 0; g_frame < frames; g_frame++)
    retro_run();

  retro_unload_game();
  retro_deinit();
  fprintf(stderr, "ran %u frames\n", frames);
  return 0;
}

#include "common/env.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace psxport {

static std::optional<fs::path> FindUpward(const char* name)
{
  fs::path dir = fs::current_path();
  for (;;)
  {
    fs::path candidate = dir / name;
    if (fs::exists(candidate))
      return candidate;
    if (!dir.has_parent_path() || dir.parent_path() == dir)
      return std::nullopt;
    dir = dir.parent_path();
  }
}

std::optional<std::string> GetEnv(const std::string& key)
{
  if (const char* v = std::getenv(key.c_str()))
    return std::string(v);

  if (auto envfile = FindUpward(".env"))
  {
    std::ifstream in(*envfile);
    std::string line;
    while (std::getline(in, line))
    {
      if (line.empty() || line[0] == '#')
        continue;
      auto eq = line.find('=');
      if (eq != std::string::npos && line.substr(0, eq) == key)
        return line.substr(eq + 1);
    }
  }
  return std::nullopt;
}

std::optional<std::string> ResolveDiscPath(const std::string& cli_arg)
{
  if (!cli_arg.empty())
    return cli_arg;
  if (auto v = GetEnv("PSXPORT_DISC"); v && !v->empty())
    return v;

  // Drop-in: any *.chd next to the .env (repo root), else in the current dir.
  fs::path root = fs::current_path();
  if (auto envfile = FindUpward(".env"))
    root = envfile->parent_path();
  for (const fs::path& dir : {root, fs::current_path()})
  {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
      if (e.path().extension() == ".chd")
        return e.path().string();
  }
  return std::nullopt;
}

}  // namespace psxport

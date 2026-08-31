#!/usr/bin/env python3
"""Generate a bare psxport consumer without inventing title behavior.

The generated launcher provisions the framework, discovers the disc's declared boot executable with
``discdump``, emits the title substrate, and starts psxport's generic resumable execution profile.
The emitter proves the generic PsyQ VSync contract from the executable; the scaffold does not invent
or copy a title-specific frame loop.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


class Refusal(ValueError):
    """A request which would create an ambiguous or nonportable scaffold."""


SLUG = re.compile(r"[a-z][a-z0-9-]*\Z")


@dataclass(frozen=True)
class Scaffold:
    title: str
    slug: str
    output: Path
    pin: str
    disc: Path | None = None


RUN_SH = """#!/usr/bin/env sh
cd "$(dirname "$0")" || exit 1
exec uv run --frozen python bootstrap.py "$@"
"""

BOOTSTRAP = r'''"""Prepare {title}'s bare psxport substrate from a user-supplied disc."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FRAMEWORK = ROOT / "external" / "psxport"
BUILD = ROOT / "build" / "player"
EXTRACTED = ROOT / "scratch" / "bin" / "{slug}"
GENERATED_SOURCE = ROOT / "generated" / "recompiled.c"


class Refusal(RuntimeError):
    pass


def run(command: list[str], environment: dict[str, str], *, cwd: Path = ROOT) -> None:
    completed = subprocess.run(command, cwd=cwd, env=environment, check=False)
    if completed.returncode:
        raise Refusal(f"command failed ({{completed.returncode}}): {{' '.join(command)}}")


def pin() -> tuple[str, str]:
    values = {{}}
    for line in (ROOT / "psxport.pin").read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip()] = value.strip()
    if not values.get("url") or not values.get("commit"):
        raise Refusal("psxport.pin must provide url and immutable commit")
    return values["url"], values["commit"]


def bootstrap_framework(environment: dict[str, str]) -> None:
    if (FRAMEWORK / "cmake" / "psxport.cmake").is_file():
        return
    shared = ROOT.parent / "psxport"
    FRAMEWORK.parent.mkdir(parents=True, exist_ok=True)
    if (shared / "cmake" / "psxport.cmake").is_file():
        FRAMEWORK.symlink_to(os.path.relpath(shared, FRAMEWORK.parent))
        return
    url, commit = pin()
    run(["git", "clone", url, str(FRAMEWORK)], environment)
    run(["git", "checkout", commit], environment, cwd=FRAMEWORK)
    run(["git", "submodule", "update", "--init", "vendor/beetle-psx", "vendor/lucent"], environment, cwd=FRAMEWORK)
    run(["git", "submodule", "update", "--init", "deps/libchdr"], environment, cwd=FRAMEWORK / "vendor" / "beetle-psx")


def ensure_framework(environment: dict[str, str]) -> None:
    bootstrap_framework(environment)
    run([
        sys.executable,
        "-B",
        str(FRAMEWORK / "tools" / "psxport_sync.py"),
        "--consumer",
        str(ROOT),
        "--auto",
    ], environment)


def resolve_disc(value: str | None, environment: dict[str, str]) -> Path:
    candidate = Path(value).expanduser() if value else None
    if candidate is None and environment.get("PSXPORT_DISC"):
        candidate = Path(environment["PSXPORT_DISC"]).expanduser()
    if candidate is None and (ROOT / ".env").is_file():
        for line in (ROOT / ".env").read_text(encoding="utf-8").splitlines():
            key, separator, raw = line.partition("=")
            if separator and key.strip() == "PSXPORT_DISC":
                candidate = Path(raw.strip()).expanduser()
                break
    if candidate is None:
        drops = sorted(ROOT.glob("*.chd"))
        if len(drops) == 1:
            candidate = drops[0]
        elif len(drops) > 1:
            raise Refusal("multiple .chd files found; pass --disc or set PSXPORT_DISC")
    if candidate is None or not candidate.is_file():
        raise Refusal("supply one disc with --disc, PSXPORT_DISC, .env, or a root-level .chd")
    return candidate.resolve()


def extracted_executable() -> Path:
    files = [path for path in EXTRACTED.iterdir() if path.is_file() and path.read_bytes()[:8] == b"PS-X EXE"]
    if len(files) != 1:
        raise Refusal(f"discdump produced {{len(files)}} PS-X EXE files in {{EXTRACTED}}; expected one")
    return files[0]


def configure(environment: dict[str, str]) -> None:
    run([
        "cmake", "-S", str(ROOT), "-B", str(BUILD), f"-DPython3_EXECUTABLE={{sys.executable}}",
        "-DBUILD_TESTING=OFF", "-DPSXPORT_BUILD_TESTS=OFF",
    ], environment)
    run([
        sys.executable,
        "-B",
        str(FRAMEWORK / "tools" / "psxport_sync.py"),
        "--consumer",
        str(ROOT),
        "--check",
        "--resolved",
        str(BUILD / "psxport_resolved.txt"),
    ], environment)


def prepare(disc: Path, environment: dict[str, str]) -> tuple[Path, Path]:
    ensure_framework(environment)
    configure(environment)
    run(["cmake", "--build", str(BUILD), "--target", "discdump", "{slug}_scaffold", "-j", str(os.cpu_count() or 1)], environment)
    EXTRACTED.mkdir(parents=True, exist_ok=True)
    run([str(BUILD / "psxport_build" / "tools" / "discdump"), str(disc), str(EXTRACTED)], environment)
    executable = extracted_executable()
    GENERATED_SOURCE.parent.mkdir(parents=True, exist_ok=True)
    run([
        sys.executable,
        "-B",
        str(FRAMEWORK / "tools" / "recomp" / "emit.py"),
        str(executable),
        str(GENERATED_SOURCE),
        "--whole-program",
    ], environment)
    configure(environment)
    player = BUILD / "{slug}_port"
    run(["cmake", "--build", str(BUILD), "--target", "{slug}_port", "-j", str(os.cpu_count() or 1)], environment)
    if not player.is_file():
        raise Refusal(f"CMake built {slug}_port but did not produce {{player}}")
    return executable, player


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc", help="user-supplied PSX CHD")
    parser.add_argument("--prepare-only", action="store_true", help="provision and emit without requesting launch")
    args = parser.parse_args(argv)
    environment = dict(os.environ)
    disc = resolve_disc(args.disc, environment)
    environment["PSXPORT_DISC"] = str(disc)
    environment.setdefault("PSXPORT_ASSET_DIR", str(FRAMEWORK))
    executable, player = prepare(disc, environment)
    if args.prepare_only:
        return 0
    os.execvpe(str(player), [str(player), str(executable)], environment)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refusal as error:
        print(f"[run] REFUSED: {{error}}", file=sys.stderr)
        raise SystemExit(2)
'''

CMAKE = """cmake_minimum_required(VERSION 3.24)
project({slug}_port LANGUAGES C CXX)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PSXPORT_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}/external/psxport" CACHE PATH "psxport framework")
if(NOT EXISTS "${{PSXPORT_DIR}}/cmake/psxport.cmake")
  message(FATAL_ERROR "PSXPORT_DIR is unresolved; run ./run.sh with a user-supplied disc")
endif()
set(PSXPORT_BUILD_SMOKE ON CACHE BOOL "Build the framework agnosticism smoke" FORCE)
add_subdirectory("${{PSXPORT_DIR}}" psxport_build)
add_custom_target({slug}_scaffold DEPENDS psxport_smoke)

execute_process(
  COMMAND git -C "${{PSXPORT_DIR}}" rev-parse HEAD
  RESULT_VARIABLE PSXPORT_REVISION_RESULT
  OUTPUT_VARIABLE PSXPORT_REVISION
  OUTPUT_STRIP_TRAILING_WHITESPACE)
string(LENGTH "${{PSXPORT_REVISION}}" PSXPORT_REVISION_LENGTH)
if(NOT PSXPORT_REVISION_RESULT EQUAL 0 OR NOT PSXPORT_REVISION MATCHES "^[0-9a-f]+$" OR NOT PSXPORT_REVISION_LENGTH EQUAL 40)
  message(FATAL_ERROR "cannot record psxport build provenance from ${{PSXPORT_DIR}}")
endif()
file(WRITE "${{CMAKE_BINARY_DIR}}/psxport_resolved.txt" "commit = ${{PSXPORT_REVISION}}\\n")

# The emitter owns the source list. A cold configure intentionally exposes only provisioning
# targets; bootstrap reconfigures after emitting the user-supplied executable, then builds player.
if(EXISTS "${{CMAKE_CURRENT_SOURCE_DIR}}/generated/rec_sources.cmake")
  include("${{CMAKE_CURRENT_SOURCE_DIR}}/generated/rec_sources.cmake")
  if(NOT GEN_REC_SRCS)
    message(FATAL_ERROR "generated/rec_sources.cmake did not declare GEN_REC_SRCS")
  endif()
  list(TRANSFORM GEN_REC_SRCS PREPEND "${{CMAKE_CURRENT_SOURCE_DIR}}/generated/")
  set_source_files_properties(
    ${{GEN_REC_SRCS}}
    PROPERTIES
      LANGUAGE CXX
      COMPILE_OPTIONS "-O1;-foptimize-sibling-calls;-fno-strict-aliasing;-fwrapv")
  add_executable({slug}_port game/app/main.cpp ${{GEN_REC_SRCS}})
  add_dependencies({slug}_port gen_gpu_shaders)
  set_target_properties({slug}_port PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories({slug}_port PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}/generated")
  target_link_libraries({slug}_port PRIVATE psxport)
endif()
"""

PLAYER_MAIN = r'''// A bare psxport product: generated code and shared runtime only.
#include "core.h"
#include "frame_loop_shell.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_program_image.h"
#include "hw_bind.h"
#include "overlay_table.h"
#include "recomp_iface.h"
#include "render_capabilities.h"
#include "render_mode.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>

extern "C" {
void mdec_init();
void spu_init();
void watchdog_init();
}

void gte_init();
void load_exe(const char *path, Core *core);
int rec_func_index(std::uint32_t address);
void shard_set_override(std::uint32_t address, RecOverrideFn function);
RecOverrideFn shard_get_override(std::uint32_t address);

namespace {

constexpr char kDiscEnvironment[] = "PSXPORT_DISC";

class BareRuntime final : public GameRuntime {
public:
  BareRuntime() {
    image_.residentText = {.begin = REC_MAIN_LO, .end = REC_MAIN_HI};
  }

  void *createContext(Core &) override { return nullptr; }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override { return RenderCapabilities::direct(); }
  bool guestVramIsPicture(const Game &) const override { return true; }
  const GuestProgramImage *guestProgramImage() const override { return &image_; }
  const GenericWholeProgramProfile *genericWholeProgramProfile() const override { return &profile_; }

private:
  GenericWholeProgramProfile profile_{};
  GuestProgramImage image_{};
};

const RecompRegistry kProgram{
    .main_dispatch = main_dispatch,
    .rec_func_index = rec_func_index,
    .overlays = g_rec_overlays,
    .overlay_count = g_rec_overlay_count,
    .shard_set_override = shard_set_override,
    .ov_a00_set_override = nullptr,
    .ov_game_set_override = nullptr,
    .guestMemset_gen = nullptr,
    .shard_get_override = shard_get_override,
    .substrate_id = g_rec_substrate_id,
    .wholeProgram = &g_rec_whole_program,
};

void printUsage() {
  std::puts("Usage: {slug}_port EXTRACTED-PS-X-EXE\n"
            "Run the bare generic psxport profile over an extracted executable.");
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && (std::string_view(argv[1]) == "-h" || std::string_view(argv[1]) == "--help")) {
    printUsage();
    return 0;
  }
  if (argc != 2 || !std::filesystem::is_regular_file(argv[1])) {
    printUsage();
    return 2;
  }

  static BareRuntime runtime;
  psxport_install_game(runtime);
  psxport_install_recomp(&kProgram);
  auto game = std::make_unique<Game>();
  game->disc.env_key = kDiscEnvironment;
  Core *core = &game->core;
  load_exe(argv[1], core);

  gte_init();
  mdec_init();
  spu_init();
  watchdog_init();
  gte_bind(core);
  core->rsub.projprim.bind(core);
  spu_bind(core);
  mdec_bind(core);
  xa_bind(core);
  game->spu_audio.init();
  game->gpu.gpu_native_init();
  game->cd.overridesInit();
  game->pad.overridesInit();
  runtime.registerOverrides(*game);
  render_path_install(core);

  FrameLoopShell shell;
  shell.prepareProduct(*game);
  for (std::uint32_t frame = 0;; ++frame) {
    shell.step(*core, frame);
  }
}
'''

README = """# {title} — bare PSX port

This repository is an intentionally minimal psxport consumer. `./run.sh --disc /path/to/disc.chd`
discovers the disc-declared boot executable, extracts it, emits its recompiled substrate, and runs
the psxport generic whole-program profile. It has no title-specific enhancements or native ownership.
The generic profile preserves the generated stack across its discovered VSync boundary; it does not
restart guest main or substitute a title frame loop.
"""

UV_LOCK = """version = 1
revision = 3
requires-python = \">=3.11\"

[[package]]
name = \"{slug}-port\"
version = \"0.0.0\"
source = {{ virtual = \".\" }}
"""


def write(path: Path, text: str, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if executable:
        path.chmod(path.stat().st_mode | 0o111)


def validate(spec: Scaffold) -> None:
    if not spec.title.strip():
        raise Refusal("title must not be empty")
    if not SLUG.fullmatch(spec.slug):
        raise Refusal("slug must be lowercase letters, digits, and hyphens, starting with a letter")
    if spec.output.exists():
        raise Refusal(f"refusing to overwrite existing path: {spec.output}")
    if not re.fullmatch(r"[0-9a-f]{40}", spec.pin):
        raise Refusal("pin must be a full 40-character commit id")
    if spec.disc is not None and not spec.disc.is_file():
        raise Refusal(f"disc is not a regular file: {spec.disc}")


def create(spec: Scaffold) -> None:
    validate(spec)
    values = {"title": spec.title, "slug": spec.slug}
    write(spec.output / "run.sh", RUN_SH, executable=True)
    write(spec.output / "bootstrap.py", BOOTSTRAP.format(**values))
    write(spec.output / "CMakeLists.txt", CMAKE.format(**values))
    write(spec.output / "README.md", README.format(**values))
    write(spec.output / "AGENTS.md", "# " + spec.title + "\n\nBare psxport consumer; game behavior belongs in future title-owned modules.\n")
    write(spec.output / "game" / "app" / "main.cpp", PLAYER_MAIN.replace("{slug}", spec.slug))
    write(spec.output / "pyproject.toml", "[project]\nname = \"" + spec.slug + "-port\"\nversion = \"0.0.0\"\nrequires-python = \">=3.11\"\n")
    write(spec.output / "uv.lock", UV_LOCK.format(slug=spec.slug))
    write(spec.output / ".gitignore", ".env\n.venv/\n__pycache__/\nbuild/\nscratch/\ngenerated/\nexternal/\n*.chd\n")
    write(spec.output / "psxport.pin", "url = https://github.com/SomeoneIsWorking/psxport.git\ncommit = " + spec.pin + "\n")
    for name, body in {
        "codemap.md": "# Codemap\n\n- `bootstrap.py`: player provisioning and substrate emission.\n",
        "project-goals.md": "# Project goals\n\n- Reach a faithful runnable base before title enhancements.\n",
        "project-state.md": "# Project state\n\n- Bare generic execution: the shared whole-program profile runs the emitted boot executable without title enhancements.\n",
        "re-frontier.md": "# RE frontier\n\n- Replace generic frame ownership only when title-specific evidence requires it.\n",
    }.items():
        write(spec.output / "docs" / name, body)
    if spec.disc:
        write(spec.output / ".env", "PSXPORT_DISC=" + str(spec.disc.resolve()) + "\n")


def current_framework_pin() -> str:
    root = Path(__file__).resolve().parents[1]
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True, check=False
    )
    revision = completed.stdout.strip()
    if completed.returncode or not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise Refusal("--pin is required when psx_port_scaffold.py is not run from a git checkout")
    return revision


def selftest() -> None:
    scratch = Path(__file__).resolve().parents[1] / "scratch"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="psx_port_scaffold_", dir=scratch) as temporary:
        root = Path(temporary)
        disc = root / "sample.chd"
        disc.write_bytes(b"fixture")
        result = root / "example"
        create(Scaffold("Example", "example", result, "a" * 40, disc))
        expected = {"run.sh", "bootstrap.py", "CMakeLists.txt", "README.md", "uv.lock", "docs/project-state.md", "game/app/main.cpp"}
        present = {path.relative_to(result).as_posix() for path in result.rglob("*") if path.is_file()}
        assert expected <= present, (expected, present)
        assert str(disc.resolve()) not in (result / "README.md").read_text(encoding="utf-8")
        assert "exec uv run --frozen python bootstrap.py \"$@\"" in (result / "run.sh").read_text(encoding="utf-8")
        assert 'GENERATED_SOURCE = ROOT / "generated" / "recompiled.c"' in (
            result / "bootstrap.py"
        ).read_text(encoding="utf-8")
        assert '"--whole-program"' in (result / "bootstrap.py").read_text(encoding="utf-8")
        assert "generic resumable execution profile" not in (result / "bootstrap.py").read_text(encoding="utf-8")
        assert "generic whole-program profile" in (result / "README.md").read_text(encoding="utf-8")
        assert 'environment.setdefault("PSXPORT_ASSET_DIR", str(FRAMEWORK))' in (
            result / "bootstrap.py"
        ).read_text(encoding="utf-8")
        cmake = (result / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "generated/rec_sources.cmake" in cmake
        assert "add_executable(example_port game/app/main.cpp ${GEN_REC_SRCS})" in cmake
        assert "psxport_resolved.txt" in cmake
        assert str(disc.resolve()) not in "\n".join(
            path.read_text(encoding="utf-8") for path in result.rglob("*") if path.is_file() and path.name != ".env"
        )
        try:
            create(Scaffold("Bad", "BAD", root / "bad", "a" * 40))
        except Refusal:
            return
        raise AssertionError("invalid slug was accepted")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title")
    parser.add_argument("--slug")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--disc", type=Path)
    parser.add_argument("--pin", help="immutable psxport commit; defaults to this framework checkout's HEAD")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        selftest()
        print("psx_port_scaffold selftest: PASS — portable bare-player template, emitted-source CMake, disc-path isolation")
        return 0
    if not args.title or not args.slug or args.output is None:
        parser.error("--title, --slug, and --output are required")
    pin = args.pin or current_framework_pin()
    create(Scaffold(args.title, args.slug, args.output.resolve(), pin, args.disc.resolve() if args.disc else None))
    print(f"psx_port_scaffold: created {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refusal as error:
        print(f"psx_port_scaffold: REFUSED: {{error}}")
        raise SystemExit(2)

"""Shared configure/build/test/execution-boundary verifier for PSXPort consumers."""

from __future__ import annotations

import os
import sys
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from automation.process import ToolError, run
from project import lightrec_cmake_definitions, lightning_cmake_definitions


RunCommand = Callable[..., object]


@dataclass(frozen=True)
class ConsumerVerifyConfig:
    """Title-owned inputs to the shared, title-neutral verification sequence."""

    name: str
    root: Path
    build: Path
    psxport: Path
    product: Path
    cmake_module: Path
    test_regex: str
    cmake_definitions: tuple[str, ...] = ()
    build_targets: tuple[str, ...] = ()
    python: Path = field(default_factory=lambda: Path(sys.executable))


class ConsumerVerifier:
    """Own the invariant consumer verification sequence with an injectable process boundary."""

    def __init__(self, config: ConsumerVerifyConfig, runner: RunCommand = run) -> None:
        self.config = config
        self.runner = runner

    def verify(self, environment: Mapping[str, str] | None = None) -> None:
        config = self._validated_config()
        process_environment = dict(os.environ if environment is None else environment)
        process_environment["PSXPORT_DIR"] = str(config.psxport)

        configure = [
            "cmake",
            "-S",
            config.root,
            "-B",
            config.build,
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_CXX_COMPILER=clang++",
            f"-DPython3_EXECUTABLE={config.python}",
            f"-DPSXPORT_DIR={config.psxport}",
            *lightrec_cmake_definitions(process_environment),
            *lightning_cmake_definitions(process_environment),
            *config.cmake_definitions,
        ]
        self._run(configure, process_environment)
        self._run(["cmake", "--build", config.build], process_environment)
        for target in config.build_targets:
            self._run(["cmake", "--build", config.build, "--target", target], process_environment)
        self._run(
            ["ctest", "--test-dir", config.build, "--output-on-failure", "-R", config.test_regex],
            process_environment,
        )

        checker = config.psxport / "tools" / "check_execution_boundary.py"
        self._run([config.python, checker, "--selftest"], process_environment)
        self._run(
            [
                config.python,
                checker,
                "--root",
                config.root,
                "--cmake",
                config.cmake_module,
                "--binary",
                config.product,
            ],
            process_environment,
        )

    def _validated_config(self) -> ConsumerVerifyConfig:
        config = self.config
        if not config.name.strip():
            raise ToolError("consumer verifier requires a non-empty product name")
        root = config.root.resolve()
        build = config.build.resolve()
        psxport = config.psxport.resolve()
        product = config.product.resolve()
        cmake_module = config.cmake_module.resolve()
        expected_build_root = root / "build"
        if not root.is_dir():
            raise ToolError(f"consumer repository root is missing: {root}")
        if not build.is_relative_to(expected_build_root):
            raise ToolError(f"consumer build must stay under {expected_build_root}: {build}")
        if not product.is_relative_to(build):
            raise ToolError(f"consumer product must stay under its build tree {build}: {product}")
        for required in (
            psxport / "cmake" / "psxport.cmake",
            psxport / "tools" / "check_execution_boundary.py",
            cmake_module,
        ):
            if not required.is_file():
                raise ToolError(f"consumer verifier required file is missing: {required}")
        if not config.test_regex:
            raise ToolError("consumer verifier refuses an empty CTest regex")
        if any(not definition.startswith("-D") for definition in config.cmake_definitions):
            raise ToolError("consumer CMake definitions must use explicit -DNAME=VALUE spelling")
        if any(not target.strip() for target in config.build_targets):
            raise ToolError("consumer build target names must not be empty")
        return ConsumerVerifyConfig(
            name=config.name,
            root=root,
            build=build,
            psxport=psxport,
            product=product,
            cmake_module=cmake_module,
            test_regex=config.test_regex,
            cmake_definitions=config.cmake_definitions,
            build_targets=config.build_targets,
            python=config.python.resolve(),
        )

    def _run(self, command: Sequence[object], environment: Mapping[str, str]) -> None:
        printable = [str(argument) for argument in command]
        print(f"[verify] {' '.join(printable)}", flush=True)
        self.runner(printable, cwd=self.config.root.resolve(), environment=environment)


def run_consumer_verification(config: ConsumerVerifyConfig) -> int:
    """Run the complete shared sequence and return one stable CLI exit status."""

    try:
        ConsumerVerifier(config).verify()
    except ToolError as error:
        print(f"[verify] FAILED: {error}", file=sys.stderr)
        return 1
    print(
        f"[verify] PASS: {config.name} asset-free product, title contracts, and execution boundary"
    )
    return 0

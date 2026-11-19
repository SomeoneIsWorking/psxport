"""psxport's release platforms: the shared target-platform provisioning under psxport's dependency root."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path

from automation.process import ToolError
from target_platform import provision as target_platform

PLATFORMS = target_platform.PLATFORMS


def dependency_root(environment: Mapping[str, str]) -> Path:
    """The same root cmake/dependency_prefix.cmake resolves (PSXPORT_DEPENDENCY_ROOT, else the user cache)."""
    return target_platform.dependency_root(environment, "psxport")


def definitions(platform: str, environment: Mapping[str, str]) -> list[str]:
    """The platform's CMake definitions, with the prefix psxport_presentation_dependencies.cmake reads."""
    try:
        provisioned = target_platform.provision(platform, dependency_root(environment), environment)
    except target_platform.ToolError as error:
        raise ToolError(str(error)) from error
    return [*provisioned.definitions, f"-DPSXPORT_TARGET_DEPENDENCY_PREFIX={provisioned.prefix}"]

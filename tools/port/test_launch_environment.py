#!/usr/bin/env python3
"""Hermetic tests for the shared player/agent launch-environment policy."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from launch_environment import (
    AGENT_RUNTIME_KEYS,
    agent_environment,
    player_environment,
)


class LaunchEnvironmentTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        self.settings = directory / "shipping.ini"
        self.settings.write_text("aspect=3\nfps60=1\n")

    def test_player_strips_ambient_agent_policy_and_preserves_unrelated_values(self) -> None:
        source = {
            "PSXPORT_VK_HEADLESS": "1",
            "PSXPORT_NOAUDIO": "1",
            "PSXPORT_NOPACE": "1",
            "PSXPORT_NOWINDOW": "1",
            "PSXPORT_HEADLESS": "1",
            "PSXPORT_DEBUG": "frame",
        }

        result = player_environment(source)

        self.assertEqual(result["PSXPORT_VK_WINDOW"], "1")
        self.assertEqual(result["PSXPORT_DEBUG"], "frame")
        for key in (*AGENT_RUNTIME_KEYS, "PSXPORT_NOWINDOW", "PSXPORT_HEADLESS"):
            self.assertNotIn(key, result)
        self.assertNotIn("PSXPORT_VK_WINDOW", source)

    def test_player_overrides_a_false_window_value_without_inventing_other_knobs(self) -> None:
        result = player_environment({"PSXPORT_VK_WINDOW": "0", "KEEP": "yes"})

        self.assertEqual(result, {"PSXPORT_VK_WINDOW": "1", "KEEP": "yes"})

    def test_agent_forces_all_three_agent_knobs_and_removes_window_policy(self) -> None:
        source = {
            "PSXPORT_VK_WINDOW": "1",
            "PSXPORT_VK_HEADLESS": "0",
            "PSXPORT_NOAUDIO": "0",
            "PSXPORT_NOPACE": "0",
            "KEEP": "yes",
        }

        result = agent_environment(source, self.settings)

        self.assertNotIn("PSXPORT_VK_WINDOW", result)
        self.assertEqual(result["KEEP"], "yes")
        for key in AGENT_RUNTIME_KEYS:
            self.assertEqual(result[key], "1")
        self.assertEqual(source["PSXPORT_VK_WINDOW"], "1")

    def test_agent_does_not_retain_legacy_headless_selectors(self) -> None:
        result = agent_environment(
            {"PSXPORT_NOWINDOW": "1", "PSXPORT_HEADLESS": "1"}, self.settings
        )

        self.assertNotIn("PSXPORT_NOWINDOW", result)
        self.assertNotIn("PSXPORT_HEADLESS", result)


class AgentSettingsPolicyTests(unittest.TestCase):
    """An agent run has to say what configuration it gated.

    PSXPORT_SETTINGS overrides the product's own working-directory discovery, so an unset variable
    is not "defaults" -- it is "whatever untracked file is lying beside the binary". That is how
    Spyro's oracle comparisons came to run enhanced by accident while recording nothing about it.
    """

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def test_refuses_when_no_settings_are_declared_anywhere(self) -> None:
        with self.assertRaises(ValueError) as raised:
            agent_environment({})
        self.assertIn("working-directory discovery", str(raised.exception))

    def test_refuses_a_named_file_that_does_not_exist(self) -> None:
        missing = self.directory / "absent.ini"
        with self.assertRaises(FileNotFoundError) as raised:
            agent_environment({}, missing)
        self.assertIn("built-in defaults", str(raised.exception))

    def test_an_inherited_environment_value_counts_but_is_still_checked(self) -> None:
        settings = self.directory / "inherited.ini"
        settings.write_text("fps60=0\n")
        result = agent_environment({"PSXPORT_SETTINGS": str(settings)})
        self.assertEqual(result["PSXPORT_SETTINGS"], str(settings.resolve()))
        with self.assertRaises(FileNotFoundError):
            agent_environment({"PSXPORT_SETTINGS": str(self.directory / "gone.ini")})

    def test_the_argument_wins_over_an_inherited_value_and_is_absolute(self) -> None:
        chosen = self.directory / "chosen.ini"
        chosen.write_text("aspect=0\n")
        other = self.directory / "other.ini"
        other.write_text("aspect=3\n")
        result = agent_environment({"PSXPORT_SETTINGS": str(other)}, chosen)
        self.assertEqual(result["PSXPORT_SETTINGS"], str(chosen.resolve()))


if __name__ == "__main__":
    unittest.main()

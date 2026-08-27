#!/usr/bin/env python3
"""Hermetic tests for the shared player/agent launch-environment policy."""

from __future__ import annotations

import unittest

from launch_environment import (
    AGENT_RUNTIME_KEYS,
    agent_environment,
    player_environment,
)


class LaunchEnvironmentTests(unittest.TestCase):
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

        result = agent_environment(source)

        self.assertNotIn("PSXPORT_VK_WINDOW", result)
        self.assertEqual(result["KEEP"], "yes")
        for key in AGENT_RUNTIME_KEYS:
            self.assertEqual(result[key], "1")
        self.assertEqual(source["PSXPORT_VK_WINDOW"], "1")

    def test_agent_does_not_retain_legacy_headless_selectors(self) -> None:
        result = agent_environment(
            {"PSXPORT_NOWINDOW": "1", "PSXPORT_HEADLESS": "1"}
        )

        self.assertNotIn("PSXPORT_NOWINDOW", result)
        self.assertNotIn("PSXPORT_HEADLESS", result)


if __name__ == "__main__":
    unittest.main()

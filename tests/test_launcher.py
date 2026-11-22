import importlib.util
import os
from pathlib import Path
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("long_virtual_camera", ROOT / "long_virtual_camera.py")
LAUNCHER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(LAUNCHER)


class LauncherTests(unittest.TestCase):
    def test_stable_is_always_explicitly_selectable(self):
        engine, entry = LAUNCHER.resolve_engine("stable")
        self.assertEqual(engine, "stable")
        self.assertEqual(entry, ROOT / "desktop" / "main.py")

    def test_auto_falls_back_to_stable_without_windows_binary(self):
        with mock.patch.object(LAUNCHER, "find_enhanced", return_value=None):
            engine, entry = LAUNCHER.resolve_engine("auto")
        self.assertEqual((engine, entry), ("stable", ROOT / "desktop" / "main.py"))

    def test_enhanced_is_not_claimed_on_non_windows(self):
        if os.name == "nt":
            self.skipTest("non-Windows contract")
        with self.assertRaisesRegex(RuntimeError, "Windows"):
            LAUNCHER.resolve_engine("enhanced")

    def test_stable_command_uses_current_python(self):
        command = LAUNCHER.command_for("stable", ROOT / "desktop" / "main.py")
        self.assertEqual(command[0], LAUNCHER.sys.executable)


if __name__ == "__main__":
    unittest.main()

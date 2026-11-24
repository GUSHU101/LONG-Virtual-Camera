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
    def test_launcher_has_no_user_selectable_engine(self):
        source = (ROOT / "long_virtual_camera.py").read_text(encoding="utf-8")
        self.assertNotIn('"--engine"', source)
        self.assertNotIn("choices=(\"auto\", \"stable\", \"enhanced\")", source)

    def test_windows_requires_the_complete_native_product(self):
        with mock.patch.object(LAUNCHER.os, "name", "nt"), \
             mock.patch.object(LAUNCHER, "find_native", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "build.bat"):
                LAUNCHER.resolve_entry()

    def test_windows_resolves_one_native_entry(self):
        expected = ROOT / "LONG-Virtual-Camera.exe"
        with mock.patch.object(LAUNCHER.os, "name", "nt"), \
             mock.patch.object(LAUNCHER, "find_native", return_value=expected):
            self.assertEqual(LAUNCHER.resolve_entry(), expected)

    def test_non_windows_does_not_select_a_second_runtime(self):
        platform = "posix" if os.name == "nt" else os.name
        with mock.patch.object(LAUNCHER.os, "name", platform):
            with self.assertRaisesRegex(RuntimeError, "Windows"):
                LAUNCHER.resolve_entry()

    def test_native_entry_executes_directly(self):
        entry = ROOT / "LONG-Virtual-Camera.exe"
        self.assertEqual(LAUNCHER.command_for(entry), [str(entry)])


if __name__ == "__main__":
    unittest.main()

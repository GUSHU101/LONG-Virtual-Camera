import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "desktop"))

from recam_ui.lifecycle import RuntimeShutdownError, shutdown_runtime  # noqa: E402


class _Recorder:
    def __init__(self, failures=()):
        self.calls = []
        self.failures = set(failures)

    def call(self, name):
        self.calls.append(name)
        if name in self.failures:
            raise RuntimeError(f"{name} failed")


class _Streamer:
    def __init__(self, recorder):
        self.recorder = recorder

    def stop(self):
        self.recorder.call("streamer")


class _Device:
    def __init__(self, recorder):
        self.recorder = recorder

    def stop_feeder(self):
        self.recorder.call("feeder")

    def stop_payload(self):
        self.recorder.call("payload")

    def remove_forward(self, port):
        self.recorder.call(f"forward:{port}")


class LifecycleTests(unittest.TestCase):
    def test_shutdown_order_restores_camera_before_removing_route(self):
        recorder = _Recorder()
        shutdown_runtime(_Streamer(recorder), _Device(recorder), 27183)
        self.assertEqual(
            ["streamer", "feeder", "payload", "forward:27183"], recorder.calls
        )

    def test_every_cleanup_step_runs_after_multiple_failures(self):
        recorder = _Recorder({"streamer", "feeder", "payload"})
        with self.assertRaises(RuntimeShutdownError) as caught:
            shutdown_runtime(_Streamer(recorder), _Device(recorder), 27183)
        self.assertEqual(
            ["streamer", "feeder", "payload", "forward:27183"], recorder.calls
        )
        self.assertEqual(3, len(caught.exception.failures))


if __name__ == "__main__":
    unittest.main()

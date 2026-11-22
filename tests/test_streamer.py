import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "desktop"))

from recam_ui.streamer import redact_command, rtmp_input  # noqa: E402


class StreamerSafetyTests(unittest.TestCase):
    def test_rtmp_listener_is_loopback_only(self):
        args = rtmp_input()
        self.assertIn("rtmp://127.0.0.1:1935/live/recam", args)
        self.assertNotIn("0.0.0.0", " ".join(args))

    def test_url_credentials_path_query_and_fragment_are_redacted(self):
        safe = redact_command([
            "ffmpeg",
            "-i",
            "https://alice:secret@example.test/private/camera.m3u8?token=abc#frag",
        ])
        rendered = " ".join(safe)
        self.assertEqual("ffmpeg -i https://example.test/…", rendered)
        for secret in ("alice", "secret", "private", "camera.m3u8", "token", "abc", "frag"):
            self.assertNotIn(secret, rendered)

    def test_secret_bearing_option_value_is_redacted(self):
        safe = redact_command([
            "ffmpeg", "-headers", "Authorization: Bearer top-secret", "-i", "pipe:0",
        ])
        self.assertEqual(["ffmpeg", "-headers", "<redacted>", "-i", "pipe:0"], safe)

    def test_local_paths_and_regular_flags_remain_diagnostic(self):
        argv = ["ffmpeg", "-re", "-i", "C:/media/demo clip.mp4"]
        self.assertEqual(argv, redact_command(argv))


if __name__ == "__main__":
    unittest.main()

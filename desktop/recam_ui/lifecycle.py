"""Shared, fail-safe shutdown for the desktop and device runtime."""

import logging


class RuntimeShutdownError(RuntimeError):
    """All shutdown steps were attempted, but one or more failed."""

    def __init__(self, failures):
        self.failures = tuple(failures)
        detail = "; ".join(f"{name}: {error}" for name, error in self.failures)
        super().__init__(f"runtime cleanup incomplete: {detail}")


def shutdown_runtime(streamer, device, port):
    """Stop every runtime component, continuing after individual failures.

    Ordering prevents new frames first, then removes the injected payload, and
    finally tears down the desktop-to-device route.  This function is shared by
    the Stop button and window-close path so closing the GUI has the same camera
    restoration semantics as an explicit Stop.
    """

    steps = [
        ("desktop streamer", streamer.stop),
        ("device feeder", device.stop_feeder),
        ("camera payload", device.stop_payload),
        ("ADB forward", lambda: device.remove_forward(port)),
    ]
    failures = []
    for name, operation in steps:
        try:
            operation()
        except Exception as exc:  # cleanup must continue after a failed step
            logging.getLogger("recam").exception("Failed to stop %s", name)
            failures.append((name, exc))
    if failures:
        raise RuntimeShutdownError(failures)

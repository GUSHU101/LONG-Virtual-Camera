#!/usr/bin/env python3
"""Single source-tree entry point for LONG Virtual Camera.

There is deliberately no user-selectable engine switch or fallback runtime.
The product is the complete Windows native host (camera, photo, ReCam-derived
desktop sources and opt-in microphone).
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parent
NATIVE_NAME = "LONG-Virtual-Camera.exe"


def native_candidates() -> list[Path]:
    candidates: list[Path] = []
    configured = os.environ.get("LONGVC_EXE")
    if configured:
        candidates.append(Path(configured).expanduser())
    build_dir = ROOT / "engines" / "camreplace" / "build" / "host-dev-dev" / "bin"
    candidates.append(build_dir / NATIVE_NAME)
    return candidates


def find_native() -> Path | None:
    if os.name != "nt":
        return None
    return next((path.resolve() for path in native_candidates() if path.is_file()), None)


def resolve_entry() -> Path:
    """Resolve the one product entry without selecting a second implementation."""
    if os.name != "nt":
        raise RuntimeError("统一原生工具目前需要 Windows 10/11 x64")
    native = find_native()
    if native is None:
        raise RuntimeError(
            "未找到 LONG-Virtual-Camera.exe。请先运行仓库根目录的 build.bat，"
            "或设置 LONGVC_EXE。"
        )
    return native


def command_for(entry: Path) -> list[str]:
    return [str(entry)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="LONG Virtual Camera 统一启动器")
    parser.add_argument("--check", action="store_true", help="只检查统一程序入口，不启动")
    args = parser.parse_args(argv)

    try:
        entry = resolve_entry()
    except RuntimeError as exc:
        parser.error(str(exc))

    if not entry.is_file():
        parser.error(f"入口不存在：{entry}")
    print(f"LONG Virtual Camera entry={entry}")
    if args.check:
        return 0
    return subprocess.call(command_for(entry), cwd=ROOT)


if __name__ == "__main__":
    raise SystemExit(main())

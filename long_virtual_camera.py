#!/usr/bin/env python3
"""Unified source-tree launcher for LONG Virtual Camera.

The stable engine is available on Windows, Linux and macOS.  The enhanced
engine is a native Windows build that adds opt-in virtual microphone and still
photo replacement.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent
STABLE_ENTRY = ROOT / "desktop" / "main.py"
ENHANCED_NAMES = (
    "LONG-Virtual-Camera-Enhanced.exe",
    "CameraReplace.exe",  # compatibility with an older local build tree
)


def enhanced_candidates() -> list[Path]:
    candidates: list[Path] = []
    configured = os.environ.get("LONGVC_ENHANCED_EXE")
    if configured:
        candidates.append(Path(configured).expanduser())
    build_dir = ROOT / "engines" / "camreplace" / "build" / "host-dev-dev" / "bin"
    candidates.extend(build_dir / name for name in ENHANCED_NAMES)
    return candidates


def find_enhanced() -> Path | None:
    if os.name != "nt":
        return None
    return next((path.resolve() for path in enhanced_candidates() if path.is_file()), None)


def resolve_engine(requested: str) -> tuple[str, Path]:
    enhanced = find_enhanced()
    if requested == "enhanced":
        if os.name != "nt":
            raise RuntimeError("增强引擎仅支持 Windows x64")
        if enhanced is None:
            raise RuntimeError(
                "未找到增强引擎。请先运行 engines\\camreplace\\build.bat，"
                "或设置 LONGVC_ENHANCED_EXE。"
            )
        return "enhanced", enhanced
    if requested == "stable":
        return "stable", STABLE_ENTRY
    if enhanced is not None:
        return "enhanced", enhanced
    return "stable", STABLE_ENTRY


def command_for(engine: str, entry: Path) -> list[str]:
    if engine == "enhanced":
        return [str(entry)]
    return [sys.executable, str(entry)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="LONG Virtual Camera 统一启动器")
    parser.add_argument(
        "--engine",
        choices=("auto", "stable", "enhanced"),
        default="auto",
        help="auto 在 Windows 上优先使用已构建的增强引擎，否则使用稳定引擎",
    )
    parser.add_argument("--check", action="store_true", help="只显示将使用的引擎，不启动")
    args = parser.parse_args(argv)

    try:
        engine, entry = resolve_engine(args.engine)
    except RuntimeError as exc:
        parser.error(str(exc))

    if not entry.is_file():
        parser.error(f"入口不存在：{entry}")
    print(f"LONG Virtual Camera engine={engine} entry={entry}")
    if args.check:
        return 0
    return subprocess.call(command_for(engine, entry), cwd=ROOT)


if __name__ == "__main__":
    raise SystemExit(main())

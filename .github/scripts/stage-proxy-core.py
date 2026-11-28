"""Stage the pinned official Android ARM64 build of sing-box."""

from hashlib import sha256
from pathlib import Path
import shutil
import sys
import tarfile
from tempfile import NamedTemporaryFile
from urllib.request import Request, urlopen

VERSION = "1.14.6"
NAME = f"sing-box-{VERSION}-android-arm64"
ARCHIVE_SHA256 = "9546a19b387173321b86a165099bed3f8d0f77302c1339e4dfd01e3fed9387f6"
GPL = Path(__file__).resolve().parents[2] / "LICENSE"


def stage(cache: Path, dest: Path) -> None:
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"{NAME}.tar.gz"
    if not archive.is_file():
        url = f"https://github.com/SagerNet/sing-box/releases/download/v{VERSION}/{archive.name}"
        request = Request(url, headers={"User-Agent": "LONG-Virtual-Camera-build"})
        staging = NamedTemporaryFile(dir=cache, prefix="sing-box-", suffix=".partial", delete=False)
        partial = Path(staging.name)
        try:
            with staging, urlopen(request, timeout=60) as response:
                shutil.copyfileobj(response, staging)
            if sha256(partial.read_bytes()).hexdigest() != ARCHIVE_SHA256:
                raise ValueError("Official sing-box archive SHA-256 mismatch")
            try:
                partial.replace(archive)
            except PermissionError:
                if not archive.is_file():
                    raise
        finally:
            partial.unlink(missing_ok=True)
    if sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise ValueError(f"Invalid sing-box cache: {archive}")
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as bundle:
        binary = bundle.extractfile(f"{NAME}/sing-box").read()
        if binary[:6] != b"\x7fELF\x02\x01" or binary[18:20] != b"\xb7\x00":
            raise ValueError("sing-box is not a little-endian ARM64 ELF")
        (dest / "sing-box").write_bytes(binary)
        (dest / "sing-box.sha256").write_text(sha256(binary).hexdigest() + "\n", encoding="ascii")
        notice = bundle.extractfile(f"{NAME}/LICENSE").read()
        (dest / "LICENSE").write_bytes(notice + b"\n\n" + GPL.read_bytes())
        (dest / "SOURCE").write_text(
            f"https://github.com/SagerNet/sing-box/tree/v{VERSION}\n"
            f"https://github.com/SagerNet/sing-box/releases/tag/v{VERSION}\n",
            encoding="ascii",
        )


if __name__ == "__main__":
    stage(Path(sys.argv[1]), Path(sys.argv[2]))

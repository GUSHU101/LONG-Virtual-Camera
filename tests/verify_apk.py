"""Verify that the built APK contains the complete, hashed ARM64 payload."""

from hashlib import sha256
from pathlib import Path
import sys
from zipfile import ZipFile


def verify(apk: Path) -> None:
    prefix = "assets/payload/arm64-v8a/"
    binaries = {"cr_injector", "cr_feed_proc", "libcr_hooks.so"}
    expected = binaries | {"longvc-control.sh"}
    with ZipFile(apk) as archive:
        present = [name.removeprefix(prefix) for name in archive.namelist()
                   if name.startswith(prefix) and not name.endswith("/")]
        if sorted(present) != sorted(expected | {"payload.sha256"}):
            raise ValueError("APK payload has missing, duplicate or unexpected files")
        scope = archive.read("META-INF/xposed/scope.list").decode().split()
        if scope != ["system"]:
            raise ValueError("APK scope is not system-only")
        properties = archive.read("META-INF/xposed/module.prop").decode().splitlines()
        if "staticScope=true" not in properties:
            raise ValueError("APK does not declare a static LSPosed scope")
        records = archive.read(prefix + "payload.sha256").decode().splitlines()
        recorded = set()
        for line in records:
            digest, size, name = line.split()
            if name not in expected or name in recorded:
                raise ValueError(f"Unexpected manifest entry {name!r}")
            data = archive.read(prefix + name)
            if not data or len(data) != int(size) or sha256(data).hexdigest() != digest:
                raise ValueError(f"Size or hash mismatch for {name}")
            if name in binaries and (data[:6] != b"\x7fELF\x02\x01"
                                     or data[18:20] != b"\xb7\x00"):
                raise ValueError(f"{name} is not a little-endian ARM64 ELF binary")
            if name == "longvc-control.sh" and b"\r" in data:
                raise ValueError("Controller script must use LF line endings")
            recorded.add(name)
        if recorded != expected:
            raise ValueError("The APK payload manifest is incomplete")
        proxy = "assets/proxy/arm64-v8a/"
        files = sorted(name.removeprefix(proxy) for name in archive.namelist()
                       if name.startswith(proxy) and not name.endswith("/"))
        if files != ["LICENSE", "SOURCE", "longvc-proxy.sh", "sing-box", "sing-box.sha256"]:
            raise ValueError("APK proxy payload has missing, duplicate or unexpected files")
        core = archive.read(proxy + "sing-box")
        if core[:6] != b"\x7fELF\x02\x01" or core[18:20] != b"\xb7\x00":
            raise ValueError("sing-box is not a little-endian ARM64 ELF binary")
        if sha256(core).hexdigest() != archive.read(proxy + "sing-box.sha256").decode().strip():
            raise ValueError("sing-box hash mismatch")
        launcher = archive.read(proxy + "longvc-proxy.sh")
        if not launcher.startswith(b"#!/system/bin/sh\n") or b"\r" in launcher:
            raise ValueError("Proxy controller script must use LF line endings")
        if b"GNU GENERAL PUBLIC LICENSE" not in archive.read(proxy + "LICENSE"):
            raise ValueError("Missing sing-box license")
        if b"https://github.com/SagerNet/sing-box/tree/" not in archive.read(proxy + "SOURCE"):
            raise ValueError("Missing sing-box source information")


if __name__ == "__main__":
    verify(Path(sys.argv[1]))
    print("APK scope, camera and proxy ARM64 payloads, and SHA-256 manifests verified")

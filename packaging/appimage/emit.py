#!/usr/bin/env python3
"""Create a type-2 AppImage without requiring FUSE in the builder."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

appdir = Path("/package/AppDir")
version = (appdir / "version").read_text().strip()
output = Path("/output") / f"GitHubCommitManager-{version}-x86_64.AppImage"
with tempfile.TemporaryDirectory(prefix="ghm-appimage-") as temporary:
    squashfs = Path(temporary) / "payload.squashfs"
    subprocess.check_call([
        "mksquashfs", str(appdir), str(squashfs), "-root-owned", "-noappend",
        "-comp", "zstd", "-processors", "2"
    ])
    with output.open("wb") as stream:
        for source in [Path("/package/runtime-x86_64"), squashfs]:
            with source.open("rb") as payload:
                shutil.copyfileobj(payload, stream)
output.chmod(0o755)
digest = hashlib.file_digest(output.open("rb"), "sha256").hexdigest()
output.with_suffix(output.suffix + ".sha256").write_text(f"{digest}  {output.name}\n")
print(f"Created {output} ({output.stat().st_size // 1024 // 1024} MiB)")

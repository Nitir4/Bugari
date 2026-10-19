#!/usr/bin/env python3
"""Create a deterministic release snapshot and rendered distro recipes."""
import argparse
import datetime
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile


def render(template, values):
    for key, value in values.items():
        template = template.replace(f"@{key}@", value)
    if re.search(r"@[A-Z_0-9]+@", template):
        raise ValueError("Unresolved packaging template field")
    return template


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--local-only", action="store_true", help="Allow an unlicensed snapshot solely for local package testing")
    parser.add_argument("--binary", type=Path, help="Render the AUR binary recipe for this payload")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.binary:
        metadata = json.loads((out / "RELEASE.json").read_text())
        values = metadata["template_values"]
        values["BINARY_SHA256"] = digest(args.binary)
        target = out / "aur-bin"
        target.mkdir(exist_ok=True)
        (target / "PKGBUILD").write_text(render(Path(__file__).with_name("PKGBUILD-bin.in").read_text(), values))
        return

    root = Path(__file__).resolve().parents[2]
    license_file = root / "LICENSE"
    license_text = license_file.read_text() if license_file.is_file() else ""
    license_id = os.environ.get("GHM_PACKAGE_LICENSE", "")
    if not license_id and (root / "packaging/linux/license.spdx").exists():
        license_id = (root / "packaging/linux/license.spdx").read_text().strip()
    if args.local_only and not license_text:
        license_id = "LicenseRef-Unlicensed"
        license_text = "All rights reserved.\nThis unlicensed local test snapshot grants no redistribution rights.\nThe project owner must choose a license before public distribution.\n"
    if not license_text or not re.fullmatch(r"[A-Za-z0-9.+-]+", license_id):
        parser.error("Choose a license first: add LICENSE and packaging/linux/license.spdx (one SPDX identifier).")
    client_id = os.environ.get("GHM_GITHUB_CLIENT_ID", "")
    if not re.fullmatch(r"[A-Za-z0-9]+", client_id):
        parser.error("Set GHM_GITHUB_CLIENT_ID to the public OAuth device-flow Client ID.")
    version = re.search(r"project\(ghm VERSION ([0-9]+\.[0-9]+\.[0-9]+)", (root / "CMakeLists.txt").read_text()).group(1)
    maintainer = os.environ.get("GHM_PACKAGE_MAINTAINER", "Nitir4 <173669837+Nitir4@users.noreply.github.com>")
    if not re.fullmatch(r"[^\n\r%@<>]+ <[A-Za-z0-9.+_-]+@[A-Za-z0-9.-]+>", maintainer):
        parser.error("GHM_PACKAGE_MAINTAINER must be 'Name <email>' without template or RPM syntax.")
    git = lambda *a: subprocess.check_output(["git", "-C", str(root), *a])
    commit = git("rev-parse", "HEAD").decode().strip()
    timestamp = int(git("show", "-s", "--format=%ct", "HEAD").decode())
    date = datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc)
    values = {
        "VERSION": version, "LICENSE": license_id, "CLIENT_ID": client_id,
        "MAINTAINER": maintainer, "RPM_DATE": date.strftime("%a %b %d %Y"),
    }
    paths = ["CMakeLists.txt", "README.md", "LICENSE", "include", "src", "data", "tests", "packaging"]
    files = set(os.fsdecode(p) for p in git("ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *paths).split(b"\0") if p)
    files.add("LICENSE")
    metadata = {
        "version": version, "commit": commit, "source_date_epoch": timestamp,
        "working_tree_status": git("status", "--short", "--", *paths).decode(),
        "license": license_id, "template_values": values,
        "local_only": args.local_only,
    }
    archive = out / f"ghm-{version}.tar.gz"
    with archive.open("wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=timestamp) as gz, tarfile.open(fileobj=gz, mode="w") as tar:
        for relative in sorted(files):
            source = root / relative
            if relative == "LICENSE" and not source.is_file():
                content = license_text.encode()
                info = tarfile.TarInfo(f"ghm-{version}/LICENSE")
                info.size, info.mtime, info.mode = len(content), timestamp, 0o644
                tar.addfile(info, io.BytesIO(content))
                continue
            if not source.is_file():
                continue
            if source.is_symlink():
                raise ValueError(f"Release source cannot contain symlinks: {relative}")
            info = tar.gettarinfo(str(source), arcname=f"ghm-{version}/{relative}")
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            info.mtime = timestamp
            with source.open("rb") as stream:
                tar.addfile(info, stream)
        content = (json.dumps(metadata, indent=2) + "\n").encode()
        info = tarfile.TarInfo(f"ghm-{version}/RELEASE.json")
        info.size, info.mtime = len(content), timestamp
        info.mode = 0o644
        tar.addfile(info, io.BytesIO(content))
    values["SOURCE_SHA256"] = digest(archive)
    metadata["source_sha256"] = values["SOURCE_SHA256"]
    (out / "RELEASE.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (out / "ghm.spec").write_text(render((root / "packaging/linux/ghm.spec.in").read_text(), values))
    aur = out / "aur"
    aur.mkdir(exist_ok=True)
    (aur / "PKGBUILD").write_text(render((root / "packaging/linux/PKGBUILD.in").read_text(), values))
    debian = out / "debian"
    (debian / "source").mkdir(parents=True, exist_ok=True)
    for name in ("control", "rules"):
        (debian / name).write_text(render((root / f"packaging/linux/debian/{name}").read_text(), values))
    (debian / "rules").chmod(0o755)
    (debian / "source/format").write_text("3.0 (quilt)\n")
    (debian / "changelog").write_text(
        f"ghm ({version}-1) unstable; urgency=medium\n\n"
        "  * Initial native package.\n\n"
        f" -- {maintainer}  {date.strftime('%a, %d %b %Y %H:%M:%S +0000')}\n"
    )
    full_license = "\n".join(" " + line if line else " ." for line in license_text.splitlines())
    (debian / "copyright").write_text(
        "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n"
        "Upstream-Name: GitHub Commit Manager\nSource: https://github.com/Nitir4/Bugari\n\n"
        f"Files: *\nCopyright: {date.year} Nitir4\nLicense: {license_id}\n{full_license}\n"
    )
    print(f"Prepared ghm {version}; source SHA256 {values['SOURCE_SHA256']}")


if __name__ == "__main__":
    main()

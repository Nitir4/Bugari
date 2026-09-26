#!/usr/bin/env python3
"""Bundle the GTK4 payload without embedding glibc or graphics drivers."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

appdir = Path(sys.argv[1]).resolve()
libdir = appdir / "usr/lib"
libdir.mkdir(parents=True, exist_ok=True)
source_dir = Path(__file__).resolve().parent
excluded = re.compile(
    r"^(ld-linux.*|lib(c|m|pthread|dl|rt|resolv|util|anl)\.so.*|"
    r"libnss_.*|lib(EGL|GL|GLX|GLdispatch|OpenGL|gbm|drm).*\.so.*)$"
)
origins = set()
copied = set()


def run(*args):
    return subprocess.check_output(args, text=True)


def dependencies(path):
    output = run("ldd", str(path))
    if "not found" in output:
        raise RuntimeError(f"Missing library for {path}:\n{output}")
    return [Path(match) for match in re.findall(r"=>\s+(/\S+)", output)]


def add_library(path, destination=None):
    if excluded.match(path.name):
        return
    destination = destination or libdir / path.name
    if destination in copied:
        return
    copied.add(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path.resolve(), destination)
    origins.add(path.resolve())
    for dependency in dependencies(path):
        add_library(dependency)


for binary in (appdir / "usr/bin").iterdir():
    for dependency in dependencies(binary):
        add_library(dependency)

# Modules loaded dynamically do not appear in the executables' DT_NEEDED lists.
multiarch = run("gcc", "-print-multiarch").strip()
system_lib = Path("/usr/lib") / multiarch
for module in (system_lib / "gio/modules").glob("*.so"):
    add_library(module, libdir / "gio/modules" / module.name)
pixbuf_root = system_lib / "gdk-pixbuf-2.0/2.10.0"
for module in (pixbuf_root / "loaders").glob("*.so"):
    add_library(module, libdir / "gdk-pixbuf/loaders" / module.name)
query = pixbuf_root.parent / "gdk-pixbuf-query-loaders"
shutil.copy2(query, appdir / "usr/bin/gdk-pixbuf-query-loaders")
origins.add(query)
for dependency in dependencies(query):
    add_library(dependency)

# Each ELF finds private libraries through its own RPATH. Do not export
# LD_LIBRARY_PATH to the host browser, portal, keyring or other child programs.
for directory in [appdir / "usr/bin", libdir]:
    for binary in directory.rglob("*"):
        if not binary.is_file() or binary.read_bytes()[:4] != b"\x7fELF":
            continue
        relative = os.path.relpath(libdir, binary.parent)
        subprocess.check_call([
            "patchelf", "--force-rpath", "--set-rpath",
            "$ORIGIN" if relative == "." else f"$ORIGIN:$ORIGIN/{relative}", str(binary)
        ])
        versions = re.findall(r"GLIBC_(\d+)\.(\d+)", run("readelf", "--version-info", str(binary)))
        if any((int(major), int(minor)) > (2, 39) for major, minor in versions):
            raise RuntimeError(f"{binary} exceeds the glibc 2.39 baseline")

share = appdir / "usr/share"
schemas = share / "glib-2.0/schemas"
schemas.mkdir(parents=True, exist_ok=True)
for schema in Path("/usr/share/glib-2.0/schemas").glob("org.gtk.*.xml"):
    shutil.copy2(schema, schemas / schema.name)
subprocess.check_call(["glib-compile-schemas", str(schemas)])
for resource in ["icons/Adwaita", "icons/hicolor", "fonts/truetype/dejavu", "mime"]:
    shutil.copytree(Path("/usr/share") / resource, share / resource, dirs_exist_ok=True)
(share / "fontconfig").mkdir(parents=True, exist_ok=True)
shutil.copy2(source_dir / "fontconfig.conf", share / "fontconfig/fonts.conf")

# Bundle notices for the packages providing libraries and runtime resources.
packages = set()
for origin in origins:
    candidates = [str(origin), str(origin).replace("/usr/lib/", "/lib/", 1)]
    for candidate in candidates:
        result = subprocess.run(["dpkg-query", "-S", candidate], capture_output=True, text=True)
        if result.returncode == 0:
            packages.update(line.split(": ", 1)[0] for line in result.stdout.splitlines())
            break
    else:
        raise RuntimeError(f"Cannot identify the license package for {origin}")
packages.update(["adwaita-icon-theme", "hicolor-icon-theme", "fonts-dejavu-core", "libgtk-4-common", "shared-mime-info"])
notices = share / "doc/ghm/third-party"
notices.mkdir(parents=True, exist_ok=True)
for package in sorted(packages):
    name = package.split(":")[0]
    copyright_file = Path("/usr/share/doc") / name / "copyright"
    if not copyright_file.exists():
        raise RuntimeError(f"Missing copyright notice for {package}")
    shutil.copy2(copyright_file, notices / f"{name}.copyright")
metadata = run("dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n", *sorted(packages))
(notices / "packages.tsv").write_text(metadata)
(notices / "README.txt").write_text(
    "Bundled dependency copyright and license notices are included here.\n"
    "Package versions and corresponding source package versions are in packages.tsv.\n"
    "Source archives and patches are available from https://launchpad.net/ubuntu/+source/\n"
    "and the Ubuntu archive source repositories. Rebuild this bundle with the\n"
    "packaging/appimage recipe and relink the application to modified libraries.\n"
    "The AppImage runtime is MIT licensed: https://github.com/AppImage/type2-runtime\n"
)

desktop = appdir / "io.github.ghm.CommitManager.desktop"
desktop.write_text(
    "[Desktop Entry]\nType=Application\nName=GitHub Commit Manager\n"
    "Comment=Manage local Git repositories and GitHub sign-in\n"
    "Exec=ghm-gui\nIcon=io.github.ghm.CommitManager\nTerminal=false\n"
    "Categories=Development;\nKeywords=Git;GitHub;Repository;Commit;\n"
)
shutil.copy2(share / "icons/hicolor/scalable/apps/io.github.ghm.CommitManager.svg",
             appdir / "io.github.ghm.CommitManager.svg")
(appdir / ".DirIcon").symlink_to("io.github.ghm.CommitManager.svg")
shutil.copy2(source_dir / "AppRun", appdir / "AppRun")
(appdir / "AppRun").chmod(0o755)
libexec = appdir / "usr/libexec"
libexec.mkdir(parents=True, exist_ok=True)
shutil.copy2(source_dir / "install.sh", libexec / "ghm-appimage-install")
(libexec / "ghm-appimage-install").chmod(0o755)
# Native-install absolute paths must not escape an ephemeral AppImage mount.
(share / "systemd/user/ghm-worker.service").unlink()
(share / "applications/io.github.ghm.CommitManager.desktop").write_text(desktop.read_text())
version = re.search(r"project\(ghm VERSION ([0-9.]+)", Path("CMakeLists.txt").read_text()).group(1)
(appdir / "version").write_text(version + "\n")
(appdir / "build-info.json").write_text(json.dumps({
    "version": version, "architecture": "x86_64", "glibc_minimum": "2.39",
    "build_distribution": "Ubuntu 24.04", "bundled_packages": sorted(packages)
}, indent=2) + "\n")

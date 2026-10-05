#!/usr/bin/env python3
"""Bundle native UCRT64 executables and their PE imports, not the MSYS runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zipfile

# DLLs supplied by supported Windows installations, never redistributed.
SYSTEM = set('advapi32 bcrypt bcryptprimitives cfgmgr32 comctl32 comdlg32 crypt32 '
             'd3d11 d3d12 dcomp dnsapi dwmapi dxgi gdi32 imm32 iphlpapi kernel32 '
             'msimg32 msvcrt ncrypt netapi32 normaliz ntdll ole32 oleaut32 opengl32 '
             'powrprof propsys psapi rpcrt4 secur32 setupapi shell32 shlwapi ucrtbase '
             'user32 userenv usp10 uxtheme version winhttp wininet winmm winspool gdiplus '
             'wintrust wldap32 ws2_32 wtsapi32 dwrite hid avrt win32u shcore'.split())

def imports(path):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'Not a PE executable: {path}')
    count, optional_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    directory = optional + (112 if magic == 0x20b else 96)
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        virtual_size, rva, raw_size, raw = struct.unpack_from('<IIII', data, offset + 8)
        sections.append((rva, max(virtual_size, raw_size), raw))
    def file_offset(rva):
        for start, size, raw in sections:
            if start <= rva < start + size:
                return raw + rva - start
        raise ValueError(f'Invalid PE RVA in {path}')
    import_rva = struct.unpack_from('<I', data, directory + 8)[0]
    if import_rva:
        cursor = file_offset(import_rva)
        while any(data[cursor:cursor + 20]):
            name_offset = file_offset(struct.unpack_from('<I', data, cursor + 12)[0])
            yield data[name_offset:data.index(0, name_offset)].decode('ascii')
            cursor += 20

def bundle(build, prefix, output, version, inventory=None):
    if output.exists():
        raise ValueError(f'Output already exists; choose an empty directory: {output}')
    binary = output / 'bin'
    binary.mkdir(parents=True)
    candidates = {p.name.lower(): p for p in (prefix / 'bin').glob('*.dll')}
    pending = []
    for name in ['ghm-gui.exe', 'ghm.exe', 'ghm-worker.exe']:
        source = build / name
        shutil.copy2(source, binary / name)
        pending.append(source)
    helpers = list((prefix / 'bin').glob('gspawn*.exe')) + [prefix / 'bin/gdk-pixbuf-query-loaders.exe']
    if len(helpers) < 3:
        raise ValueError('Required GLib process helpers are missing')
    for source in helpers:
        shutil.copy2(source, binary / source.name)
        pending.append(source)
    for relative in ['lib/gio/modules', 'lib/gdk-pixbuf-2.0/2.10.0/loaders']:
        source = prefix / relative
        if source.exists():
            target = output / relative
            target.mkdir(parents=True)
            for library in source.glob('*.dll'):
                shutil.copy2(library, target / library.name)
                pending.append(library)
    bundled = set()
    while pending:
        for name in imports(pending.pop()):
            key = name.lower()
            if key in bundled:
                continue
            source = candidates.get(key)
            if source is None:
                stem = key.removesuffix('.dll')
                if stem in SYSTEM or key == 'winspool.drv' or stem.startswith(('api-ms-win-', 'ext-ms-win-')):
                    continue
                raise ValueError(f'Missing required DLL: {name}')
            if key == 'msys-2.0.dll':
                raise ValueError('The application must not depend on the MSYS runtime')
            shutil.copy2(source, binary / source.name)
            bundled.add(key)
            pending.append(source)
    for relative in ['share/glib-2.0/schemas', 'share/icons/Adwaita', 'share/icons/hicolor',
                     'share/gtk-4.0', 'share/themes', 'etc/fonts', 'share/fontconfig']:
        source = prefix / relative
        if source.exists():
            shutil.copytree(source, output / relative)
    # Keep dependency notices and the package versions for source lookup.
    license_root = prefix / 'share/licenses'
    if not license_root.is_dir():
        raise ValueError('MSYS2 dependency license directory is missing')
    shutil.copytree(license_root, output / 'third-party/licenses')
    if inventory is not None:
        shutil.copy2(inventory, output / 'third-party/packages.txt')
    repo_root = Path(__file__).resolve().parents[2]
    for name in ['install.ps1', 'uninstall.ps1', 'USER-README.txt']:
        shutil.copy2(repo_root / 'packaging/windows' / name, output / name)
    metadata = {'version': version, 'architecture': 'x86_64', 'toolchain': 'UCRT64',
                'dlls': sorted(bundled), 'validation': 'test candidate; desktop checks required'}
    (output / 'build-info.json').write_text(json.dumps(metadata, indent=2) + '\n')
    files = sorted(p for p in output.rglob('*') if p.is_file())
    (output / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.relative_to(output).as_posix() + '\n'
        for p in files))
    archive = Path(str(output) + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zip_file:
        for path in sorted(p for p in output.rglob('*') if p.is_file()):
            zip_file.write(path, Path(output.name) / path.relative_to(output))
    archive.with_suffix('.zip.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
    print(f'Bundled {len(bundled)} DLLs: {archive}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--prefix', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', default='0.1.0')
    parser.add_argument('--inventory', type=Path)
    args = parser.parse_args()
    bundle(args.build.resolve(), args.prefix.resolve(), args.output.resolve(), args.version, args.inventory)

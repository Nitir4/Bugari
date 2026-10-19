#!/bin/sh
set -eu
target=$1
version=$(python3 -c 'import json; print(json.load(open("/input/RELEASE.json"))["version"])')
export SOURCE_DATE_EPOCH=$(python3 -c 'import json; print(json.load(open("/input/RELEASE.json"))["source_date_epoch"])')
mkdir -p /work /artifacts
cd /work
cp "/input/ghm-$version.tar.gz" .
case "$target" in
    fedora)
        mkdir -p /work/rpmbuild/SOURCES /work/rpmbuild/SPECS
        cp "/input/ghm-$version.tar.gz" /work/rpmbuild/SOURCES/
        cp /input/ghm.spec /work/rpmbuild/SPECS/
        chown -R builder:builder /work
        runuser -u builder -- rpmbuild -ba --define '_topdir /work/rpmbuild' /work/rpmbuild/SPECS/ghm.spec
        find /work/rpmbuild/RPMS /work/rpmbuild/SRPMS -name '*.rpm' -exec cp '{}' /artifacts/ \;
        cp /input/ghm.spec /artifacts/
        ;;
    debian)
        cp "/input/ghm-$version.tar.gz" "ghm_${version}.orig.tar.gz"
        tar -xzf "ghm-$version.tar.gz"
        cp -a /input/debian "ghm-$version/"
        cd "ghm-$version"
        chown -R builder:builder /work
        runuser -u builder -- dpkg-buildpackage -us -uc
        cd /work
        for file in ghm_*.deb ghm_*.dsc ghm_*.changes ghm_*.buildinfo ghm_*.tar.*; do
            test ! -f "$file" || cp "$file" /artifacts/
        done
        ;;
    arch)
        cp /input/aur/PKGBUILD .
        chown -R builder:builder /work
        runuser -u builder -- makepkg --noconfirm
        mkdir -p /artifacts/aur /artifacts/aur-bin
        runuser -u builder -- makepkg --printsrcinfo > /artifacts/aur/.SRCINFO
        cp PKGBUILD /artifacts/aur/
        for file in ghm-*.pkg.tar.zst; do cp "$file" /artifacts/; done
        # Ship a distro-native binary payload for the AUR ghm-bin recipe.
        tar -C /work/pkg/ghm -czf "/artifacts/ghm-$version-arch-x86_64.tar.gz" usr
        cp /input/RELEASE.json /artifacts/
        python3 /recipe/prepare.py /artifacts --binary "/artifacts/ghm-$version-arch-x86_64.tar.gz"
        cp /artifacts/aur-bin/PKGBUILD /work/PKGBUILD
        runuser -u builder -- makepkg --printsrcinfo > /artifacts/aur-bin/.SRCINFO
        # Verify the binary recipe against exactly the archive it will download.
        cp "/artifacts/ghm-$version-arch-x86_64.tar.gz" /work/
        chown builder:builder "/work/ghm-$version-arch-x86_64.tar.gz"
        runuser -u builder -- makepkg --noconfirm --cleanbuild
        for file in ghm-bin-*.pkg.tar.zst; do cp "$file" /artifacts/; done
        ;;
    *) exit 2 ;;
esac
cp "/input/ghm-$version.tar.gz" /input/RELEASE.json /artifacts/
cd /artifacts
find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
# Only export artifacts after every build and package check succeeds.
cp -a /artifacts/. /output/
chown -R "${OUTPUT_UID}:${OUTPUT_GID}" /output

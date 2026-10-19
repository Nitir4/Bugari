# Native Linux releases

Build separate x86_64 packages against Fedora 43, Debian 13, and current Arch
libraries. A Fedora binary must not be repackaged as an Arch or Debian binary.
Debian 12's GTK is below the application's required GTK 4.12 baseline.

The native release contains `ghm-gui`, `ghm`, `ghm-worker`, the application menu
entry, icon, documentation, and a systemd user service. Scheduling stays opt-in.
The existing [AppImage](../appimage/README.md) remains a separate portable option.

## Before building

The project owner must choose a license and add its complete text to `LICENSE`
at the repository root, with its SPDX identifier on one line in
`packaging/linux/license.spdx`. The release script deliberately refuses to
invent a license. A license suitable for distribution is also needed for
Fedora COPR and official free software archives.

For local validation before this decision, set `GHM_LOCAL_ONLY=1` and use a
separate output directory, such as `GHM_PACKAGE_OUTPUT_DIR="$PWD/dist/linux-local"`.
These test packages include `LicenseRef-Unlicensed` metadata, grant no
redistribution rights, and are marked `local_only` in `RELEASE.json`. They must
not be published. No project license file is added by this mode.

Install Docker or Podman and Python 3. Set the public GitHub OAuth app Client ID
used by the GUI and CLI. This is not an OAuth client secret. For public releases,
set the maintainer name and an email address where you accept package reports.

```sh
export GHM_GITHUB_CLIENT_ID=YOUR_PUBLIC_OAUTH_CLIENT_ID
export GHM_PACKAGE_MAINTAINER='Your Name <you@example.org>'
sh packaging/linux/build.sh all
```

Use `fedora`, `debian`, or `arch` in place of `all` for one distribution.
`CONTAINER_ENGINE=podman` selects Podman. Set `GHM_PACKAGE_OUTPUT_DIR` to a new,
empty directory for subsequent builds; the script refuses to mix old artifacts
with new ones. Build dependencies download while creating the containers;
compilation and tests then run without network access and as an unprivileged
builder. The host needs an x86_64 CPU; cross-compilation is not implemented.

Outputs go under `dist/linux/<distribution>/`:

| Distribution | Outputs |
| --- | --- |
| Fedora | Binary RPMs, source RPM, rendered spec |
| Debian | `.deb`, `.dsc`, original source, Debian source archive, build information |
| Arch | `.pkg.tar.zst` for `ghm` and `ghm-bin`, native binary payload, `aur/` and `aur-bin/` recipes with `.SRCINFO` |

Every directory also contains the shared `ghm-VERSION.tar.gz` source release,
`RELEASE.json` identifying the commit and local modifications, and `SHA256SUMS`.
Source snapshots include current source changes, including new untracked source
files, and exclude ignored files, build directories, Git metadata, and credentials.
All three builds run the CTest suite and validate the desktop entry before
exporting artifacts. Arch also builds the generated binary recipe against its
actual payload. Display-dependent GUI tests are not part of these builds.

## Installation

Install the main package rather than optional debug-symbol packages:

```sh
# Fedora
sudo dnf install ./dist/linux/fedora/ghm-0.1.0-1.fc43.x86_64.rpm
# Debian 13
sudo apt install ./dist/linux/debian/ghm_0.1.0-1_amd64.deb
# Arch (choose either ghm or ghm-bin)
sudo pacman -U ./dist/linux/arch/ghm-0.1.0-1-x86_64.pkg.tar.zst
```

Launch GitHub Commit Manager from the application menu, or run `ghm --help`.
Enable scheduling for your user only when needed:

```sh
systemctl --user daemon-reload
systemctl --user enable --now ghm-worker.service
```

Saved GitHub login needs a running desktop Secret Service keyring. The packages
do not install or select a keyring provider for your desktop.

## Publishing

Creating packages does not automatically add the app to distribution archives.
The scripts only create local artifacts; they do not push, upload, or submit.
Verify installation with `sh packaging/linux/verify.sh all` before publishing.

For a GitHub release, commit and tag the reviewed source as `vVERSION`, build
from that source, and upload the source tarball, distro binaries, Arch binary
payload, and checksums. The generated AUR recipes use that release URL and
real SHA256 hashes: publish the exact archives they reference before submission.
Current working-tree builds are useful for testing; `RELEASE.json` records their
local modifications so they cannot be mistaken for a clean tagged release.

**Fedora COPR:** Create a Fedora account and COPR project, configure your COPR
token locally, and submit the generated source RPM to selected Fedora chroots.
COPR builds and hosts the repository. Users can then enable `OWNER/ghm` with
`dnf copr enable`. See [COPR's official quick start](https://docs.pagure.org/copr.copr/user_documentation.html#quick-start).
Build for every Fedora release you intend to support; the local recipe verifies
Fedora 43 only. Official Fedora inclusion requires package review and the
[maintainer onboarding process](https://docs.fedoraproject.org/en-US/package-maintainers/Joining_the_Package_Maintainers/).

**Arch AUR:** Register an AUR account and configure an SSH key. Check that `ghm`
and `ghm-bin` are available names before submission. The `aur/` and `aur-bin/`
directories contain only `PKGBUILD` and `.SRCINFO`, which can be committed to
their respective AUR repositories after their download URLs are live. Do not
upload `.pkg.tar.zst` files to AUR; it hosts build recipes. Users can install
`ghm-bin` through an AUR helper. AUR inclusion does not imply admission to Arch's
official repositories. See [AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines).

**Debian:** A `.deb` on GitHub supports direct installation. For automatic APT
updates, separately maintain a signed APT repository with `Packages`, `Release`,
and `InRelease` metadata and a documented signing-key setup. For official Debian
inclusion, check existing [WNPP entries](https://www.debian.org/devel/wnpp/), file
an Intent To Package, audit copyright and Debian policy compliance, sign the
source package, and [find a sponsor](https://mentors.debian.net/intro-maintainers/).
The generated source package is a starting point for review, not a claim of
archive acceptance. Review the generated copyright information against all
contributors before an official submission.

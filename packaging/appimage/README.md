# AppImage

This recipe builds an x86_64 AppImage on Ubuntu 24.04 and bundles GTK4, libgit2,
curl, SQLite, libsecret and their application libraries. The binary baseline is
glibc 2.39 or later. Ubuntu 24.04, Debian 13 and recent Fedora/Arch systems are
intended targets; older Ubuntu/Debian releases and musl systems are outside this
baseline. Bundling does not establish compatibility with every distribution.

The host supplies its display server, graphics drivers, fonts/configuration,
certificate trust store, desktop D-Bus session and Secret Service keyring.
Bundled DejaVu fonts and a fallback font configuration cover minimal systems
without `/etc/fonts/fonts.conf`. The launcher detects the host CA bundle;
`GHM_CA_BUNDLE` can specify a custom bundle for both HTTPS APIs and Git.
Local work works without GitHub login. The AppImage uses the same XDG data paths
and keyring as a native installation.
The portable GUI defaults to GTK's Cairo renderer to avoid graphics-driver
and bundled-library mismatches. Set `GSK_RENDERER` explicitly to try another
renderer supported by your desktop.

## Build

With Docker available:

```sh
GHM_GITHUB_CLIENT_ID=YOUR_PUBLIC_OAUTH_CLIENT_ID sh packaging/appimage/build.sh
```

Use `CONTAINER_ENGINE=podman` for Podman. The current recipe targets x86_64 and
requires the public Client ID of a GitHub OAuth application with Device Flow
enabled. No secret, saved token or user database enters the build context.

The build runs all 22 CTests and the isolated local GUI workflow/stress under
Xvfb. It then copies the payload, dynamic libraries, GTK resources and dependency
license notices into an AppDir. Private libraries use relative ELF RPATHs;
`LD_LIBRARY_PATH` is not exported to host browser/portal child programs. The
official type-2 runtime is checksum-pinned. If upstream changes its continuous
asset, review the new release and update the digest before rebuilding.

Outputs are `dist/GitHubCommitManager-0.1.0-x86_64.AppImage` and its `.sha256`
checksum. `GHM_APPIMAGE_OUTPUT_DIR` overrides the output directory. Build outputs
are ignored by Git. Dependencies are supplied by Ubuntu's current package
repositories; this is not a byte-reproducible build.

## Run

```sh
chmod +x GitHubCommitManager-0.1.0-x86_64.AppImage
./GitHubCommitManager-0.1.0-x86_64.AppImage
./GitHubCommitManager-0.1.0-x86_64.AppImage --cli --help
```

If FUSE mounting is unavailable, use the runtime's extraction mode:

```sh
./GitHubCommitManager-0.1.0-x86_64.AppImage --appimage-extract-and-run
./GitHubCommitManager-0.1.0-x86_64.AppImage --appimage-extract-and-run --cli --help
```

Opening a portable window alone does not enable a background worker. For
scheduled jobs after the window closes, explicitly install the image:

```sh
./GitHubCommitManager-0.1.0-x86_64.AppImage --appimage-extract-and-run --install
```

This copies the image to `$XDG_DATA_HOME/ghm/appimage` (normally
`~/.local/share/ghm/appimage`), adds a desktop launcher, and installs/enables
`ghm-worker.service` in the user configuration directory. The worker launches
the stable image with `--worker` in extraction mode; it never refers to a
temporary mount. Installing an updated image replaces that stable copy.
A pre-existing custom user worker unit is preserved and causes installation
to refuse. The app's native worker unit in the data directory is overridden by
the AppImage unit, so there is only one enabled app worker.

The installer requires a working systemd user session. It does not enable
lingering or change system services. Jobs missed while the user manager or
machine is off are handled when the worker starts again, as in a native install.

To return to an existing native install, stop the user worker, remove only the
AppImage-managed user unit and AppImage desktop entry, reload the user manager,
and re-enable the native unit. Keep the shared `ghm` database, repositories and
keyring entries. Dependency notices and package/source version records are
inside `usr/share/doc/ghm/third-party` in the extracted AppImage.

AppImage layout and runtime references:
[manual packaging](https://docs.appimage.org/packaging-guide/manual.html),
[type-2 runtime](https://github.com/AppImage/type2-runtime).

## Package checks

```sh
sh packaging/appimage/check.sh dist/GitHubCommitManager-0.1.0-x86_64.AppImage
bash packaging/appimage/check-install.sh dist/GitHubCommitManager-0.1.0-x86_64.AppImage
```

The first check requires Git and creates disposable repositories/data to verify
the packaged CLI, branch creation, frozen scheduled commit and exact push to a
local bare remote. The second needs `desktop-file-validate` and substitutes a
fake service manager to check install paths and custom-unit refusal without
changing your actual worker. It is not a real systemd startup check.

`sh packaging/appimage/test.sh path/to/AppImage` builds minimal Ubuntu 24.04
and Debian 13 test containers and runs both checks plus GUI startup under Xvfb.
These environments install no GTK or application libraries. Runtime checks use
no network and receive only the AppImage, without host credentials or data.
They do not replace testing a real desktop's portals, keyring, browser and user
service manager.

The current package was checked on a Fedora 43 desktop and in minimal Ubuntu
24.04 and Debian 13 containers. Checks passed for normal FUSE launch on Fedora,
extraction mode, the packaged CLI/worker's frozen commit and exact local push,
installer file generation/refusal rules, SVG icons, fonts and GTK startup.
Authenticated private GitHub cloning with the saved desktop keyring also passed
on Fedora. Worker integration file tests use a fake service manager. Setting
`GHM_CHECK_SYSTEMD=1` for `check.sh` uses a temporary real systemd user service
for the packaged worker and removes it afterward, without replacing the native
worker; the frozen commit and exact local push passed through that real user
service on Fedora. Installing the AppImage over a native installation is an explicit user
choice rather than part of these checks. Other
architectures and older glibc baselines are not supported by this recipe.

# Build on the oldest supported binary baseline, not the developer's host.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libgtk-4-dev libgit2-dev \
    libcurl4-openssl-dev libsqlite3-dev libsecret-1-dev libjson-glib-dev \
    patchelf python3 squashfs-tools curl ca-certificates desktop-file-utils \
    libglib2.0-bin librsvg2-common glib-networking dconf-gsettings-backend \
    adwaita-icon-theme fonts-dejavu-core xvfb xauth dbus-x11 \
    && rm -rf /var/lib/apt/lists/*
RUN useradd --system --create-home builder
WORKDIR /source
COPY CMakeLists.txt README.md ./
COPY include/ include/
COPY src/ src/
COPY data/ data/
COPY tests/ tests/
COPY packaging/appimage/ packaging/appimage/
ARG GHM_GITHUB_CLIENT_ID
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DGHM_GITHUB_CLIENT_ID="${GHM_GITHUB_CLIENT_ID}" \
    && cmake --build /build -j4 \
    && chown -R builder:builder /build \
    && runuser -u builder -- ctest --test-dir /build --output-on-failure \
    && runuser -u builder -- dbus-run-session -- xvfb-run -a env GSK_RENDERER=cairo \
       GHM_GUI_TEST_TIMEOUT_SECONDS=90 /build/ghm-gui-workflow \
    && DESTDIR=/package/AppDir cmake --install /build \
    && python3 packaging/appimage/bundle.py /package/AppDir \
    && desktop-file-validate /package/AppDir/io.github.ghm.CommitManager.desktop
# The official runtime embeds FUSE and supports --appimage-extract-and-run.
# The hash pins it even though upstream's continuous download URL can change.
RUN curl --fail --location --retry 3 \
      https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 \
      -o /package/runtime-x86_64 \
    && echo '1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf  /package/runtime-x86_64' | sha256sum -c - \
    && curl --fail --location --retry 3 \
      https://raw.githubusercontent.com/AppImage/type2-runtime/75849dce7cc37e4319b633df1f116ca895c71a12/LICENSE \
      -o /package/AppDir/usr/share/doc/ghm/third-party/AppImage-runtime.LICENSE
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
RUN apt-get update && apt-get install -y --no-install-recommends python3 squashfs-tools \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /package/ /package/
COPY packaging/appimage/emit.py /usr/local/libexec/ghm-appimage-emit.py
ENTRYPOINT ["python3", "/usr/local/libexec/ghm-appimage-emit.py"]

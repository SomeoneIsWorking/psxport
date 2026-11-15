# The release AppImage build host: Ubuntu 22.04, the oldest LTS the AppImage catalog runs on, so a
# package built here needs glibc 2.35 at most. tools/port/appimage_container.py builds and runs it.
FROM docker.io/library/ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake binutils ca-certificates cmake curl desktop-file-utils file git gnupg \
        libtool make ninja-build pkg-config texinfo xz-utils zlib1g-dev libzstd-dev zsync \
        libc6-dev libstdc++-11-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
        libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev \
        libgl-dev libvulkan-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev \
        libdbus-1-dev libdrm-dev libgbm-dev libibus-1.0-dev \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends clang-20 lld-20 libc++-20-dev libc++abi-20-dev \
    && rm -rf /var/lib/apt/lists/*
ARG UV_VERSION=0.10.12
ARG UV_SHA256=adccf40b5d1939a5e0093081ec2307ea24235adf7c2d96b122c561fa37711c46
RUN curl -fsSL -o /tmp/uv.tar.gz \
        "https://github.com/astral-sh/uv/releases/download/${UV_VERSION}/uv-x86_64-unknown-linux-musl.tar.gz" \
    && echo "${UV_SHA256}  /tmp/uv.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/uv.tar.gz -C /usr/local/bin --strip-components=1 \
    && rm /tmp/uv.tar.gz
ENV CC=clang-20 CXX=clang++-20 UV_FROZEN=1 PYTHONDONTWRITEBYTECODE=1
# libstdc++ 11 has no <format>; libc++ 20 is linked statically, so the package needs no newer GLIBCXX.
ENV CXXFLAGS=-stdlib=libc++ LDFLAGS="-stdlib=libc++ -static-libstdc++"

# Ubuntu 22.04 packages no glslc; the LunarG Vulkan SDK's build needs glibc 2.34.
ARG VULKAN_SDK_VERSION=1.4.341.1
ARG VULKAN_SDK_SHA256=3bf0f762afb6c79bc6a9d9fb5998745ccff928800a29619b501ed9de7fd9789b
RUN curl -fsSL -o /tmp/vulkansdk.tar.xz \
        "https://sdk.lunarg.com/sdk/download/${VULKAN_SDK_VERSION}/linux/vulkansdk-linux-x86_64-${VULKAN_SDK_VERSION}.tar.xz" \
    && echo "${VULKAN_SDK_SHA256}  /tmp/vulkansdk.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/vulkansdk.tar.xz -C /usr/local/bin --strip-components=3 "${VULKAN_SDK_VERSION}/x86_64/bin/glslc" \
    && rm /tmp/vulkansdk.tar.xz && glslc --version

# Ubuntu 22.04 has no SDL3. The fork commit is release-3.4.20 plus an ICCCM WM_NAME: upstream writes
# UTF8_STRING under a UTF-8 locale, which the AppImage catalog's window check cannot read.
ARG SDL3_COMMIT=b76a1c0dbba2f3b936fb95ad55ec4cae58318db6
ARG SDL3_SHA256=693d77d5c450b4c2a3ec2ca3fdbf6e8835aaad658146c38eefe60ef7c7451cff
ARG SDL3_IMAGE_VERSION=3.4.8
ARG SDL3_IMAGE_SHA256=e8223b424bc7541cf84ffff5cf7e4f24ec3711c60462cb1bd6dc1d7bd7f25477
# FreeType is static: linuxdeploy's exclude list leaves the shared one out of every AppImage.
ARG FREETYPE_TAG=VER-2-14-1
ARG FREETYPE_SHA256=44bd69d1f0750603410cfd4f26f1c5523c5a3a087a2dfa8639b757dc7c6677be
WORKDIR /tmp/deps
RUN curl -fsSL -o sdl3.tar.gz "https://github.com/SomeoneIsWorking/SDL/archive/${SDL3_COMMIT}.tar.gz" \
    && echo "${SDL3_SHA256}  sdl3.tar.gz" | sha256sum -c - \
    && curl -fsSL -o sdl3_image.tar.gz \
        "https://github.com/libsdl-org/SDL_image/releases/download/release-${SDL3_IMAGE_VERSION}/SDL3_image-${SDL3_IMAGE_VERSION}.tar.gz" \
    && echo "${SDL3_IMAGE_SHA256}  sdl3_image.tar.gz" | sha256sum -c - \
    && curl -fsSL -o freetype.tar.gz "https://github.com/freetype/freetype/archive/refs/tags/${FREETYPE_TAG}.tar.gz" \
    && echo "${FREETYPE_SHA256}  freetype.tar.gz" | sha256sum -c - \
    && tar -xzf sdl3.tar.gz && tar -xzf sdl3_image.tar.gz && tar -xzf freetype.tar.gz \
    && cmake -S SDL-${SDL3_COMMIT} -B build-sdl3 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    && cmake --build build-sdl3 && cmake --install build-sdl3 \
    && cmake -S SDL3_image-${SDL3_IMAGE_VERSION} -B build-sdl3-image -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSDLIMAGE_VENDORED=OFF -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF \
        -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_WEBP=OFF \
    && cmake --build build-sdl3-image && cmake --install build-sdl3-image \
    && cmake -S freetype-${FREETYPE_TAG} -B build-freetype -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_BZIP2=ON \
        -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON \
    && cmake --build build-freetype && cmake --install build-freetype \
    && rm -rf /tmp/deps && ldconfig
WORKDIR /

# The maintained Lightrec and GNU Lightning forks at the revisions psxport pins.
ARG LIGHTNING_REVISION
ARG LIGHTREC_REVISION
RUN git config --system --add safe.directory '*' \
    && git clone https://github.com/SomeoneIsWorking/lightning.git /opt/lightning \
    && git -C /opt/lightning checkout --detach "${LIGHTNING_REVISION}" \
    && git -C /opt/lightning submodule update --init --recursive \
    && uv run --directory /opt/lightning --frozen python tools/build_lightning.py --clean --cc clang-20 \
        --skip-upstream-tests \
    && git clone https://github.com/SomeoneIsWorking/lightrec.git /opt/lightrec \
    && git -C /opt/lightrec checkout --detach "${LIGHTREC_REVISION}"
ENV PSXPORT_LIGHTNING_PREFIX=/opt/lightning/build/install/linux-x86_64 PSXPORT_LIGHTREC_DIR=/opt/lightrec

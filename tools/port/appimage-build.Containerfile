# The release AppImage build host: Ubuntu 22.04, the oldest LTS the AppImage catalog runs on, so a
# package built here needs glibc 2.35 at most. tools/port/appimage_container.py builds and runs it.
FROM docker.io/library/ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        binutils ca-certificates curl desktop-file-utils file git gnupg \
        make ninja-build pkg-config xz-utils zlib1g-dev zsync \
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
# Ubuntu 22.04's CMake is 3.22; titles need 3.24.
ARG CMAKE_VERSION=3.31.6
ARG CMAKE_SHA256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
RUN curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
    && echo "${CMAKE_SHA256}  /tmp/cmake.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/cmake.tar.gz -C /usr/local --strip-components=1 \
    && rm /tmp/cmake.tar.gz && cmake --version
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

# Ubuntu 22.04 has no SDL3: SDL3, SDL3_image and FreeType come from target-libraries.json, the pins the
# Windows prefix also builds. SDL3 is the SomeoneIsWorking fork, whose ICCCM WM_NAME the AppImage
# catalog's window check can read; FreeType is static, as linuxdeploy never bundles it.
COPY target_libraries.py target-libraries.json /opt/psxport-deps/
RUN uv run --no-project --python 3.12 python /opt/psxport-deps/target_libraries.py libraries --platform linux \
        --prefix /usr/local --work /tmp/deps \
    && rm -rf /tmp/deps && ldconfig

# The mounted checkouts belong to the host user.
RUN git config --system --add safe.directory '*'

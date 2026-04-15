# ── Stage 1: Build the Next.js dashboard ──────────────────────────
FROM node:22-alpine AS web-builder
WORKDIR /src/web-dashboard
COPY web-dashboard/package.json web-dashboard/package-lock.json* ./
RUN npm ci --ignore-scripts
COPY web-dashboard/ ./
RUN npm run build

# ── Stage 2: Build the C++ application ────────────────────────────
FROM ubuntu:22.04 AS cpp-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc-12 g++-12 cmake ninja-build git curl zip unzip tar \
    pkg-config linux-libc-dev ca-certificates \
    autoconf automake libtool make python3 nasm \
    && rm -rf /var/lib/apt/lists/*

# Install vcpkg at the baseline commit referenced in vcpkg.json
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && cd /opt/vcpkg \
    && git checkout c0991b94587f8493967303e45572cb187817cc9b \
    && ./bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${VCPKG_ROOT}:${PATH}"
ENV CC=gcc-12
ENV CXX=g++-12

WORKDIR /src

# Skip debug builds — release only (halves vcpkg build time)
ENV VCPKG_OVERLAY_TRIPLETS=/src/triplets
RUN mkdir -p /src/triplets \
    && cp /opt/vcpkg/triplets/community/arm64-linux.cmake /src/triplets/ \
    && echo 'set(VCPKG_BUILD_TYPE release)' >> /src/triplets/arm64-linux.cmake

# Install vcpkg dependencies first (cached unless vcpkg.json changes)
COPY vcpkg.json ./
RUN vcpkg install --triplet arm64-linux \
    --x-manifest-root=/src \
    --x-install-root=/src/vcpkg_installed

# Now copy the rest of the source
COPY CMakeLists.txt ./
COPY src/ src/
COPY third_party/ third_party/
COPY tools/ tools/

RUN cmake -G Ninja \
    -S . \
    -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc-12 \
    -DCMAKE_CXX_COMPILER=g++-12 \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-linux \
    -DVCPKG_OVERLAY_TRIPLETS=/src/triplets \
    -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
    -DVCPKG_MANIFEST_MODE=OFF \
    -DENABLE_GUI=OFF

RUN ninja -C build

# ── Stage 3: Runtime image ────────────────────────────────────────
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash streamon

WORKDIR /app

COPY --from=cpp-builder /src/build/StreaMonitor ./StreaMonitor
COPY --from=web-builder /src/web ./web/

# /app/config is the persistent volume for config.json, app_config.json, logs, crashes
# /app/downloads is the persistent volume for recordings
RUN mkdir -p downloads config \
    && chown -R streamon:streamon /app

# Entrypoint: symlink config files from /app/config into working dir, then exec
COPY docker-entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

USER streamon

EXPOSE 5000

VOLUME ["/app/downloads", "/app/config"]

ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["--web"]

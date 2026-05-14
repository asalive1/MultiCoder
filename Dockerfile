# MultiCoder — build + runtime image
# Base: Ubuntu 24.04 (Noble Numbat)

FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# ---- Build dependencies ----
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    pkg-config \
    ffmpeg \
    libportaudio2 \
    portaudio19-dev \
    libmp3lame-dev \
    libfdk-aac-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    libxml2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Configure + build (Release, ignoring optional encoding libs for MVP scaffold)
RUN cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=/opt/multicoder \
    -G "Unix Makefiles" \
  && cmake --build build \
  && cmake --install build

# ---- Runtime image ----
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Runtime libraries
RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    libportaudio2 \
    libmp3lame0 \
    libfdk-aac2 \
    libssl3 \
    libcurl4 \
    libxml2 \
    && rm -rf /var/lib/apt/lists/*

# Copy built binaries + web assets
COPY --from=builder /opt/multicoder /opt/multicoder
COPY www/ /opt/multicoder/www/

# Create config + log directories with correct permissions
RUN mkdir -p /etc/MC && \
    for i in 1 2 3 4 5; do \
        mkdir -p /etc/encoder${i}/logs /etc/encoder${i}/hls/segments; \
    done && \
    chmod -R 755 /etc/MC /etc/encoder1 /etc/encoder2 /etc/encoder3 /etc/encoder4 /etc/encoder5

# Copy default configs
COPY configs/ /opt/multicoder/configs/
COPY scripts/init-dirs.sh /opt/multicoder/bin/init-dirs.sh
RUN chmod +x /opt/multicoder/bin/init-dirs.sh

# Create a non-root service user
RUN useradd -r -s /bin/false -m -d /opt/multicoder multicoder && \
    chown -R multicoder:multicoder /opt/multicoder /etc/MC && \
    for i in 1 2 3 4 5; do chown -R multicoder:multicoder /etc/encoder${i}; done

USER multicoder

WORKDIR /opt/multicoder

# Default UI port
EXPOSE 8050

# Entrypoint: init dirs then start supervisor; workers launched by supervisor or entrypoint
COPY scripts/entrypoint.sh /opt/multicoder/bin/entrypoint.sh
USER root
RUN chmod +x /opt/multicoder/bin/entrypoint.sh
USER multicoder

ENTRYPOINT ["/opt/multicoder/bin/entrypoint.sh"]

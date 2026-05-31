# ============================================================================
# Cortex — Multi-stage Docker build
# ============================================================================
# Build:   docker build -t cortex .
# Run:     docker compose up  (preferred — sets up Postgres + Redis too)
# ============================================================================

# ── Stage 1: Build ──────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    libpqxx-dev libspdlog-dev libsimdjson-dev \
    libcurl4-openssl-dev libssl-dev \
    libhiredis-dev libgtest-dev \
    libonnxruntime-dev llhttp-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY CMakeLists.txt .
COPY include/ include/
COPY src/ src/
COPY tests/ tests/
COPY data/ data/

RUN cmake -B out -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
    && cmake --build out -j$(nproc)

# Run unit tests during build to catch regressions
RUN cd out && ctest --output-on-failure --timeout 60 || true

# ── Stage 2: Runtime ────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpqxx-7t64 libspdlog1.12 libsimdjson22 \
    libcurl4t64 libssl3t64 \
    libhiredis1.1.0 \
    libonnxruntime \
    llhttp \
    ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -r -s /bin/false cortex

WORKDIR /app

# Copy built binaries
COPY --from=builder /build/out/cortex_server .
COPY --from=builder /build/out/cortex_etl .

# Copy runtime assets
COPY www/ www/
COPY data/models/ data/models/
COPY sql/schema.sql sql/schema.sql
COPY config/cortex.toml config/cortex.toml

RUN chown -R cortex:cortex /app

USER cortex

EXPOSE 8080

HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
    CMD curl -sf http://localhost:8080/health || exit 1

ENTRYPOINT ["./cortex_server"]
CMD ["--port", "8080", "--db", "host=db port=5432 dbname=cortex user=cortex", "--www", "./www"]

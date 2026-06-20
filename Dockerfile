# ============================================================================
# Cortex — Docker build
# ============================================================================
# Single-stage build. The project depends on libraries that are NOT in Ubuntu's
# apt repos (ONNX Runtime, llhttp) plus a C++20-ABI build of libpqxx, so these
# are built/installed from source/tarball exactly as CI does. Keeping build and
# runtime in one image means the run environment is identical to the build
# environment — no fragile runtime-package-name matching across stages.
#
# Build:   docker build -t cortex .
# Run:     docker compose -f deploy/aws/docker-compose.prod.yml up
# ============================================================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# ── Base build + runtime apt deps (incl. gRPC/Protobuf for cluster mode) ─────
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl ca-certificates \
    libpq-dev libspdlog-dev libsimdjson-dev \
    libcurl4-openssl-dev libssl-dev \
    libhiredis-dev libgtest-dev \
    libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

# ── libpqxx 7.9.2 from source (C++20 ABI — the distro package is C++17) ──────
RUN curl -sL https://github.com/jtv/libpqxx/archive/refs/tags/7.9.2.tar.gz | tar xz \
    && cmake -B /tmp/pqxx-build -S libpqxx-7.9.2 \
        -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Release \
        -DSKIP_BUILD_TEST=ON -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=/usr \
    && cmake --build /tmp/pqxx-build -j"$(nproc)" \
    && cmake --install /tmp/pqxx-build \
    && ldconfig \
    && rm -rf libpqxx-7.9.2 /tmp/pqxx-build

# ── ONNX Runtime 1.22.0 (linux-aarch64) ─────────────────────────────────────
RUN ORT_VERSION=1.22.0 \
    && curl -sL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-aarch64-${ORT_VERSION}.tgz" \
       | tar xz -C /usr/local --strip-components=1 \
    && mkdir -p /usr/local/include/onnxruntime \
    && for h in onnxruntime_c_api onnxruntime_cxx_api onnxruntime_cxx_inline \
                onnxruntime_float16 onnxruntime_run_options_config_keys \
                onnxruntime_session_options_config_keys; do \
         ln -sf "/usr/local/include/${h}.h" /usr/local/include/onnxruntime/ 2>/dev/null || true; \
       done \
    && ldconfig

# ── llhttp 9.2.1 from source ─────────────────────────────────────────────────
RUN curl -sL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.2.1.tar.gz | tar xz \
    && cmake -B /tmp/llhttp-build -S llhttp-release-v9.2.1 \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=ON \
    && cmake --build /tmp/llhttp-build -j"$(nproc)" \
    && cmake --install /tmp/llhttp-build \
    && ldconfig \
    && rm -rf llhttp-release-v9.2.1 /tmp/llhttp-build

# ── Build Cortex (server + ETL only; skip the test suite) ────────────────────
WORKDIR /build
COPY CMakeLists.txt .
COPY include/ include/
COPY src/ src/
COPY proto/ proto/
COPY tests/ tests/
COPY data/ data/
RUN cmake -B out -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build out -j2 --target cortex_server cortex_etl

# ── Assemble the runtime layout at /app ──────────────────────────────────────
WORKDIR /app
RUN cp /build/out/cortex_server /build/out/cortex_etl /app/
COPY www/ www/
COPY data/models/ data/models/
COPY sql/schema.sql sql/schema.sql
COPY config/cortex.toml config/cortex.toml
RUN rm -rf /build \
    && useradd -r -s /bin/false cortex \
    && chown -R cortex:cortex /app
USER cortex

EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
    CMD curl -sf http://localhost:8080/health || exit 1

ENTRYPOINT ["./cortex_server"]
CMD ["--port", "8080", "--www", "./www"]

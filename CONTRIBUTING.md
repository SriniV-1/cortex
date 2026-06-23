# Contributing to Cortex

Thanks for your interest in contributing! Cortex is a real-time NBA analytics
engine written in modern C++20 (PostgreSQL · Redis · SIMD · HNSW vector search).
Bug reports, fixes, docs improvements, and well-scoped features are all welcome.

## Ways to contribute

- **Report a bug** — open an issue with steps to reproduce and your environment.
- **Fix a bug** — small, focused PRs are easiest to review and merge.
- **Improve docs** — README, architecture notes, and inline comments.
- **Propose a feature** — open a feature request issue first to discuss scope.

## Development setup

```bash
# 1. Dependencies (macOS / Homebrew)
brew install cmake postgresql@15 redis libpqxx spdlog simdjson googletest \
             openssl hiredis llhttp onnxruntime grpc

# 2. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.logicalcpu)

# 3. Run
./cortex.sh start          # starts Postgres/Redis, builds, launches server
```

## Before you open a pull request

1. **Branch** from `main` (`git checkout -b fix/short-description`).
2. **Build and test** locally:
   ```bash
   cmake --build build -j
   ctest --test-dir build --output-on-failure   # or: ./cortex.sh test
   ```
3. **Keep it scoped** — one logical change per PR.
4. **Match the surrounding style** — C++20, RAII, `const`-correctness, and the
   existing naming conventions. No raw owning pointers.
5. **Mind latency** — Cortex targets low-latency HTTP; avoid blocking work on
   the request path and note any performance-relevant change in the PR.
6. **Describe the change** clearly: what, why, and how you verified it.

## Reporting security issues

Please do **not** open public issues for security vulnerabilities. See
[SECURITY.md](SECURITY.md) for responsible disclosure.

## Code of Conduct

By participating, you agree to uphold our [Code of Conduct](CODE_OF_CONDUCT.md).

# Frontend Audit — Cortex Dashboard

*Audit performed before the 2026-06 frontend modernization. The findings below
drove the redesign; each is addressed by the new architecture described at the end.*

## State before redesign

- One file: `www/index.html` — 2,243 lines (728 lines of inline CSS, ~1,100 lines
  of inline JS, Chart.js 4 from a CDN).
- Visual identity: sports-broadcast / basketball-court theme (hardwood floor
  texture, hoop + ball SVG hero, jersey-number navigation).
- Features: live WebSocket game stream with win probability, stat leaderboards,
  recent games + live scoreboard, SIMD similarity search with team autofill,
  Elo power rankings + modal, Elo trajectory/distribution charts, dark/light
  theme, 30s auto-refresh, toasts.

## Findings

### 1. Identity mismatch (highest impact)
The page presents as an NBA fan site. The actual asset — a C++20 engine with a
lock-free SPSC ring buffer (8.7M ev/s), a hand-written kqueue/epoll HTTP +
WebSocket server, SIMD NEON vector search over 4.7M states, a hand-rolled HNSW
index, an ONNX inference path, and a gRPC distributed mode — was invisible.
A recruiter or staff engineer landing on the page had no way to discover any
of it.

### 2. Monolithic delivery
All CSS and JS inline in one 120 KB HTML document: no HTTP caching across
loads, no separation of concerns, dozens of `style="…"` attributes bypassing
the token system. Chart.js (~210 KB) pulled from a CDN for two charts — a
hard external dependency for an otherwise self-hosted system, and a failure
in offline demos.

### 3. Missing technical surfaces
The backend already exposed `/metrics` (Prometheus), `/api/index/status`,
latency-tested endpoints, and measured benchmark results in the README — none
surfaced in the UI. No benchmarks page, no architecture view, no observability
view, no recruiter-oriented summary.

### 4. Accessibility
Color-only status dots; search-result overlays without ARIA roles or keyboard
navigation; emoji as icons; no skip link; no `prefers-reduced-motion`
handling for the continuous court animations.

### 5. Information architecture
A single scroll with one audience in mind. No URL-addressable views, no
keyboard navigation, no command palette — table stakes for the
infrastructure-product aesthetic (Datadog, Grafana, Linear) this project
deserves.

## Redesign architecture (what replaced it)

```
www/
  index.html              app shell: sidebar, topbar, view root, command palette
  static/
    css/app.css           design system: tokens, primitives, view styles
    js/app.js             hash router, shell, theme, command palette, shortcuts
    js/api.js             typed fetch wrappers for every backend endpoint
    js/charts.js          dependency-free SVG chart primitives
    js/data/benchmarks.js measured results (sources cited inline)
    js/views/*.js         overview, benchmarks, architecture, observability, recruiter
```

- **Hash-routed views** (`#/`, `#/benchmarks`, `#/architecture`,
  `#/observability`, `#/recruiter`) — works with the existing C++ static
  handler (`/` + `/static/*`), zero server changes.
- **Zero runtime dependencies** — Chart.js removed; all charts are hand-rolled
  SVG, consistent with the repo's from-scratch ethos.
- **Every chart number is a measured result** from the repo's benchmark suite
  or README performance table — nothing invented.
- **All previous functionality preserved**: live WS stream, leaderboards,
  similarity search, Elo rankings/modal/charts, theme toggle, auto-refresh.

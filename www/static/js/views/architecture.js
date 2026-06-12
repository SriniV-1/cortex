// Architecture view — an inspectable SVG system diagram. Every node maps to
// real source files; clicking one shows what it does and its measured numbers.
// Flow dots animate along the hot path (feed → ring buffer → processor → serving).

const NS = 'http://www.w3.org/2000/svg';

const NODES = [
  { id: 'feed',   x: 30,  y: 48,  w: 140, h: 56, name: 'NBA Live Feed',     sub: 'liveData boxscore/pbp',
    path: 'external · polled over HTTPS',
    desc: 'The upstream source: NBA stats endpoints polled for live play-by-play and box scores. Historical backfill comes from the same feeds via the ETL pipeline.',
    stats: ['polling client', 'JSON'] },
  { id: 'client', x: 220, y: 48,  w: 140, h: 56, name: 'NBAClient',         sub: 'feed poller + parser',
    path: 'src/etl/NBAClient.cpp',
    desc: 'Fetches and parses feed JSON into typed play events. Fuzz-tested parsing path (libFuzzer) — malformed feed data cannot crash ingestion.',
    stats: ['libFuzzer-hardened', 'gtest suite'] },
  { id: 'ring',   x: 410, y: 48,  w: 170, h: 56, name: 'SPSC Ring Buffer',  sub: 'lock-free · 8.7M ev/s',
    path: 'include/stream/RingBuffer.hpp',
    desc: 'Cache-line-padded single-producer/single-consumer ring decoupling the network feed from stream processing. No locks, no contention — measured at 8.7M events/s, 8.7× the design target.',
    stats: ['8.7M events/s', 'cache-line padded', 'property-tested'] },
  { id: 'proc',   x: 630, y: 48,  w: 150, h: 56, name: 'StreamProcessor',   sub: 'consumes · fans out',
    path: 'src/stream/StreamProcessor.cpp',
    desc: 'Drains the ring, updates running stats, scores win probability on every event, and pushes frames to WebSocket subscribers.',
    stats: ['per-event win prob', 'WS fan-out'] },

  { id: 'stats',  x: 80,  y: 182, w: 150, h: 56, name: 'StatAccumulator',   sub: 'running aggregates',
    path: 'src/stream/StatAccumulator.cpp',
    desc: 'Maintains live player/team aggregates consumed by the leaderboard and game endpoints.',
    stats: ['gtest suite'] },
  { id: 'gsi',    x: 270, y: 182, w: 175, h: 56, name: 'GameStateIndex',    sub: 'SIMD NEON + HNSW',
    path: 'src/analytics/GameStateIndex.cpp · HNSWIndex.cpp',
    desc: 'Vector search over every historical game state (4.7M × 8-dim, 142 MB in RAM). Brute-force NEON scan answers in ~6 ms p99; the hand-rolled HNSW graph in <1 ms at >95% recall@10.',
    stats: ['4.7M vectors', '~6 ms p99 exact', '<1 ms HNSW'] },
  { id: 'elo',    x: 485, y: 182, w: 130, h: 56, name: 'EloTracker',        sub: '47 ms full build',
    path: 'src/analytics/EloTracker.cpp',
    desc: 'Team Elo from 8,400+ results: K=20/32 (season/playoffs), +100 home court, 25% season regression. Rebuilt at startup in 47 ms, persisted to Postgres.',
    stats: ['8,400+ games', '47 ms build'] },
  { id: 'onnx',   x: 655, y: 182, w: 125, h: 56, name: 'WinProbModel',      sub: 'ONNX · ~0.1 ms',
    path: 'src/analytics/WinProbModel.cpp',
    desc: '7-feature Elo-enhanced logistic regression exported to a 217-byte ONNX graph. 75.5% accuracy / 0.837 AUC, scored on every live event in ~0.1 ms.',
    stats: ['217-byte graph', '0.837 AUC', '~0.1 ms'] },

  { id: 'pg',     x: 80,  y: 318, w: 165, h: 56, name: 'PostgreSQL 15',     sub: 'partitioned · COPY 50K/s',
    path: 'src/etl/BulkInserter.cpp · sql/',
    desc: 'System of record: 6-partition range-partitioned events table with partition pruning and materialized-view box scores. The COPY-protocol bulk loader sustains ~50K rows/s.',
    stats: ['4.7M rows', '~50K rows/s COPY', 'matviews'] },
  { id: 'redis',  x: 290, y: 318, w: 150, h: 56, name: 'Redis Cache',       sub: 'cache-aside + breaker',
    path: 'src/serving/RedisCache.cpp',
    desc: 'Cache-aside on hot queries, guarded by a 3-state circuit breaker — when Redis degrades, the breaker opens and queries fall through to Postgres instead of hanging.',
    stats: ['3-state breaker', 'cache-aside'] },
  { id: 'http',   x: 485, y: 318, w: 175, h: 56, name: 'HTTP/WS Server',    sub: 'kqueue/epoll · JWT/RBAC',
    path: 'src/serving/HttpServer.cpp · Router.cpp',
    desc: 'Hand-written HTTP/1.1 + RFC 6455 WebSocket server on an edge-triggered kqueue/epoll event loop. Trie router with path params, JWT auth + RBAC, per-connection backpressure (1024-frame cap), rate limiting.',
    stats: ['15.6 ms p99 @ 1k WS', 'from scratch', 'backpressure caps'] },
  { id: 'web',    x: 700, y: 318, w: 80,  h: 56, name: 'Clients',           sub: 'REST · WS',
    path: 'www/ · this dashboard',
    desc: 'This dashboard plus any REST/WebSocket consumer. Everything you see here is served by the C++ process you are inspecting.',
    stats: ['dependency-free UI'] },
];

const EDGES = [
  ['feed', 'client'], ['client', 'ring'], ['ring', 'proc'],
  ['proc', 'stats', 'down'], ['proc', 'gsi', 'down'], ['proc', 'elo', 'down'], ['proc', 'onnx', 'down'],
  ['stats', 'pg', 'down'], ['gsi', 'http', 'down'], ['elo', 'http', 'down'], ['onnx', 'http', 'down'],
  ['pg', 'http'], ['redis', 'http'], ['http', 'web'],
];

const GROUPS = [
  { label: 'INGEST',            y: 36 },
  { label: 'ANALYTICS',         y: 170 },
  { label: 'STORAGE · SERVING', y: 306 },
];

const CLUSTER = [
  { title: 'Coordinator', file: 'src/distributed/Coordinator.cpp',
    body: 'Owns game→worker assignment via a <span class="mono">150-virtual-node</span> FNV-1a consistent hash ring — adding a worker migrates only ~1/N of games. Streams assignments over gRPC.' },
  { title: 'Workers (IngestorNode)', file: 'src/distributed/IngestorNode.cpp',
    body: 'Each runs its own NBAClient → RingBuffer → StreamProcessor pipeline for assigned games. Registers on startup, heartbeats every <span class="mono">2 s</span>.' },
  { title: 'Failure recovery', file: 'src/distributed/ConsistentHashRing.cpp',
    body: 'Three missed heartbeats (<span class="mono">6 s</span>) → games reassigned with an incremented epoch; stale workers are fenced (split-brain prevention). New owner replays play-by-play from S3 — stateless rebuild in <span class="mono">&lt;1 s</span>.' },
];

function el(tag, attrs = {}) {
  const n = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  return n;
}

function center(n) { return { x: n.x + n.w / 2, y: n.y + n.h / 2 }; }

function edgePath(a, b) {
  const ca = center(a), cb = center(b);
  // same row → straight horizontal; otherwise gentle vertical S-curve
  if (Math.abs(ca.y - cb.y) < 8) {
    return `M${a.x + a.w},${ca.y} L${b.x},${cb.y}`;
  }
  const x1 = ca.x, y1 = a.y + a.h, x2 = cb.x, y2 = b.y;
  const my = (y1 + y2) / 2;
  return `M${x1},${y1} C${x1},${my} ${x2},${my} ${x2},${y2}`;
}

function renderDiagram() {
  const host = document.getElementById('arch-diagram');
  host.innerHTML = '';
  const svg = el('svg', { viewBox: '0 0 800 400' });
  const byId = Object.fromEntries(NODES.map(n => [n.id, n]));

  GROUPS.forEach(g => {
    const t = el('text', { x: 10, y: g.y, class: 'arch-group-label' });
    t.textContent = g.label;
    svg.appendChild(t);
  });

  EDGES.forEach(([from, to]) => {
    svg.appendChild(el('path', { d: edgePath(byId[from], byId[to]), class: 'arch-edge' }));
  });

  // animated dots along the hot path
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (!reduced) {
    const hot = ['feed→client', 'client→ring', 'ring→proc'].map((_, i) => {
      const [a, b] = [['feed', 'client'], ['client', 'ring'], ['ring', 'proc']][i];
      return edgePath(byId[a], byId[b]);
    });
    hot.forEach((d, i) => {
      const dot = el('circle', { r: 3, class: 'arch-flow-dot' });
      const motion = el('animateMotion', { dur: '1.6s', repeatCount: 'indefinite', path: d, begin: `${i * 0.5}s` });
      dot.appendChild(motion);
      svg.appendChild(dot);
    });
  }

  NODES.forEach(n => {
    const g = el('g', { class: 'arch-node', 'data-id': n.id, tabindex: 0, role: 'button' });
    g.appendChild(el('rect', { x: n.x, y: n.y, width: n.w, height: n.h, rx: 8 }));
    const t1 = el('text', { x: n.x + 12, y: n.y + 24 });
    t1.textContent = n.name;
    const t2 = el('text', { x: n.x + 12, y: n.y + 42, class: 'arch-sub' });
    t2.textContent = n.sub;
    g.appendChild(t1); g.appendChild(t2);
    svg.appendChild(g);
  });

  host.appendChild(svg);

  const select = id => {
    host.querySelectorAll('.arch-node').forEach(nd => nd.classList.toggle('selected', nd.dataset.id === id));
    renderDetail(byId[id]);
  };
  host.addEventListener('click', e => {
    const node = e.target.closest('.arch-node');
    if (node) select(node.dataset.id);
  });
  host.addEventListener('keydown', e => {
    const node = e.target.closest('.arch-node');
    if (node && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); select(node.dataset.id); }
  });
  select('ring'); // open on the most interesting component
}

function renderDetail(n) {
  document.getElementById('arch-detail').innerHTML = `
    <h4>${n.name}</h4>
    <div class="arch-path">${n.path}</div>
    <p>${n.desc}</p>
    <div class="arch-stats">${n.stats.map(s => `<span class="badge">${s}</span>`).join('')}</div>`;
}

function renderCluster() {
  document.getElementById('arch-cluster').innerHTML = '<div class="cluster-grid">' +
    CLUSTER.map(c => `<div class="cluster-cell">
      <h4>${c.title}</h4>
      <div class="arch-path" style="font-size:10px;margin-bottom:6px">${c.file}</div>
      <p>${c.body}</p>
    </div>`).join('') + '</div>';
}

export const architectureView = {
  mount() {
    renderDiagram();
    renderCluster();
  },
  show() {},
  hide() {},
};

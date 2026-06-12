// Measured benchmark results. Every number here comes from the repo's own
// benchmark suite (tests/benchmarks/) or the README performance table —
// nothing is estimated or invented. Sources are cited per entry so the
// UI can link a chart back to the code that produced it.

export const QUERY_LATENCY = {
  title: 'Query latency — p99 vs. target',
  unit: 'ms',
  target: 20,
  source: 'tests/benchmarks/bench_queries.cpp · README “Performance Results”',
  rows: [
    { label: 'game_summary',            value: 0.6,  detail: 'Single-game box score (materialized view)' },
    { label: 'game_events',             value: 3.2,  detail: 'Play-by-play range scan (partition-pruned)' },
    { label: 'similarity (4.7M scan)',  value: 6.0,  detail: 'Brute-force SIMD NEON over the full feature store' },
    { label: 'player_season',           value: 6.3,  detail: 'Season aggregate across partitions' },
    { label: 'WS broadcast (1k conns)', value: 15.6, detail: 'Fan-out p99 with per-connection backpressure' },
  ],
};

export const THROUGHPUT = {
  title: 'Ingestion throughput',
  source: 'tests/benchmarks/bench_throughput.cpp',
  rows: [
    { label: 'Lock-free SPSC ring buffer', value: 8_700_000, unit: 'events/s', target: 1_000_000,
      detail: 'Cache-line-padded single-producer/single-consumer ring. Target: >1M ev/s — beat by 8.7×.' },
    { label: 'PostgreSQL COPY loader', value: 50_000, unit: 'rows/s', target: null,
      detail: 'Binary COPY protocol into a 6-partition range-partitioned table.' },
  ],
};

export const ANN_COMPARISON = {
  title: 'HNSW vs. brute-force SIMD',
  source: 'src/analytics/HNSWIndex.cpp · tests/benchmarks/bench_similarity.cpp',
  rows: [
    { label: 'Brute-force SIMD (NEON)', latency_ms: 6.0, recall: 100, detail: 'vld1q/vfmaq/vaddvq linear scan, 4.7M × 8-dim vectors, 142 MB in RAM' },
    { label: 'HNSW graph index',        latency_ms: 1.0, recall: 95,  detail: 'Hand-rolled hierarchical small-world graph; >95% recall@10, <1 ms', latencyPrefix: '<' },
  ],
};

export const MICRO = {
  title: 'Microbenchmarks',
  source: 'README “Performance Results”',
  rows: [
    { label: 'Win-probability inference (ONNX)', value: '~0.1 ms', detail: '7-feature logistic regression, 217-byte graph, scored on every live event' },
    { label: 'Elo build (8,400+ games)',          value: '47 ms',   detail: 'Full historical rebuild at server startup' },
    { label: 'Worker failure detection',          value: '6 s',     detail: '3 missed 2 s heartbeats → reassignment (cluster mode)' },
    { label: 'Worker state rebuild',              value: '<1 s',    detail: 'Stateless replay from S3 play-by-play after reassignment' },
  ],
};

export const SCALE = [
  { value: 4_700_000, label: 'play-by-play events', fmt: 'M' },
  { value: 8_400,     label: 'games (2019–2026)',   fmt: 'K' },
  { value: 1_250,     label: 'players tracked',      fmt: 'raw' },
  { value: 142,       label: 'MB in-RAM feature store', fmt: 'raw' },
];

export const RUN_YOURSELF = [
  { cmd: './build/cortex_bench',       what: 'query latency (p50/p95/p99)' },
  { cmd: './build/cortex_throughput',  what: 'ring buffer throughput' },
  { cmd: './build/cortex_similarity',  what: 'SIMD similarity scan across 4.7M events' },
  { cmd: './build/cortex_ws_load',     what: 'WebSocket broadcast latency (1000 clients)' },
];

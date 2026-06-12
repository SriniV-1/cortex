// Observability view — everything here is live: Prometheus metrics from
// /metrics, health/readiness, index status, and request latency measured
// from this browser against the running server. Polls every 5s while
// visible; stops completely when the view is hidden.

import { api, parsePrometheus } from '../api.js';
import { lineChart, sparkline, fmtNum } from '../charts.js';
import { onThemeChange } from '../app.js';

const POLL_MS = 5000;
const HISTORY_MAX = 120; // 10 minutes at 5s cadence

let pollTimer = null;
let tick = 0;
const eventHistory = [];           // { t, v } cumulative counter samples
const probeHistory = {};           // endpoint → [ms, …]
let lastMetrics = {};

const CARDS = [
  { key: 'cortex_events_processed',    label: 'events processed',     sub: 'cumulative · this process' },
  { key: 'cortex_active_games',        label: 'active games',         sub: 'live WS-streamed games' },
  { key: 'cortex_similarity_index_size', label: 'similarity index',   sub: 'vectors in RAM' },
  { key: 'cortex_elo_games_processed', label: 'elo games processed',  sub: 'rating system input' },
  { key: 'cortex_rate_limiter_buckets', label: 'rate-limit buckets',  sub: 'distinct client IPs tracked' },
];

const PROBES = [
  { path: '/health',          label: '/health' },
  { path: '/api/stats',       label: '/api/stats' },
  { path: '/api/elo',         label: '/api/elo' },
  { path: '/api/leaderboard?stat=ppg', label: '/api/leaderboard' },
  { path: '/metrics',         label: '/metrics' },
];

function renderCards() {
  const host = document.getElementById('obs-cards');
  host.innerHTML = CARDS.map(c => {
    const v = lastMetrics[c.key];
    return `<div class="obs-card">
      <div class="obs-card-label"><span>${c.label}</span><span class="spark" data-key="${c.key}"></span></div>
      <div class="obs-card-value">${v === undefined ? '—' : fmtNum(v)}</div>
      <div class="obs-card-sub">${c.sub}</div>
    </div>`;
  }).join('');
}

function renderEventsChart() {
  if (eventHistory.length < 2) {
    document.getElementById('obs-events-chart').innerHTML =
      '<p class="card-sub" style="padding-top:8px">Collecting samples — the rate chart appears after two polls (~10 s)…</p>';
    return;
  }
  // convert the cumulative counter into a per-interval rate
  const pts = [];
  for (let i = 1; i < eventHistory.length; i++) {
    const dv = eventHistory[i].v - eventHistory[i - 1].v;
    const dt = (eventHistory[i].t - eventHistory[i - 1].t) / 1000;
    pts.push({ x: i, y: Math.max(0, Math.round(dv / dt)) });
  }
  const rate = pts.length ? pts[pts.length - 1].y : 0;
  document.getElementById('obs-rate').textContent = `${fmtNum(rate)} ev/s now`;
  lineChart(document.getElementById('obs-events-chart'),
    [{ label: 'events/s', color: getComputedStyle(document.documentElement).getPropertyValue('--accent').trim() || '#6e8bff', points: pts }],
    { height: 220, yMin: 0, xLabels: Object.fromEntries(pts.map(p => [p.x, `-${(pts.length - p.x) * POLL_MS / 1000}s`])), hoverFmt: v => fmtNum(v) + '/s' });
}

function renderProbes() {
  const host = document.getElementById('obs-probe');
  const rows = PROBES.map(p => {
    const hist = probeHistory[p.label] || [];
    const last = hist[hist.length - 1];
    const cls = last === undefined ? '' : last < 20 ? 'good' : last < 100 ? 'ok' : 'bad';
    return `<tr>
      <td class="mono">${p.label}</td>
      <td><span class="probe-ms ${cls}">${last === undefined ? '—' : last.toFixed(1) + ' ms'}</span></td>
      <td><span class="spark" data-probe="${p.label}"></span></td>
    </tr>`;
  }).join('');
  host.innerHTML = `<table class="probe-table">
    <thead><tr><th>endpoint</th><th>last round-trip</th><th>trend</th></tr></thead>
    <tbody>${rows}</tbody></table>
    <p class="card-sub" style="margin-top:10px">Round-trip = fetch start → response headers, measured with
    <code>performance.now()</code> in your browser. Includes network — server-side p99s are on the
    <a href="#/benchmarks" style="color:var(--accent-2)">Benchmarks</a> page.</p>`;
  host.querySelectorAll('.spark[data-probe]').forEach(s => {
    sparkline(s, (probeHistory[s.dataset.probe] || []).slice(-30));
  });
}

function renderHealth(healthOk, indexStatus) {
  const items = [
    { label: 'HTTP server', ok: healthOk, detail: healthOk ? '200 /health' : 'unreachable' },
    { label: 'Similarity index', ok: !!(indexStatus && indexStatus.loaded),
      detail: indexStatus ? (indexStatus.loaded ? fmtNum(indexStatus.size) + ' vectors' : 'building…') : 'unknown' },
    { label: 'Metrics endpoint', ok: Object.keys(lastMetrics).length > 0,
      detail: Object.keys(lastMetrics).length + ' series' },
    { label: 'Live ingestion', ok: (lastMetrics.cortex_active_games || 0) > 0,
      detail: (lastMetrics.cortex_active_games || 0) > 0 ? `${lastMetrics.cortex_active_games} active game(s)` : 'idle — no live games right now', neutral: true },
  ];
  document.getElementById('obs-health').innerHTML = '<div class="health-list">' +
    items.map(i => `<div class="health-item">
      <span class="dot ${i.ok ? 'live' : i.neutral ? 'warn' : ''}"></span>
      <span>${i.label}</span><span class="mono">${i.detail}</span>
    </div>`).join('') + '</div>';
}

async function poll() {
  tick++;
  let healthOk = false, indexStatus = null;

  // probe latencies (measured) — sequential so probes don't contend with
  // each other and inflate every reading
  for (const p of PROBES) {
    const t0 = performance.now();
    try {
      const r = await fetch(p.path);
      if (!r.ok) throw new Error();
      const ms = performance.now() - t0;
      (probeHistory[p.label] = probeHistory[p.label] || []).push(ms);
      if (probeHistory[p.label].length > HISTORY_MAX) probeHistory[p.label].shift();
      if (p.path === '/health') healthOk = true;
    } catch { /* endpoint down — leave gap in history */ }
  }

  try {
    lastMetrics = parsePrometheus(await api.metricsText());
    if (lastMetrics.cortex_events_processed !== undefined) {
      eventHistory.push({ t: Date.now(), v: lastMetrics.cortex_events_processed });
      if (eventHistory.length > HISTORY_MAX) eventHistory.shift();
    }
  } catch { lastMetrics = {}; }

  try { indexStatus = await api.indexStatus(); } catch { /* unavailable */ }

  renderCards();
  renderEventsChart();
  renderProbes();
  renderHealth(healthOk, indexStatus);
}

export const observabilityView = {
  mount() {
    renderCards();
    onThemeChange(() => { renderEventsChart(); renderProbes(); });
  },
  show() {
    poll();
    pollTimer = setInterval(poll, POLL_MS);
  },
  hide() {
    clearInterval(pollTimer);
    pollTimer = null;
  },
  refresh: poll,
};

// Benchmarks view — interactive charts over the repo's measured results.
// Data lives in data/benchmarks.js with sources cited; this module renders it.

import { QUERY_LATENCY, THROUGHPUT, ANN_COMPARISON, MICRO, RUN_YOURSELF } from '../data/benchmarks.js';
import { hbarChart, fmtNum } from '../charts.js';
import { onThemeChange } from '../app.js';

const esc = s => String(s ?? '').replace(/[&<>"']/g, c =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

function renderLatency() {
  document.getElementById('bench-latency-src').textContent = 'Source: ' + QUERY_LATENCY.source;
  hbarChart(document.getElementById('bench-latency'),
    QUERY_LATENCY.rows, { unit: 'ms', target: QUERY_LATENCY.target, max: 22 });
}

function renderThroughput() {
  document.getElementById('bench-throughput-src').textContent = 'Source: ' + THROUGHPUT.source;
  const host = document.getElementById('bench-throughput');
  host.innerHTML = '';
  THROUGHPUT.rows.forEach(r => {
    const wrap = document.createElement('div');
    wrap.style.marginBottom = '18px';
    const ratio = r.target ? (r.value / r.target).toFixed(1) : null;
    wrap.innerHTML = `
      <div class="micro-row" style="border:0;padding-bottom:4px;">
        <span class="micro-label">${esc(r.label)}</span>
        <span class="micro-value">${fmtNum(r.value)} ${esc(r.unit)}</span>
        ${ratio ? `<span class="chip chip-accent">${ratio}× target</span>` : ''}
      </div>
      <div class="hbar"><div class="hbar-row">
        <span class="hbar-label" style="width:0;min-width:0"></span>
        <span class="hbar-track" style="grid-column: 1 / 3">
          <span class="hbar-fill" style="width:0%"></span>
          ${r.target ? `<span class="hbar-target" style="left:${(r.target / (r.value * 1.06)) * 100}%"></span>` : ''}
        </span>
        <span class="hbar-value mono">${fmtNum(r.value)}</span>
      </div></div>
      <div class="micro-detail" style="margin-top:6px">${esc(r.detail)}</div>`;
    host.appendChild(wrap);
    requestAnimationFrame(() => setTimeout(() => {
      wrap.querySelector('.hbar-fill').style.width = '100%';
    }, 60));
  });
}

function renderAnn() {
  document.getElementById('bench-ann-src').textContent = 'Source: ' + ANN_COMPARISON.source;
  const host = document.getElementById('bench-ann');
  host.innerHTML = `<div class="ann-grid">` + ANN_COMPARISON.rows.map(r => `
    <div class="ann-cell">
      <div class="ann-name">${esc(r.label)}</div>
      <div class="ann-stat"><span>p99 latency</span><span class="mono">${r.latencyPrefix || ''}${r.latency_ms.toFixed(1)} ms</span></div>
      <div class="ann-stat"><span>recall@10</span><span class="mono">${r.recall === 100 ? '100% (exact)' : '>' + r.recall + '%'}</span></div>
      <div class="ann-detail">${esc(r.detail)}</div>
    </div>`).join('') + `</div>
    <p class="card-sub" style="margin-top:12px">The trade: the HNSW graph gives up ~5 points of recall
    for a <strong>6× latency win</strong>. Both are hand-written — the SIMD scan is the exact baseline
    the HNSW index is validated against.</p>`;
}

function renderMicro() {
  document.getElementById('bench-micro-src').textContent = 'Source: ' + MICRO.source;
  document.getElementById('bench-micro').innerHTML = '<div class="micro-list">' +
    MICRO.rows.map(r => `
      <div class="micro-row">
        <span class="micro-label">${esc(r.label)}</span>
        <span class="micro-value">${esc(r.value)}</span>
        <span class="micro-detail">${esc(r.detail)}</span>
      </div>`).join('') + '</div>';
}

function renderRun() {
  document.getElementById('bench-run').innerHTML = '<div class="run-list">' +
    RUN_YOURSELF.map(r => `<div class="run-row"><code>${esc(r.cmd)}</code><span>${esc(r.what)}</span></div>`).join('') +
    '</div>';
}

export const benchmarksView = {
  mount() {
    renderLatency();
    renderThroughput();
    renderAnn();
    renderMicro();
    renderRun();
    onThemeChange(() => { renderLatency(); });
  },
  show() {
    // re-trigger bar animations on revisit
    renderLatency();
  },
  hide() {},
};

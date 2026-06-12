// App shell: hash router + view lifecycle, theme, command palette,
// keyboard shortcuts, toasts, server status. Views register a
// { mount, show, hide } interface; mount runs once (lazily), show/hide
// let views start/stop their own polling so hidden views cost nothing.

import { api } from './api.js';
import { overviewView } from './views/overview.js';
import { benchmarksView } from './views/benchmarks.js';
import { architectureView } from './views/architecture.js';
import { observabilityView } from './views/observability.js';
import { recruiterView } from './views/recruiter.js';

// ── Toasts ──────────────────────────────────────────────────────────────
const TOAST_LIMIT = 4;
export function showToast(msg, type = 'error') {
  const container = document.getElementById('toast-container');
  while (container.children.length >= TOAST_LIMIT) container.firstChild.remove();
  const toast = document.createElement('div');
  toast.className = 'toast' + (type === 'warn' ? ' toast-warn' : type === 'info' ? ' toast-info' : '');
  toast.textContent = msg;
  container.appendChild(toast);
  setTimeout(() => toast.remove(), 5000);
}
window.showToast = showToast; // legacy hooks

// ── Theme ───────────────────────────────────────────────────────────────
const themeListeners = new Set();
export function onThemeChange(fn) { themeListeners.add(fn); }
export function applyTheme(theme) {
  document.documentElement.setAttribute('data-theme', theme);
  localStorage.setItem('theme', theme);
  document.getElementById('theme-icon-moon').hidden = theme !== 'dark';
  document.getElementById('theme-icon-sun').hidden = theme === 'dark';
  themeListeners.forEach(fn => { try { fn(theme); } catch { /* listener error */ } });
}
applyTheme(localStorage.getItem('theme') || 'dark');
document.getElementById('theme-toggle').addEventListener('click', () => {
  const cur = document.documentElement.getAttribute('data-theme');
  applyTheme(cur === 'dark' ? 'light' : 'dark');
});

// ── Server status (sidebar) ─────────────────────────────────────────────
export async function checkHealth() {
  const dot = document.getElementById('server-dot');
  const label = document.getElementById('server-status');
  try {
    const r = await api.health();
    if (r.ok) { dot.className = 'dot live'; label.textContent = 'online'; return true; }
  } catch { /* offline */ }
  dot.className = 'dot';
  label.textContent = 'offline';
  return false;
}

function tickClock() {
  const el = document.getElementById('rail-clock');
  const p = n => String(n).padStart(2, '0');
  const d = new Date();
  el.textContent = `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
tickClock();
setInterval(tickClock, 1000);

// ── Auto-refresh ────────────────────────────────────────────────────────
// The shell owns the 30s cadence; the active view's refresh() is invoked.
const REFRESH_PERIOD = 30;
let refreshTimer = null, countdownTimer = null, countdownSecs = REFRESH_PERIOD;

function doRefresh() {
  checkHealth();
  const v = views[currentRoute];
  if (v && v.refresh) v.refresh();
}

export function startAutoRefresh() {
  const btn = document.getElementById('auto-refresh-toggle');
  btn.setAttribute('aria-pressed', 'true');
  btn.classList.add('refreshing');
  countdownSecs = REFRESH_PERIOD;
  const cd = document.getElementById('refresh-countdown');
  cd.textContent = countdownSecs + 's';
  countdownTimer = setInterval(() => {
    countdownSecs--;
    if (countdownSecs <= 0) countdownSecs = REFRESH_PERIOD;
    cd.textContent = countdownSecs + 's';
  }, 1000);
  refreshTimer = setInterval(() => { countdownSecs = REFRESH_PERIOD; doRefresh(); }, REFRESH_PERIOD * 1000);
}
export function stopAutoRefresh() {
  const btn = document.getElementById('auto-refresh-toggle');
  btn.setAttribute('aria-pressed', 'false');
  btn.classList.remove('refreshing');
  document.getElementById('refresh-countdown').textContent = '';
  clearInterval(refreshTimer); clearInterval(countdownTimer);
  refreshTimer = countdownTimer = null;
}
document.getElementById('auto-refresh-toggle').addEventListener('click', () => {
  refreshTimer ? stopAutoRefresh() : startAutoRefresh();
});

// ── Router ──────────────────────────────────────────────────────────────
const views = {
  overview: overviewView,
  benchmarks: benchmarksView,
  architecture: architectureView,
  observability: observabilityView,
  recruiter: recruiterView,
};
const TITLES = {
  overview: 'Courtside', benchmarks: 'Stat Sheet', architecture: 'Playbook',
  observability: "Scorer's Table", recruiter: 'Scouting Report',
};
const mounted = new Set();
let currentRoute = null;

function parseRoute() {
  const h = (location.hash || '#/').replace(/^#\//, '');
  return views[h] ? h : 'overview';
}

function navigate() {
  const route = parseRoute();
  if (route === currentRoute) return;
  if (currentRoute && views[currentRoute].hide) views[currentRoute].hide();

  document.querySelectorAll('.view').forEach(s => { s.hidden = s.dataset.view !== route; });
  document.querySelectorAll('.nav-item').forEach(a => {
    const active = a.dataset.route === route;
    a.classList.toggle('active', active);
    if (active) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
  });
  document.getElementById('view-title').textContent = TITLES[route];
  document.title = `Cortex — ${TITLES[route]}`;

  currentRoute = route;
  const v = views[route];
  if (!mounted.has(route)) { mounted.add(route); v.mount(); }
  if (v.show) v.show();
  document.getElementById('main').scrollTop = 0;
  window.scrollTo({ top: 0 });
}
window.addEventListener('hashchange', navigate);

// ── Command palette ─────────────────────────────────────────────────────
const NAV_ICON = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M5 12h14M13 6l6 6-6 6"/></svg>';
const ACT_ICON = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M13 2 4.5 13.5H11L9.5 22 19 10h-6.5L13 2Z"/></svg>';
const EXT_ICON = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M14 4h6v6M20 4l-9 9M10 5H6a2 2 0 0 0-2 2v11a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2v-4"/></svg>';

const COMMANDS = [
  { section: 'Navigate', label: 'Courtside — live dashboard', kbd: '1', icon: NAV_ICON, run: () => { location.hash = '#/'; } },
  { section: 'Navigate', label: 'Stat Sheet — benchmarks', kbd: '2', icon: NAV_ICON, run: () => { location.hash = '#/benchmarks'; } },
  { section: 'Navigate', label: 'Playbook — architecture', kbd: '3', icon: NAV_ICON, run: () => { location.hash = '#/architecture'; } },
  { section: 'Navigate', label: "Scorer's Table — observability", kbd: '4', icon: NAV_ICON, run: () => { location.hash = '#/observability'; } },
  { section: 'Navigate', label: 'Scouting Report — recruiter view', kbd: '5', icon: NAV_ICON, run: () => { location.hash = '#/recruiter'; } },
  { section: 'Actions', label: 'Toggle theme', kbd: 'T', icon: ACT_ICON, run: () => document.getElementById('theme-toggle').click() },
  { section: 'Actions', label: 'Toggle auto-refresh', icon: ACT_ICON, run: () => document.getElementById('auto-refresh-toggle').click() },
  { section: 'Actions', label: 'Refresh data now', kbd: 'R', icon: ACT_ICON, run: doRefresh },
  { section: 'Actions', label: 'Search players', kbd: '/', icon: ACT_ICON, run: () => { location.hash = '#/'; setTimeout(() => document.getElementById('player-search').focus(), 60); } },
  { section: 'Actions', label: 'Run similarity search', icon: ACT_ICON, run: () => { location.hash = '#/'; setTimeout(() => document.getElementById('sim-btn').scrollIntoView({ behavior: 'smooth', block: 'center' }), 60); } },
  { section: 'Links', label: 'GitHub repository', icon: EXT_ICON, run: () => window.open('https://github.com/SriniV-1/cortex', '_blank', 'noopener') },
  { section: 'Links', label: 'Raw /metrics (Prometheus)', icon: EXT_ICON, run: () => window.open('/metrics', '_blank', 'noopener') },
  { section: 'Links', label: 'Health endpoint', icon: EXT_ICON, run: () => window.open('/health', '_blank', 'noopener') },
];

const cmdkBackdrop = document.getElementById('cmdk-backdrop');
const cmdkInput = document.getElementById('cmdk-input');
const cmdkList = document.getElementById('cmdk-list');
let cmdkIdx = 0, cmdkFiltered = [];

function openCmdk() {
  cmdkBackdrop.hidden = false;
  cmdkInput.value = '';
  renderCmdk('');
  cmdkInput.focus();
}
function closeCmdk() { cmdkBackdrop.hidden = true; }

function renderCmdk(q) {
  const needle = q.trim().toLowerCase();
  cmdkFiltered = COMMANDS.filter(c => !needle || c.label.toLowerCase().includes(needle) || c.section.toLowerCase().includes(needle));
  cmdkIdx = 0;
  if (!cmdkFiltered.length) { cmdkList.innerHTML = '<div class="cmdk-empty">No matching commands</div>'; return; }
  let html = '', lastSection = null;
  cmdkFiltered.forEach((c, i) => {
    if (c.section !== lastSection) { html += `<div class="cmdk-section">${c.section}</div>`; lastSection = c.section; }
    html += `<div class="cmdk-item${i === 0 ? ' highlighted' : ''}" data-i="${i}" role="option">${c.icon}<span>${c.label}</span>${c.kbd ? `<kbd>${c.kbd}</kbd>` : ''}</div>`;
  });
  cmdkList.innerHTML = html;
}
function cmdkHighlight() {
  cmdkList.querySelectorAll('.cmdk-item').forEach(el =>
    el.classList.toggle('highlighted', Number(el.dataset.i) === cmdkIdx));
  const el = cmdkList.querySelector(`[data-i="${cmdkIdx}"]`);
  if (el) el.scrollIntoView({ block: 'nearest' });
}
cmdkInput.addEventListener('input', () => renderCmdk(cmdkInput.value));
cmdkInput.addEventListener('keydown', e => {
  if (e.key === 'ArrowDown') { e.preventDefault(); cmdkIdx = Math.min(cmdkIdx + 1, cmdkFiltered.length - 1); cmdkHighlight(); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); cmdkIdx = Math.max(cmdkIdx - 1, 0); cmdkHighlight(); }
  else if (e.key === 'Enter') { e.preventDefault(); const c = cmdkFiltered[cmdkIdx]; if (c) { closeCmdk(); c.run(); } }
  else if (e.key === 'Escape') closeCmdk();
});
cmdkList.addEventListener('click', e => {
  const item = e.target.closest('.cmdk-item');
  if (!item) return;
  const c = cmdkFiltered[Number(item.dataset.i)];
  if (c) { closeCmdk(); c.run(); }
});
cmdkBackdrop.addEventListener('mousedown', e => { if (e.target === cmdkBackdrop) closeCmdk(); });
document.getElementById('cmdk-btn').addEventListener('click', openCmdk);

// ── Global keyboard shortcuts ───────────────────────────────────────────
const inField = el => el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable);
document.addEventListener('keydown', e => {
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
    e.preventDefault();
    cmdkBackdrop.hidden ? openCmdk() : closeCmdk();
    return;
  }
  if (e.key === 'Escape') { closeCmdk(); return; }
  if (inField(e.target) || e.metaKey || e.ctrlKey || e.altKey) return;
  switch (e.key) {
    case '1': location.hash = '#/'; break;
    case '2': location.hash = '#/benchmarks'; break;
    case '3': location.hash = '#/architecture'; break;
    case '4': location.hash = '#/observability'; break;
    case '5': location.hash = '#/recruiter'; break;
    case 't': document.getElementById('theme-toggle').click(); break;
    case 'r': doRefresh(); break;
    case '/': e.preventDefault();
      location.hash = '#/';
      setTimeout(() => document.getElementById('player-search').focus(), 60);
      break;
  }
});

// ── Boot ────────────────────────────────────────────────────────────────
checkHealth();
navigate();
startAutoRefresh();

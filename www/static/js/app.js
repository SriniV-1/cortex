// App shell for the single-page Courtside dashboard: theme, toasts,
// server status, clock, and the 30s auto-refresh cadence. The dashboard
// itself lives in views/overview.js.

import { api } from './api.js';
import { overviewView } from './views/overview.js';

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

// ── Server status ───────────────────────────────────────────────────────
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
const REFRESH_PERIOD = 30;
let refreshTimer = null, countdownTimer = null, countdownSecs = REFRESH_PERIOD;

function doRefresh() {
  checkHealth();
  overviewView.refresh();
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

// ── Boot ────────────────────────────────────────────────────────────────
checkHealth();
overviewView.mount();
startAutoRefresh();

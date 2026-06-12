// Thin fetch wrappers over the Cortex HTTP API. Same-origin, no auth needed
// for read endpoints. Each helper returns parsed JSON or throws.

const API = '';

async function getJSON(path) {
  const r = await fetch(API + path);
  if (!r.ok) throw new Error(`${path} → ${r.status}`);
  return r.json();
}

export const api = {
  health:      () => fetch(API + '/health'),
  metricsText: async () => {
    const r = await fetch(API + '/metrics');
    if (!r.ok) throw new Error('/metrics → ' + r.status);
    return r.text();
  },
  stats:        () => getJSON('/api/stats'),
  leaderboard:  (stat) => getJSON('/api/leaderboard?stat=' + encodeURIComponent(stat)),
  recentGames:  (type) => getJSON('/api/games/recent' + (type && type !== 'all' ? '?type=' + type : '')),
  scoreboard:   () => getJSON('/api/scoreboard'),
  searchPlayers:(q) => getJSON('/api/players/search?q=' + encodeURIComponent(q)),
  searchGames:  (team) => getJSON('/api/games/search?team=' + encodeURIComponent(team)),
  elo:          () => getJSON('/api/elo'),
  eloHistory:   () => getJSON('/api/elo/history'),
  indexStatus:  () => getJSON('/api/index/status'),
  similar:      ({ home, away, period, clock, k = 10 }) =>
    getJSON(`/api/similar?score_home=${home}&score_away=${away}&period=${period}&clock=${clock}&k=${k}`),
};

// Parse a Prometheus text exposition into { metric_name: value }.
export function parsePrometheus(text) {
  const out = {};
  for (const line of text.split('\n')) {
    if (!line || line.startsWith('#')) continue;
    const sp = line.lastIndexOf(' ');
    if (sp <= 0) continue;
    const name = line.slice(0, sp).trim();
    const val = Number(line.slice(sp + 1));
    if (!Number.isNaN(val)) out[name] = val;
  }
  return out;
}

// Live WebSocket subscription to a game's play-by-play stream.
export function liveStream(gameId, onEvent, onClose) {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/live/${gameId}`);
  ws.onmessage = (e) => { try { onEvent(JSON.parse(e.data)); } catch { /* malformed frame */ } };
  ws.onclose = () => onClose && onClose();
  return ws;
}

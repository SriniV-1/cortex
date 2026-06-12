// Overview view — the live dashboard. Ports every behavior of the previous
// single-page UI: live WebSocket stream with win probability, leaderboards,
// recent games + live scoreboard, SIMD similarity search with Elo-blended
// win probability, team autofill, Elo rankings/modal, and Elo history charts.

import { api, liveStream } from '../api.js';
import { lineChart, hbarChart, countUp, fmtNum } from '../charts.js';
import { showToast, onThemeChange } from '../app.js';

const esc = s => String(s ?? '').replace(/[&<>"']/g, c =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

let currentStat = 'ppg';
let currentGameType = 'all';
let countersAnimated = false;

// ── Small utilities ─────────────────────────────────────────────────────
const $ = id => document.getElementById(id);
const pad2 = n => String(n).padStart(2, '0');
const nowStr = () => { const d = new Date(); return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`; };

function skeletonRows(n) {
  let html = '';
  for (let i = 0; i < n; i++) {
    const w = 70 + Math.random() * 30;
    html += `<div class="skeleton-row"><div class="skeleton-circle skeleton"></div><div class="skeleton-line skeleton" style="width:${w.toFixed(0)}%"></div></div>`;
  }
  return html;
}

// ── Hero metrics ────────────────────────────────────────────────────────
async function loadStreamStats() {
  try {
    const t = await api.metricsText();
    const m = t.match(/cortex_events_processed (\d+)/);
    if (m) {
      const v = parseInt(m[1]);
      $('chip-stream').textContent = fmtNum(v);
      $('side-events').textContent = fmtNum(v);
    }
  } catch { /* metrics offline — hero keeps placeholder */ }
}

async function loadDbStats() {
  try {
    const d = await api.stats();
    const set = (id, v) => {
      if (!v) return;
      if (!countersAnimated) countUp($(id), v, { fmt: x => fmtNum(Math.round(x)) });
      else $(id).textContent = fmtNum(v);
    };
    set('chip-games', d.total_games);
    set('chip-players', d.total_players);
    set('chip-events', d.total_events);
    countersAnimated = true;
  } catch {
    showToast('Failed to fetch database stats', 'error');
  }
}

// ── Leaderboard ─────────────────────────────────────────────────────────
const STAT_CONFIG = {
  ppg:    { label: 'PPG', col: 'ppg',    fmt: v => v.toFixed(1) },
  rpg:    { label: 'RPG', col: 'rpg',    fmt: v => v.toFixed(1) },
  spg:    { label: 'SPG', col: 'spg',    fmt: v => v.toFixed(1) },
  bpg:    { label: 'BPG', col: 'bpg',    fmt: v => v.toFixed(1) },
  fg_pct: { label: 'FG%', col: 'fg_pct', fmt: v => v.toFixed(1) + '%' },
  ft_pct: { label: 'FT%', col: 'ft_pct', fmt: v => v.toFixed(1) + '%' },
};

async function loadLeaderboard(stat) {
  stat = stat || currentStat;
  const container = $('leaderboard-container');
  if (!container.querySelector('.lb-table')) container.innerHTML = skeletonRows(8);
  try {
    const resp = await api.leaderboard(stat);
    const data = resp.players || resp;
    const cfg = STAT_CONFIG[stat] || STAT_CONFIG.ppg;
    const maxVal = data.length > 0 ? (data[0][cfg.col] || 1) : 1;
    let html = `<table class="lb-table"><thead><tr><th>Player</th><th>G</th><th>PTS</th><th>REB</th><th>${cfg.label}</th></tr></thead><tbody>`;
    data.forEach(p => {
      const val = p[cfg.col] || 0;
      const pct = Math.round((val / maxVal) * 100);
      html += `<tr>
        <td><span class="rank${p.rank <= 3 ? ' top' : ''}">${p.rank}</span><span class="player-name">${esc(p.name)}</span><span class="team-badge">${esc(p.team)}</span>${p.pos ? `<span class="pos-badge">${esc(p.pos)}</span>` : ''}</td>
        <td class="mono">${p.games}</td>
        <td class="mono">${(p.pts || 0).toLocaleString()}</td>
        <td class="mono">${(p.reb || 0).toLocaleString()}</td>
        <td><div class="ppg-bar"><span class="stat-highlight">${cfg.fmt(val)}</span><div class="bar-bg"><div class="bar-fill" style="width:${pct}%"></div></div></div></td>
      </tr>`;
    });
    container.innerHTML = html + '</tbody></table>';
  } catch {
    container.innerHTML = '<div class="error-msg">Failed to fetch leaderboard</div>';
    showToast('Leaderboard failed to load', 'error');
  }
}

// ── Recent games + live scoreboard ──────────────────────────────────────
let subscribedGameId = null;

async function loadGames(type) {
  type = type || currentGameType;
  const container = $('games-container');
  if (!container.querySelector('.game-item')) container.innerHTML = skeletonRows(6);
  try {
    const [recentResp, sb] = await Promise.all([
      api.recentGames(type),
      api.scoreboard().catch(() => null),
    ]);
    const recentData = recentResp.data || recentResp;
    const scoreboardGames = sb ? (sb.games || []).filter(g => g.status === 1 || g.status === 2) : [];

    let html = '';
    const liveGames = [];

    scoreboardGames.forEach(g => {
      const isLive = g.status === 2;
      if (isLive) liveGames.push(g);
      const [sc, sl] = isLive ? ['status-live', 'Live'] : ['status-sched', 'Scheduled'];
      html += `<div class="game-item${isLive ? ' live-game' : ''}"${isLive ? ` data-live='${esc(JSON.stringify({
        id: g.game_id, label: `${g.away} @ ${g.home}`, hs: g.home_score || 0, as: g.away_score || 0,
        period: g.period || 0, clock: g.game_clock || '',
      }))}'` : ''}>
        <div>
          <div class="game-matchup"><span>${esc(g.away)}</span><span class="vs">@</span><span>${esc(g.home)}</span></div>
          <div class="game-date-small">Today</div>
          ${isLive ? '<div class="game-live-tip">Click to watch live</div>' : ''}
        </div>
        <div class="game-right">
          <div class="game-score"><span>${g.away_score}</span><span class="sep">–</span><span>${g.home_score}</span></div>
          <span class="status-badge ${sc}">${sl}</span>
        </div>
      </div>`;
    });

    recentData.forEach(g => {
      const st = g.status || 3;
      const isFinal = st >= 3, isLive = st === 2;
      const homeWon = isFinal && g.home_score > g.away_score;
      const awayWon = isFinal && g.away_score > g.home_score;
      const [badgeCls, badgeLabel] = isFinal ? ['status-final', 'Final'] : isLive ? ['status-live', 'Live'] : ['status-sched', 'Scheduled'];
      const scoreHtml = (isFinal || isLive)
        ? `<div class="game-score"><span class="${awayWon ? 'winner' : ''}">${g.away_score}</span><span class="sep">–</span><span class="${homeWon ? 'winner' : ''}">${g.home_score}</span></div>`
        : '<div class="game-score tbd">vs</div>';
      html += `<div class="game-item">
        <div>
          <div class="game-matchup"><span>${esc(g.away_name)}</span><span class="vs">@</span><span>${esc(g.home_name)}</span></div>
          <div class="game-date-small">${esc(g.date)}</div>
        </div>
        <div class="game-right">${scoreHtml}<span class="status-badge ${badgeCls}">${badgeLabel}</span></div>
      </div>`;
    });

    container.innerHTML = html || '<div class="sim-note">No recent games found</div>';
    renderTicker(recentData, scoreboardGames);

    if (liveGames.length > 0 && !subscribedGameId) {
      const g = liveGames[0];
      subscribeToGame(g.game_id, `${g.away} @ ${g.home}`, g.home_score, g.away_score, g.period, g.game_clock);
    }
  } catch {
    container.innerHTML = '<div class="error-msg">Failed to fetch games</div>';
    showToast('Recent games failed to load', 'error');
  }
}

// ── Bottom-line ticker — real finals + live games, looped twice for a
//    seamless marquee ─────────────────────────────────────────────────────
function renderTicker(recent, live) {
  const track = $('ticker-track');
  if (!track) return;
  const items = [];
  live.forEach(g => {
    if (g.status !== 2) return;
    items.push(`<span>${esc(g.away)} ${g.away_score} <span class="tk-sep">–</span> ${esc(g.home)} ${g.home_score} <span class="tk-final" style="color:var(--green)">LIVE</span></span>`);
  });
  recent.filter(g => (g.status || 3) >= 3).slice(0, 14).forEach(g => {
    const awayWon = g.away_score > g.home_score;
    items.push(`<span><span class="${awayWon ? 'tk-win' : ''}">${esc(g.away_name)} ${g.away_score}</span> <span class="tk-sep">–</span> <span class="${!awayWon ? 'tk-win' : ''}">${esc(g.home_name)} ${g.home_score}</span> <span class="tk-final">FINAL</span></span>`);
  });
  if (!items.length) { track.innerHTML = '<span>Cortex — every play, quantified</span>'; return; }
  const strip = items.join('<span class="tk-sep">▪</span>');
  track.innerHTML = strip + '<span class="tk-sep">▪</span>' + strip;
}

// click-to-subscribe on live games (delegated; replaces inline onclick)
function bindGameClicks() {
  $('games-container').addEventListener('click', e => {
    const item = e.target.closest('.game-item.live-game');
    if (!item || !item.dataset.live) return;
    try {
      const g = JSON.parse(item.dataset.live);
      subscribeToGame(g.id, g.label, g.hs, g.as, g.period, g.clock);
    } catch { /* stale payload */ }
  });
}

// ── Live stream (WebSocket) ─────────────────────────────────────────────
let ws = null, streamEvents = 0;
const MAX_STREAM = 5;
let liveHomeTeam = '', liveAwayTeam = '';

const formatClock = secs => {
  if (secs == null || secs < 0) return '--:--';
  return Math.floor(secs / 60) + ':' + String(secs % 60).padStart(2, '0');
};
const formatPeriod = p => (!p || p <= 0) ? '--' : (p <= 4 ? 'Q' + p : 'OT' + (p - 4));
const parseGameClock = iso => {
  if (!iso) return null;
  const m = iso.match(/PT(\d+)M([\d.]+)S/);
  return m ? parseInt(m[1]) * 60 + Math.floor(parseFloat(m[2])) : null;
};

function updateClockDisplay(period, clockSecs) {
  $('live-clock-period').textContent = formatPeriod(period);
  $('live-clock-time').textContent = formatClock(clockSecs);
}
function updateWinProbDisplay(homePct) {
  $('live-wp-home').textContent = homePct + '%';
  $('live-wp-away').textContent = (100 - homePct) + '%';
  $('live-wp-fill').style.width = homePct + '%';
}

function subscribeToGame(gameId, label, initialHome, initialAway, period, gameClock) {
  subscribedGameId = gameId;
  if (ws) ws.close();
  ws = liveStream(gameId, addStreamEvent, () => { /* server will reconnect on refresh */ });

  const parts = (label || '').split(/\s*@\s*/);
  liveAwayTeam = (parts[0] || '').trim();
  liveHomeTeam = (parts[1] || '').trim();

  $('stream-idle').hidden = true;
  $('stream-active').hidden = false;
  $('live-away-name').textContent = liveAwayTeam || '---';
  $('live-home-name').textContent = liveHomeTeam || '---';
  $('live-wp-home-label').textContent = liveHomeTeam || 'HOME';
  $('live-wp-away-label').textContent = liveAwayTeam || 'AWAY';

  const hs = initialHome || 0, as = initialAway || 0;
  $('live-home-score').textContent = hs;
  $('live-away-score').textContent = as;
  $('live-home-score').classList.toggle('leading', hs > as);
  $('live-away-score').classList.toggle('leading', as > hs);

  updateClockDisplay(period || 0, typeof gameClock === 'string' ? parseGameClock(gameClock) : gameClock);

  const homeElo = eloRatings[liveHomeTeam], awayElo = eloRatings[liveAwayTeam];
  if (homeElo && awayElo) {
    const diff = (homeElo.rating + 100) - awayElo.rating;
    updateWinProbDisplay(Math.round(100 / (1 + Math.pow(10, -diff / 400))));
  } else {
    updateWinProbDisplay(50);
  }

  $('stream-box').innerHTML = '<div class="stream-empty">Subscribed — waiting for plays…</div>';
  streamEvents = 0;
}

const ACTION_META = {
  0: { tag: '·',   name: 'Unknown' },
  1: { tag: '2PT', name: '2-PT' },
  2: { tag: '3PT', name: '3-PT' },
  3: { tag: 'FT',  name: 'Free throw' },
  4: { tag: 'REB', name: 'Rebound' },
  5: { tag: 'AST', name: 'Assist' },
  6: { tag: 'TOV', name: 'Turnover' },
  7: { tag: 'FOUL', name: 'Foul' },
  8: { tag: 'SUB', name: 'Substitution' },
  9: { tag: 'TO',  name: 'Timeout' },
  10:{ tag: 'JB',  name: 'Jump ball' },
  11:{ tag: 'PER', name: 'Period' },
  127:{ tag: '·',  name: 'Other' },
};

function popScore(el, next) {
  if (el.textContent !== String(next)) {
    el.classList.remove('pop');
    void el.offsetWidth; // restart the animation
    el.classList.add('pop');
  }
  el.textContent = next;
}

function addStreamEvent(ev) {
  if (ev.score_home !== undefined) {
    popScore($('live-home-score'), ev.score_home);
    popScore($('live-away-score'), ev.score_away);
    $('live-home-score').classList.toggle('leading', ev.score_home > ev.score_away);
    $('live-away-score').classList.toggle('leading', ev.score_away > ev.score_home);
    // keep the similarity form in sync with the live game unless edited
    const h = $('sim-home'), a = $('sim-away');
    if (h && !h.dataset.userEdited) { h.value = ev.score_home; a.value = ev.score_away; }
  }
  if (ev.win_prob !== undefined) updateWinProbDisplay(Math.round(ev.win_prob * 100));
  if (ev.period !== undefined || ev.clock_secs !== undefined) updateClockDisplay(ev.period, ev.clock_secs);

  const box = $('stream-box');
  const empty = box.querySelector('.stream-empty');
  if (empty) empty.remove();

  const meta = ACTION_META[ev.action] || ACTION_META[0];
  const isShot = ev.action >= 1 && ev.action <= 3;
  const made = isShot && ev.shot_made;
  const missed = isShot && !ev.shot_made;
  let detail = meta.name;
  if (made) detail += ' made';
  else if (missed) detail += ' missed';

  let wpHtml = '';
  if (ev.win_prob !== undefined) {
    const pct = Math.round(ev.win_prob * 100);
    const cls = pct >= 60 ? 'high' : pct >= 40 ? 'mid' : 'low';
    wpHtml = `<span class="ev-wp ${cls}">${pct}%</span>`;
  }
  const timeStr = ev.period !== undefined && ev.clock_secs !== undefined
    ? formatPeriod(ev.period) + ' ' + formatClock(ev.clock_secs) : nowStr();
  const scoreStr = ev.score_home !== undefined ? `${ev.score_away} – ${ev.score_home}` : '';

  const div = document.createElement('div');
  div.className = 'stream-event';
  div.innerHTML =
    `<span class="ev-time">${timeStr}</span>` +
    `<span class="ev-tag${made ? ' made' : missed ? ' miss' : ''}">${meta.tag}</span>` +
    `<span class="ev-detail">${detail}</span>` +
    `<span class="ev-score">${scoreStr}</span>` + wpHtml;
  box.insertBefore(div, box.firstChild);
  while (box.children.length > MAX_STREAM) box.removeChild(box.lastChild);
  streamEvents++;
}

// ── Similarity search ───────────────────────────────────────────────────
async function checkIndexStatus() {
  const dot = $('sim-index-dot'), label = $('sim-index-label'), btn = $('sim-btn');
  try {
    const d = await api.indexStatus();
    if (d.loaded) {
      dot.style.color = 'var(--green)';
      label.textContent = (d.size >= 1e6 ? (d.size / 1e6).toFixed(1) + 'M' : (d.size / 1000).toFixed(0) + 'K') + ' states indexed';
      btn.disabled = false;
    } else {
      dot.style.color = 'var(--amber)';
      label.textContent = 'building…';
      btn.disabled = true;
      setTimeout(checkIndexStatus, 5000);
    }
  } catch {
    label.textContent = 'index unavailable';
    btn.disabled = true;
    showToast('Similarity index unavailable', 'warn');
  }
}

async function runSimilarity() {
  const btn = $('sim-btn'), meta = $('sim-meta'), container = $('sim-results');
  const home = parseInt($('sim-home').value) || 0;
  const away = parseInt($('sim-away').value) || 0;
  const period = parseInt($('sim-period').value) || 1;
  const clock = parseInt($('sim-clock').value);
  btn.disabled = true; btn.textContent = 'Searching…'; meta.textContent = '';
  container.innerHTML = '<div class="sim-note">Scanning the full feature store…</div>';
  try {
    const data = await api.similar({ home, away, period, clock });
    meta.textContent = `Scanned ${(data.index_size / 1e6).toFixed(1)}M events in ${data.query_ms.toFixed(1)} ms — SIMD brute force`;
    if (!data.results || !data.results.length) {
      container.innerHTML = '<div class="sim-note">No results.</div>';
      renderWinProb([]);
      return;
    }
    renderWinProb(data.results);
    container.innerHTML = data.results.map(m => {
      const s = (m.similarity * 100).toFixed(1);
      const o = m.home_won ? '<span class="outcome-win">Home won</span>' : '<span class="outcome-loss">Away won</span>';
      return `<div class="sim-card">
        <div class="sim-card-header"><span class="sim-matchup">${esc(m.away)} @ ${esc(m.home)}</span><span class="sim-similarity">${s}%</span></div>
        <div class="sim-score">${m.score_away} – ${m.score_home}</div>
        <div class="sim-details"><span>Q${m.period} · ${esc(m.date)}</span>${o}</div>
      </div>`;
    }).join('');
  } catch {
    container.innerHTML = '<div class="error-msg">Search failed — is the server running?</div>';
    showToast('Similarity search failed', 'error');
  } finally {
    btn.disabled = false; btn.textContent = 'Find similar';
  }
}

function renderWinProb(results) {
  const container = $('win-prob-container');
  if (!results || results.length === 0) { container.innerHTML = ''; return; }

  const homeWins = results.filter(r => r.home_won).length;
  const histPct = Math.round((homeWins / results.length) * 100);

  const homeTeam = ($('sim-home-team').value || '').toUpperCase().trim();
  const awayTeam = ($('sim-away-team').value || '').toUpperCase().trim();
  const homeElo = eloRatings[homeTeam], awayElo = eloRatings[awayTeam];

  let eloPct = null, eloHtml = '';
  if (homeElo && awayElo) {
    const diff = (homeElo.rating + 100) - awayElo.rating;
    eloPct = Math.round(100 / (1 + Math.pow(10, -diff / 400)));
    eloHtml = `<div class="wp-elo-row">
      <span class="lead">Elo strength</span>
      <span>${esc(homeTeam)} <span class="mono">${Math.round(homeElo.rating)}</span> (${homeElo.wins}W–${homeElo.losses}L)</span>
      <span class="sep">vs</span>
      <span>${esc(awayTeam)} <span class="mono">${Math.round(awayElo.rating)}</span> (${awayElo.wins}W–${awayElo.losses}L)</span>
      <span class="tail">Elo: ${eloPct}% home</span>
    </div>`;
  }

  const combinedPct = eloPct !== null ? Math.round(0.6 * histPct + 0.4 * eloPct) : histPct;
  const awayPct = 100 - combinedPct;

  container.innerHTML = `<div class="win-prob-banner">
    <div><div class="wp-label">${esc(homeTeam) || 'Home'} win prob</div><div class="wp-value" style="color:var(--green)">${combinedPct}%</div></div>
    <div style="flex:1">
      <div class="wp-bar-outer"><div class="wp-bar-fill" style="width:${combinedPct}%"></div></div>
      <div class="wp-sides"><span>${esc(homeTeam) || 'Home'} ${combinedPct}%</span><span>${esc(awayTeam) || 'Away'} ${awayPct}%</span></div>
    </div>
    <div style="text-align:right"><div class="wp-label">${esc(awayTeam) || 'Away'} win prob</div><div class="wp-value" style="color:var(--red)">${awayPct}%</div></div>
  </div>
  ${eloPct !== null ? `<div class="wp-note">Combined: 60% historical outcomes (${histPct}%) + 40% Elo expectation (${eloPct}%)</div>` : ''}
  ${eloHtml}`;
}

// ── Search overlays ─────────────────────────────────────────────────────
function setupSearch(inputId, resultsId, minLen, fetcher, renderItems) {
  const input = $(inputId), results = $(resultsId);
  let timer = null;
  const setOpen = open => {
    results.classList.toggle('open', open);
    input.setAttribute('aria-expanded', String(open));
  };
  input.addEventListener('input', () => {
    clearTimeout(timer);
    const q = input.value.trim();
    if (q.length < minLen) { setOpen(false); return; }
    timer = setTimeout(async () => {
      try {
        results.innerHTML = renderItems(await fetcher(q), q);
        setOpen(true);
      } catch { /* search backend hiccup — keep overlay closed */ }
    }, 300);
  });
  input.addEventListener('blur', () => setTimeout(() => setOpen(false), 200));
  input.addEventListener('focus', () => {
    if (results.innerHTML && input.value.trim().length >= minLen) setOpen(true);
  });
  input.addEventListener('keydown', e => { if (e.key === 'Escape') setOpen(false); });
}

const renderPlayerResults = data => {
  const players = data.players || [];
  if (!players.length) return '<div class="search-result-item"><span class="sr-meta">No players found</span></div>';
  return players.map(p => `<div class="search-result-item" role="option">
    <div class="sr-name">${esc(p.name)}${p.team ? ` <span class="team-badge">${esc(p.team)}</span>` : ''}${p.pos ? ` <span class="pos-badge">${esc(p.pos)}</span>` : ''}</div>
    <div class="sr-stats">
      <span><span class="sr-stat-val">${p.ppg}</span> PPG</span>
      <span><span class="sr-stat-val">${p.rpg}</span> RPG</span>
      <span><span class="sr-stat-val">${p.spg}</span> SPG</span>
      <span><span class="sr-stat-val">${p.bpg}</span> BPG</span>
      <span><span class="sr-stat-val">${p.fg_pct}%</span> FG</span>
      <span>${p.games}G</span>
    </div>
  </div>`).join('');
};

const renderGameResults = (data, q) => {
  const games = data.games || [];
  if (!games.length) return `<div class="search-result-item"><span class="sr-meta">No games found for ${esc(q.toUpperCase())}</span></div>`;
  return games.slice(0, 20).map(g => {
    const homeWon = g.home_score > g.away_score;
    const playoffs = g.season_type === 'Playoffs' ? ' <span class="pos-badge">PLAYOFFS</span>' : '';
    return `<div class="search-result-item" role="option">
      <div class="sr-name">${esc(g.away)} @ ${esc(g.home)}${playoffs}</div>
      <div class="sr-meta">${esc(g.date)} · <span class="${!homeWon ? 'outcome-win' : ''}">${g.away_score}</span> – <span class="${homeWon ? 'outcome-win' : ''}">${g.home_score}</span></div>
    </div>`;
  }).join('');
};

// ── Elo ─────────────────────────────────────────────────────────────────
let eloRatings = {};   // tricode → { rating, wins, losses, ... }
let _eloTeamsCache = [];
let _autofillReady = false;

async function fetchEloMap() {
  try {
    const d = await api.elo();
    (d.data || d.teams || []).forEach(t => { eloRatings[t.tricode] = t; });
    if (!_autofillReady && Object.keys(eloRatings).length > 0) {
      _autofillReady = true;
      setupTeamAutofill('sim-home-team', 'home-team-dropdown');
      setupTeamAutofill('sim-away-team', 'away-team-dropdown');
    }
  } catch { showToast('Failed to load Elo data', 'warn'); }
}

function setupTeamAutofill(inputId, dropdownId) {
  const input = $(inputId), dropdown = $(dropdownId);
  let highlightIdx = -1;

  function renderDropdown(filter) {
    const teams = Object.keys(eloRatings).sort((a, b) => eloRatings[b].rating - eloRatings[a].rating);
    const filtered = filter ? teams.filter(t => t.startsWith(filter)) : teams;
    if (!filtered.length) { dropdown.classList.remove('open'); return; }
    highlightIdx = -1;
    dropdown.innerHTML = filtered.map((t, i) => {
      const te = eloRatings[t];
      return `<div class="team-autofill-item" role="option" data-tricode="${esc(t)}" data-idx="${i}">
        <span class="taf-tricode">${esc(t)}</span><span class="taf-rating mono">${Math.round(te.rating)}</span><span class="taf-record">${te.wins}W–${te.losses}L</span>
      </div>`;
    }).join('');
    dropdown.classList.add('open');
  }

  const refresh = () => renderDropdown(input.value.trim().toUpperCase());
  input.addEventListener('focus', refresh);
  input.addEventListener('input', () => { input.value = input.value.toUpperCase(); refresh(); });
  input.addEventListener('keydown', e => {
    const items = dropdown.querySelectorAll('.team-autofill-item');
    if (!items.length || !dropdown.classList.contains('open')) return;
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      highlightIdx = e.key === 'ArrowDown'
        ? Math.min(highlightIdx + 1, items.length - 1)
        : Math.max(highlightIdx - 1, 0);
      items.forEach((el, i) => el.classList.toggle('highlighted', i === highlightIdx));
      items[highlightIdx].scrollIntoView({ block: 'nearest' });
    } else if (e.key === 'Enter') {
      e.preventDefault();
      if (highlightIdx >= 0) {
        input.value = items[highlightIdx].dataset.tricode;
        input.dataset.userEdited = '1';
        dropdown.classList.remove('open');
      }
    } else if (e.key === 'Escape') {
      dropdown.classList.remove('open');
    }
  });
  dropdown.addEventListener('mousedown', e => {
    e.preventDefault();
    const item = e.target.closest('.team-autofill-item');
    if (item) {
      input.value = item.dataset.tricode;
      input.dataset.userEdited = '1';
      dropdown.classList.remove('open');
    }
  });
  input.addEventListener('blur', () => setTimeout(() => dropdown.classList.remove('open'), 150));
}

async function loadElo() {
  const container = $('elo-container');
  try {
    const resp = await api.elo();
    const teams = resp.data || resp.teams || [];
    _eloTeamsCache = teams;
    if (!teams.length) { container.innerHTML = '<div class="sim-note">No ratings available</div>'; return; }
    const maxR = teams[0].rating, minR = teams[teams.length - 1].rating;
    const range = maxR - minR || 1;
    $('elo-badge').textContent = `${(resp.games_processed || 0).toLocaleString()} games analyzed`;
    container.innerHTML = '<div class="elo-grid">' + teams.map((t, idx) => `
      <div class="elo-team" data-idx="${idx}" role="button" tabindex="0" aria-label="${esc(t.tricode)} details">
        <span class="elo-rank${t.rank <= 3 ? ' top3' : ''}">${t.rank}</span>
        <div class="elo-info">
          <div class="elo-tricode">${esc(t.tricode)} <span class="elo-rating">${Math.round(t.rating)}</span></div>
          <div class="elo-record">${t.wins}W – ${t.losses}L</div>
        </div>
        <div class="elo-bar-outer"><div class="elo-bar-fill" style="width:${Math.round(((t.rating - minR) / range) * 100)}%"></div></div>
      </div>`).join('') + '</div>';
  } catch {
    container.innerHTML = '<div class="sim-note">Elo ratings unavailable</div>';
    showToast('Elo ratings failed to load', 'error');
  }
}

function openEloModal(idx) {
  const t = _eloTeamsCache[idx];
  if (!t) return;
  const modal = $('elo-modal');
  const delta = t.rating - 1500;
  const deltaStr = (delta >= 0 ? '+' : '') + Math.round(delta);
  const winPct = t.games > 0 ? ((t.wins / t.games) * 100).toFixed(1) : '0.0';
  const maxR = _eloTeamsCache[0].rating, minR = _eloTeamsCache[_eloTeamsCache.length - 1].rating;
  const pct = Math.round(((t.rating - minR) / (maxR - minR || 1)) * 100);

  modal.innerHTML = `
    <div class="elo-modal-header">
      <span class="elo-modal-rank${t.rank <= 3 ? ' top3' : ''}">#${t.rank}</span>
      <div>
        <div><span class="elo-modal-tricode">${esc(t.tricode)}</span><span class="elo-modal-rating">${Math.round(t.rating)}</span></div>
        <div class="elo-modal-record">${t.wins}W – ${t.losses}L</div>
      </div>
      <button class="elo-modal-close" aria-label="Close">&times;</button>
    </div>
    <div class="elo-modal-body">
      <div class="elo-modal-stats">
        <div class="elo-modal-stat"><div class="elo-modal-stat-label">Elo rating</div><div class="elo-modal-stat-value">${Math.round(t.rating)}</div></div>
        <div class="elo-modal-stat"><div class="elo-modal-stat-label">Δ from 1500</div><div class="elo-modal-stat-value ${delta >= 0 ? 'positive' : 'negative'}">${deltaStr}</div></div>
        <div class="elo-modal-stat"><div class="elo-modal-stat-label">Win rate</div><div class="elo-modal-stat-value">${winPct}%</div></div>
        <div class="elo-modal-stat"><div class="elo-modal-stat-label">Games</div><div class="elo-modal-stat-value">${t.games}</div></div>
      </div>
      <div class="elo-modal-bar-section">
        <div class="elo-modal-bar-label"><span>Rating strength</span><span class="mono">${Math.round(t.rating)} / ${Math.round(maxR)}</span></div>
        <div class="elo-modal-bar-outer"><div class="elo-modal-bar-fill" style="width:${pct}%"></div></div>
      </div>
      <div class="elo-modal-calc">
        <strong>How Elo is calculated.</strong> Every team starts at <code>1500</code>; ratings update after each game based on result vs. expectation.<br>
        <strong>K-factor:</strong> <code>K=20</code> regular season, <code>K=32</code> playoffs ·
        <strong>Home court:</strong> <code>+100</code> ·
        <strong>Season reset:</strong> 25% regression to 1500<br>
        <strong>Expected:</strong> <code>E = 1 / (1 + 10^(−(Elo_home + 100 − Elo_away) / 400))</code><br>
        <strong>Update:</strong> <code>New = Old + K × (Result − E)</code>
      </div>
    </div>`;
  modal.querySelector('.elo-modal-close').addEventListener('click', closeEloModal);
  $('elo-modal-backdrop').classList.add('open');
}
function closeEloModal() { $('elo-modal-backdrop').classList.remove('open'); }

// ── Elo history charts (SVG, dependency-free) ───────────────────────────
const TEAM_COLORS = {
  BOS: '#00b25c', NYK: '#1f8fff', PHI: '#2f7bd6', BKN: '#9aa4ba', TOR: '#e0455a',
  MIL: '#1d9e6e', CLE: '#b2433f', CHI: '#e0455a', IND: '#3f6fd8', DET: '#e05252',
  ATL: '#ff5a52', MIA: '#d8385e', ORL: '#2f9fe0', WAS: '#5470c6', CHA: '#7a64d8',
  DEN: '#5b7ec9', MIN: '#4f8fd0', OKC: '#2fa3e8', POR: '#ec5b5b', UTA: '#5470c6',
  GSW: '#3f6fd8', LAC: '#e05252', LAL: '#9d6fe8', PHX: '#8a64d8', SAC: '#a06fd8',
  DAL: '#3f7fd0', HOU: '#e0455a', MEM: '#7d92c9', NOP: '#5470a6', SAS: '#9aa4ba',
};

let eloHistoryData = null;
let selectedTeams = new Set();
let _allTeams = [];

async function loadEloHistory() {
  try {
    const data = await api.eloHistory();
    eloHistoryData = data.snapshots || [];
    _allTeams = [...new Set(eloHistoryData.map(s => s.tricode))].sort();
    selectedTeams = new Set(
      (_eloTeamsCache.length ? _eloTeamsCache.slice(0, 5).map(t => t.tricode) : _allTeams.slice(0, 5)));
    buildChartControls();
    renderTrajectoryChart();
    renderDistributionChart();
  } catch {
    showToast('Elo history chart data unavailable', 'warn');
  }
}

function buildChartControls() {
  const container = $('elo-chart-controls');
  if (container.children.length) return; // build once
  [['Top 5', 5], ['Top 10', 10], ['All 30', 30]].forEach(([label, n], i) => {
    const b = document.createElement('button');
    b.className = 'tab' + (i === 0 ? ' active' : '');
    b.textContent = label;
    b.addEventListener('click', () => {
      selectedTeams = n === 30 ? new Set(_allTeams) : new Set(_eloTeamsCache.slice(0, n).map(t => t.tricode));
      container.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t === b));
      renderTrajectoryChart();
    });
    container.appendChild(b);
  });
}

function renderTrajectoryChart() {
  if (!eloHistoryData || !eloHistoryData.length) return;
  const seasons = [...new Set(eloHistoryData.map(s => s.season))].sort();
  const xLabels = {};
  seasons.forEach(s => { xLabels[s] = `${s}-${String(s + 1).slice(2)}`; });
  const series = [...selectedTeams].map(tricode => ({
    label: tricode,
    color: TEAM_COLORS[tricode] || '#888',
    points: seasons.map(season => {
      const snap = eloHistoryData.find(s => s.season === season && s.tricode === tricode);
      return { x: season, y: snap ? Math.round(snap.rating) : null };
    }),
  }));
  lineChart($('elo-trajectory-chart'), series, { xLabels, height: 300 });
}

function renderDistributionChart() {
  if (!_eloTeamsCache.length) return;
  const sorted = [..._eloTeamsCache].sort((a, b) => b.rating - a.rating);
  const min = Math.round(sorted[sorted.length - 1].rating) - 40;
  const host = $('elo-distribution-chart');
  host.innerHTML = '';
  const list = document.createElement('div');
  list.className = 'hbar';
  const max = Math.round(sorted[0].rating);
  sorted.forEach(t => {
    const pct = ((t.rating - min) / (max - min || 1)) * 100;
    const row = document.createElement('div');
    row.className = 'hbar-row';
    row.title = `${t.tricode}: ${Math.round(t.rating)} (${t.wins}W–${t.losses}L)`;
    row.innerHTML = `
      <span class="hbar-label mono">${esc(t.tricode)}</span>
      <span class="hbar-track"><span class="hbar-fill" style="width:${pct}%;background:${TEAM_COLORS[t.tricode] || 'var(--accent)'}"></span></span>
      <span class="hbar-value mono">${Math.round(t.rating)}</span>`;
    list.appendChild(row);
  });
  host.appendChild(list);
}

// ── View lifecycle ──────────────────────────────────────────────────────
async function refreshData() {
  await Promise.all([loadLeaderboard(), loadGames(), loadStreamStats(), loadDbStats()]);
  await loadElo();
  loadEloHistory();
}

export const overviewView = {
  mount() {
    // tabs
    $('stat-tabs').addEventListener('click', e => {
      const tab = e.target.closest('.tab');
      if (!tab) return;
      document.querySelectorAll('#stat-tabs .tab').forEach(t => {
        t.classList.toggle('active', t === tab);
        t.setAttribute('aria-selected', String(t === tab));
      });
      currentStat = tab.dataset.stat;
      loadLeaderboard(currentStat);
    });
    $('game-type-tabs').addEventListener('click', e => {
      const tab = e.target.closest('.tab');
      if (!tab) return;
      document.querySelectorAll('#game-type-tabs .tab').forEach(t => {
        t.classList.toggle('active', t === tab);
        t.setAttribute('aria-selected', String(t === tab));
      });
      currentGameType = tab.dataset.type;
      loadGames(currentGameType);
    });

    bindGameClicks();
    $('sim-btn').addEventListener('click', runSimilarity);
    document.querySelectorAll('#sim-home,#sim-away,#sim-period,#sim-clock,#sim-home-team,#sim-away-team')
      .forEach(el => el.addEventListener('input', () => { el.dataset.userEdited = '1'; }));

    setupSearch('player-search', 'player-search-results', 2, api.searchPlayers, renderPlayerResults);
    setupSearch('game-search', 'game-search-results', 2,
      q => api.searchGames(q.toUpperCase()), renderGameResults);

    // Elo modal (delegated)
    $('elo-container').addEventListener('click', e => {
      const team = e.target.closest('.elo-team');
      if (team) openEloModal(Number(team.dataset.idx));
    });
    $('elo-container').addEventListener('keydown', e => {
      const team = e.target.closest('.elo-team');
      if (team && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); openEloModal(Number(team.dataset.idx)); }
    });
    $('elo-modal-backdrop').addEventListener('click', e => {
      if (e.target === e.currentTarget) closeEloModal();
    });
    document.addEventListener('keydown', e => { if (e.key === 'Escape') closeEloModal(); });

    onThemeChange(() => {
      // charts read CSS vars at render time — re-render on theme flips
      if (eloHistoryData) { renderTrajectoryChart(); renderDistributionChart(); }
    });

    fetchEloMap();
    refreshData();
    checkIndexStatus();
  },
  show() { /* data persists across navigation; auto-refresh handles updates */ },
  hide() { /* WS stays open intentionally — live game keeps streaming */ },
  refresh: refreshData,
};

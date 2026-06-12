// Dependency-free SVG chart primitives. Charts read their colors from CSS
// custom properties at render time, so a theme switch just needs a re-render.
// All charts are responsive (viewBox) and respect prefers-reduced-motion.

const NS = 'http://www.w3.org/2000/svg';

function el(tag, attrs = {}, children = []) {
  const node = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  for (const c of children) node.appendChild(c);
  return node;
}

function cssVar(name, fallback) {
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return v || fallback;
}

function reducedMotion() {
  return window.matchMedia('(prefers-reduced-motion: reduce)').matches;
}

export function fmtNum(n) {
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(1).replace(/\.0$/, '') + 'M';
  if (n >= 1_000) return (n / 1_000).toFixed(1).replace(/\.0$/, '') + 'K';
  return String(n);
}

// ── Horizontal bar chart with an optional target/threshold line ───────────
// rows: [{ label, value, detail? }], opts: { unit, target, max, accent }
export function hbarChart(container, rows, opts = {}) {
  container.innerHTML = '';
  const unit = opts.unit || '';
  const max = opts.max || Math.max(...rows.map(r => r.value), opts.target || 0) * 1.08;
  const animate = !reducedMotion();

  const list = document.createElement('div');
  list.className = 'hbar';
  rows.forEach((r, i) => {
    const pct = Math.max((r.value / max) * 100, 0.5);
    const row = document.createElement('div');
    row.className = 'hbar-row';
    if (r.detail) row.title = r.detail;
    row.innerHTML = `
      <span class="hbar-label">${r.label}</span>
      <span class="hbar-track">
        <span class="hbar-fill" style="width:${animate ? 0 : pct}%"></span>
        ${opts.target ? `<span class="hbar-target" style="left:${(opts.target / max) * 100}%"></span>` : ''}
      </span>
      <span class="hbar-value mono">${typeof r.value === 'number' ? (r.value < 10 ? r.value.toFixed(1) : fmtNum(r.value)) : r.value}${unit ? ' ' + unit : ''}</span>`;
    list.appendChild(row);
    if (animate) {
      requestAnimationFrame(() =>
        setTimeout(() => { row.querySelector('.hbar-fill').style.width = pct + '%'; }, 40 * i));
    }
  });
  if (opts.target) {
    const legend = document.createElement('div');
    legend.className = 'hbar-legend';
    legend.innerHTML = `<span class="hbar-target-key"></span> target ${opts.target}${unit ? ' ' + unit : ''}`;
    list.appendChild(legend);
  }
  container.appendChild(list);
}

// ── Multi-series line chart (SVG) ──────────────────────────────────────────
// series: [{ label, color, points: [{x, y}] }] — x values shared/sortable.
// opts: { xLabels, yLabel, height, yMin, yMax, hoverFmt }
export function lineChart(container, series, opts = {}) {
  container.innerHTML = '';
  const W = 720, H = opts.height || 280;
  const padL = 46, padR = 14, padT = 12, padB = 28;
  const iw = W - padL - padR, ih = H - padT - padB;

  const allY = series.flatMap(s => s.points.map(p => p.y)).filter(y => y != null);
  const allX = [...new Set(series.flatMap(s => s.points.map(p => p.x)))].sort((a, b) => a - b);
  if (!allY.length || !allX.length) { container.textContent = 'No data'; return; }

  const yMin = opts.yMin ?? Math.floor(Math.min(...allY) / 50) * 50 - 25;
  const yMax = opts.yMax ?? Math.ceil(Math.max(...allY) / 50) * 50 + 25;
  const xMin = allX[0], xMax = allX[allX.length - 1];
  const sx = x => padL + ((x - xMin) / (xMax - xMin || 1)) * iw;
  const sy = y => padT + (1 - (y - yMin) / (yMax - yMin || 1)) * ih;

  const grid = cssVar('--chart-grid', 'rgba(128,128,128,.15)');
  const tick = cssVar('--text-3', '#888');

  const svg = el('svg', { viewBox: `0 0 ${W} ${H}`, class: 'linechart', role: 'img' });

  // gridlines + y ticks
  const ySteps = 4;
  for (let i = 0; i <= ySteps; i++) {
    const yv = yMin + ((yMax - yMin) / ySteps) * i;
    const y = sy(yv);
    svg.appendChild(el('line', { x1: padL, x2: W - padR, y1: y, y2: y, stroke: grid, 'stroke-width': 1 }));
    const t = el('text', { x: padL - 8, y: y + 3.5, 'text-anchor': 'end', class: 'chart-tick' });
    t.textContent = Math.round(yv);
    t.setAttribute('fill', tick);
    svg.appendChild(t);
  }
  // x labels
  const labelEvery = Math.ceil(allX.length / 8);
  allX.forEach((xv, i) => {
    if (i % labelEvery !== 0 && i !== allX.length - 1) return;
    const t = el('text', { x: sx(xv), y: H - 8, 'text-anchor': 'middle', class: 'chart-tick' });
    t.textContent = opts.xLabels ? (opts.xLabels[xv] ?? xv) : xv;
    t.setAttribute('fill', tick);
    svg.appendChild(t);
  });

  // series paths
  series.forEach(s => {
    const pts = s.points.filter(p => p.y != null).sort((a, b) => a.x - b.x);
    if (!pts.length) return;
    const d = pts.map((p, i) => `${i ? 'L' : 'M'}${sx(p.x).toFixed(1)},${sy(p.y).toFixed(1)}`).join(' ');
    const path = el('path', { d, fill: 'none', stroke: s.color, 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' });
    if (!reducedMotion()) {
      path.style.strokeDasharray = '1400';
      path.style.strokeDashoffset = '1400';
      path.style.animation = 'chart-draw .9s ease forwards';
    }
    svg.appendChild(path);
    pts.forEach(p => svg.appendChild(el('circle', { cx: sx(p.x), cy: sy(p.y), r: 2.6, fill: s.color })));
  });

  container.appendChild(svg);

  // hover crosshair + tooltip
  const tip = document.createElement('div');
  tip.className = 'chart-tip';
  tip.hidden = true;
  container.style.position = 'relative';
  container.appendChild(tip);
  const hover = el('line', { y1: padT, y2: H - padB, stroke: tick, 'stroke-width': 1, 'stroke-dasharray': '3 3', opacity: 0 });
  svg.appendChild(hover);

  svg.addEventListener('mousemove', e => {
    const rect = svg.getBoundingClientRect();
    const mx = ((e.clientX - rect.left) / rect.width) * W;
    let best = allX[0], bd = Infinity;
    for (const xv of allX) { const d = Math.abs(sx(xv) - mx); if (d < bd) { bd = d; best = xv; } }
    hover.setAttribute('x1', sx(best)); hover.setAttribute('x2', sx(best));
    hover.setAttribute('opacity', 0.6);
    const items = series
      .map(s => ({ s, p: s.points.find(p => p.x === best && p.y != null) }))
      .filter(o => o.p)
      .sort((a, b) => b.p.y - a.p.y);
    tip.innerHTML = `<div class="chart-tip-title">${opts.xLabels ? (opts.xLabels[best] ?? best) : best}</div>` +
      items.map(o => `<div class="chart-tip-row"><span class="chart-tip-dot" style="background:${o.s.color}"></span>${o.s.label}<span class="mono">${opts.hoverFmt ? opts.hoverFmt(o.p.y) : Math.round(o.p.y)}</span></div>`).join('');
    tip.hidden = false;
    const tx = Math.min((sx(best) / W) * rect.width + 12, rect.width - 170);
    tip.style.left = tx + 'px';
    tip.style.top = '8px';
  });
  svg.addEventListener('mouseleave', () => { tip.hidden = true; hover.setAttribute('opacity', 0); });
}

// ── Sparkline (tiny line for metric cards) ─────────────────────────────────
export function sparkline(container, values, opts = {}) {
  container.innerHTML = '';
  if (values.length < 2) return;
  const W = 120, H = 32, p = 2;
  const min = Math.min(...values), max = Math.max(...values);
  const sx = i => p + (i / (values.length - 1)) * (W - 2 * p);
  const sy = v => p + (1 - (v - min) / (max - min || 1)) * (H - 2 * p);
  const d = values.map((v, i) => `${i ? 'L' : 'M'}${sx(i).toFixed(1)},${sy(v).toFixed(1)}`).join(' ');
  const color = opts.color || cssVar('--accent', '#6e8bff');
  const svg = el('svg', { viewBox: `0 0 ${W} ${H}`, class: 'sparkline', 'aria-hidden': 'true' }, [
    el('path', { d: `${d} L${sx(values.length - 1)},${H} L${sx(0)},${H} Z`, fill: color, opacity: 0.12 }),
    el('path', { d, fill: 'none', stroke: color, 'stroke-width': 1.5 }),
  ]);
  container.appendChild(svg);
}

// ── Animated number counter ────────────────────────────────────────────────
export function countUp(elm, target, { decimals = 0, duration = 1100, fmt } = {}) {
  if (reducedMotion()) { elm.textContent = fmt ? fmt(target) : target.toFixed(decimals); return; }
  const start = performance.now();
  function step(now) {
    const p = Math.min((now - start) / duration, 1);
    const eased = 1 - Math.pow(1 - p, 3);
    const v = target * eased;
    elm.textContent = fmt ? fmt(v) : v.toFixed(decimals);
    if (p < 1) requestAnimationFrame(step);
    else elm.textContent = fmt ? fmt(target) : target.toFixed(decimals);
  }
  requestAnimationFrame(step);
}

// Recruiter view — mostly static HTML in index.html; this module animates
// the scale counters when the view first becomes visible.

import { SCALE } from '../data/benchmarks.js';
import { countUp, fmtNum } from '../charts.js';

export const recruiterView = {
  mount() {
    const host = document.getElementById('rec-scale');
    host.innerHTML = SCALE.map((s, i) => `
      <div class="metric">
        <span class="metric-v mono" data-i="${i}">0</span>
        <span class="metric-l">${s.label}</span>
      </div>`).join('');
    host.querySelectorAll('[data-i]').forEach(el => {
      const s = SCALE[Number(el.dataset.i)];
      countUp(el, s.value, { fmt: v => s.fmt === 'raw' ? String(Math.round(v)) : fmtNum(Math.round(v)) });
    });
  },
  show() {},
  hide() {},
};

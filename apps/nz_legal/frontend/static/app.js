// NZ Legal RAG - extracted from index.html for CSP compliance

// ---- Tab switching ----

const _TAB_IDS = ['ask', 'notable', 'sentencing', 'pg', 'about'];

function showTab(name) {
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
  document.querySelector(`.tab-btn[data-tab="${name}"]`).classList.add('active');
  _TAB_IDS.forEach(id => {
    const el = document.getElementById('tab-' + id);
    if (!el) return;
    el.style.display = (id === name) ? (id === 'ask' ? 'flex' : 'block') : 'none';
  });
}

// ---- Sentencing Tracker ----

function _median(arr) {
  if (!arr.length) return null;
  const s = arr.slice().sort((a, b) => a - b);
  const m = Math.floor(s.length / 2);
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}

function _fmtMonths(m) {
  if (m == null) return 'n/a';
  const y = Math.floor(m / 12);
  const mo = Math.round(m % 12);
  if (y === 0) return `${mo}m`;
  if (mo === 0) return `${y}y`;
  return `${y}y ${mo}m`;
}

function _sentBadge(s) {
  const st = s.sentence_type;
  if (!st) return '';
  const labels = {
    imprisonment: 'Imprisonment', home_detention: 'Home detention',
    community_work: 'Community work', fine: 'Fine', supervision: 'Supervision',
  };
  return `<span class="badge-sentence">${labels[st] || st}</span>`;
}

async function searchSentencing() {
  const btn = document.getElementById('sent-search-btn');
  btn.disabled = true; btn.textContent = 'Searching...';
  const el = document.getElementById('sentencing-results');
  el.innerHTML = '<div class="loading-dots">Searching...</div>';

  const flags = Array.from(document.querySelectorAll('.sent-flag-cb:checked')).map(c => c.value);
  const sentType  = document.getElementById('sent_type').value;
  const minSp     = document.getElementById('sent_min_sp').value;
  const maxSp     = document.getElementById('sent_max_sp').value;
  const minFs     = document.getElementById('sent_min_fs').value;
  const maxFs     = document.getElementById('sent_max_fs').value;
  const gpSel     = document.getElementById('sent_gp').value;
  const court     = document.getElementById('sent_court').value;
  const yearFrom  = document.getElementById('sent_year_from').value;
  const yearTo    = document.getElementById('sent_year_to').value;

  const body = { limit: 50 };
  if (flags.length)  body.flags = flags;
  if (sentType)      body.sentence_type = sentType;
  if (minSp)         body.min_starting_point = parseFloat(minSp);
  if (maxSp)         body.max_starting_point = parseFloat(maxSp);
  if (minFs)         body.min_final_sentence = parseFloat(minFs);
  if (maxFs)         body.max_final_sentence = parseFloat(maxFs);
  if (gpSel === 'yes') body.has_guilty_plea = true;
  if (gpSel === 'no')  body.has_guilty_plea = false;
  if (court)         body.courts = [court];
  if (yearFrom)      body.year_from = parseInt(yearFrom);
  if (yearTo)        body.year_to   = parseInt(yearTo);

  try {
    const res  = await fetch('/sentencing-tracker', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const data = await res.json();

    if (!res.ok) {
      el.innerHTML = `<div class="notable-empty">Error: ${data.detail || 'Something went wrong.'}</div>`;
      return;
    }
    if (!data.length) {
      el.innerHTML = '<div class="notable-empty">No cases match these filters. Try broader criteria.</div>';
      return;
    }

    const sps  = data.map(c => c.sentencing.starting_point_months).filter(v => v != null);
    const fss  = data.map(c => c.sentencing.final_sentence_months).filter(v => v != null);
    const disc = data.map(c => c.sentencing.guilty_plea_discount_pct).filter(v => v != null);
    const hdms = data.map(c => c.sentencing.home_detention_months).filter(v => v != null);

    let statsHtml = `<div class="stats-box">
      <div class="stat-item"><span class="stat-label">Cases</span><span class="stat-value">${data.length}</span></div>`;
    if (sps.length)  statsHtml += `<div class="stat-item"><span class="stat-label">Median starting point</span><span class="stat-value">${_fmtMonths(_median(sps))}</span></div>`;
    if (fss.length)  statsHtml += `<div class="stat-item"><span class="stat-label">Median imprisonment</span><span class="stat-value">${_fmtMonths(_median(fss))}</span></div>`;
    if (hdms.length) statsHtml += `<div class="stat-item"><span class="stat-label">Median home detention</span><span class="stat-value">${_fmtMonths(_median(hdms))}</span></div>`;
    if (disc.length) statsHtml += `<div class="stat-item"><span class="stat-label">Median GP discount</span><span class="stat-value">${_median(disc).toFixed(0)}%</span></div>`;
    statsHtml += '</div>';

    let cardsHtml = `<div class="notable-count">${data.length} case${data.length !== 1 ? 's' : ''} found</div>`;
    for (const c of data) {
      const s = c.sentencing || {};
      const sentFacts = [];
      if (s.starting_point_months != null) sentFacts.push(`<span class="sent-fact"><strong>Start:</strong> ${_fmtMonths(s.starting_point_months)}</span>`);
      if (s.final_sentence_months != null) sentFacts.push(`<span class="sent-fact"><strong>Sentence:</strong> ${_fmtMonths(s.final_sentence_months)}</span>`);
      if (s.home_detention_months != null) sentFacts.push(`<span class="sent-fact"><strong>Home det.:</strong> ${_fmtMonths(s.home_detention_months)}</span>`);
      if (s.community_work_hours != null)  sentFacts.push(`<span class="sent-fact"><strong>Comm. work:</strong> ${s.community_work_hours}h</span>`);
      if (s.guilty_plea_discount_pct != null) sentFacts.push(`<span class="sent-fact"><strong>GP discount:</strong> ${s.guilty_plea_discount_pct}%</span>`);
      if (s.reparation_amount != null) sentFacts.push(`<span class="sent-fact"><strong>Reparation:</strong> $${s.reparation_amount.toLocaleString()}</span>`);
      if (s.fine_amount != null) sentFacts.push(`<span class="sent-fact"><strong>Fine:</strong> $${s.fine_amount.toLocaleString()}</span>`);

      const pillsHtml = (c.flags || []).length
        ? `<div class="flag-pills" style="margin-top:6px">${c.flags.map(f => `<span class="flag-pill">${FLAG_LABELS[f] || f}</span>`).join('')}</div>`
        : '';

      const gpBadge = s.has_guilty_plea ? '<span class="badge-gp">Guilty plea</span>' : '';
      const prevBadge = s.has_previous_convictions ? '<span class="flag-pill">Prior convictions</span>' : '';

      cardsHtml += `
        <div class="sent-card">
          <div class="sent-card-header">
            <div class="notable-card-title">
              <a href="${c.url || '#'}" target="_blank">${c.title || c.case_id}</a>
              <div class="notable-card-meta">${c.court_name} &bull; ${c.date || 'n/d'}</div>
            </div>
            ${_sentBadge(s)}
          </div>
          <div class="sent-facts">${sentFacts.join('')}</div>
          <div style="display:flex;flex-wrap:wrap;gap:5px;">${gpBadge}${prevBadge}</div>
          ${pillsHtml}
        </div>`;
    }

    el.innerHTML = statsHtml + cardsHtml;
  } catch (e) {
    el.innerHTML = '<div class="notable-empty">Could not reach the server. Is the API running?</div>';
  } finally {
    btn.disabled = false; btn.textContent = 'Search Sentencing Tracker';
  }
}

function clearSentencing() {
  document.querySelectorAll('.sent-flag-cb').forEach(cb => cb.checked = false);
  ['sent_type','sent_gp','sent_court'].forEach(id => document.getElementById(id).value = '');
  ['sent_min_sp','sent_max_sp','sent_min_fs','sent_max_fs','sent_year_from','sent_year_to']
    .forEach(id => document.getElementById(id).value = '');
  document.getElementById('sentencing-results').innerHTML =
    '<div class="notable-empty">Select filters above then click Search.</div>';
}

// ---- PG Tracker ----

const _PG_TYPE_LABELS = {
  unjustified_dismissal:  'Unjustified dismissal',
  constructive_dismissal: 'Constructive dismissal',
  disadvantage:           'Disadvantage / good faith',
  harassment:             'Harassment',
  discrimination:         'Discrimination',
  unjustified_action:     'Unjustified action',
};

async function searchPG() {
  const btn = document.getElementById('pg-search-btn');
  btn.disabled = true; btn.textContent = 'Searching...';
  const el = document.getElementById('pg-results');
  el.innerHTML = '<div class="loading-dots">Searching...</div>';

  const grievanceTypes = Array.from(document.querySelectorAll('.pg-type-cb:checked')).map(c => c.value);
  const reinSel    = document.getElementById('pg_reinstatement').value;
  const minContrib = document.getElementById('pg_min_contrib').value;
  const maxContrib = document.getElementById('pg_max_contrib').value;
  const minComp    = document.getElementById('pg_min_comp').value;
  const maxComp    = document.getElementById('pg_max_comp').value;
  const court      = document.getElementById('pg_court').value;
  const yearFrom   = document.getElementById('pg_year_from').value;
  const yearTo     = document.getElementById('pg_year_to').value;

  const body = { limit: 50 };
  if (grievanceTypes.length) body.grievance_types = grievanceTypes;
  if (reinSel === 'yes')     body.reinstatement = true;
  if (reinSel === 'no')      body.reinstatement = false;
  if (minContrib)            body.min_contributory = parseFloat(minContrib);
  if (maxContrib)            body.max_contributory = parseFloat(maxContrib);
  if (minComp)               body.min_compensation = parseFloat(minComp);
  if (maxComp)               body.max_compensation = parseFloat(maxComp);
  if (court)                 body.courts = [court];
  if (yearFrom)              body.year_from = parseInt(yearFrom);
  if (yearTo)                body.year_to   = parseInt(yearTo);

  try {
    const res  = await fetch('/pg-tracker', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const data = await res.json();

    if (!res.ok) {
      el.innerHTML = `<div class="notable-empty">Error: ${data.detail || 'Something went wrong.'}</div>`;
      return;
    }
    if (!data.length) {
      el.innerHTML = '<div class="notable-empty">No cases match these filters. Try broader criteria.</div>';
      return;
    }

    const comps   = data.map(c => (c.penalty || {}).awarded_amount).filter(v => v != null);
    const contribs = data.map(c => (c.pg || {}).contributory_conduct_pct).filter(v => v != null);
    const nReinst  = data.filter(c => (c.pg || {}).reinstatement_ordered === true).length;

    let statsHtml = `<div class="stats-box">
      <div class="stat-item"><span class="stat-label">Cases</span><span class="stat-value">${data.length}</span></div>
      <div class="stat-item"><span class="stat-label">Reinstatement ordered</span><span class="stat-value">${nReinst} / ${data.length}</span></div>`;
    if (comps.length) statsHtml += `<div class="stat-item"><span class="stat-label">Median compensation</span><span class="stat-value">$${_median(comps).toLocaleString(undefined,{maximumFractionDigits:0})}</span></div>`;
    if (contribs.length) statsHtml += `<div class="stat-item"><span class="stat-label">Median contrib. conduct</span><span class="stat-value">${_median(contribs).toFixed(0)}%</span></div>`;
    statsHtml += '</div>';

    let cardsHtml = `<div class="notable-count">${data.length} case${data.length !== 1 ? 's' : ''} found</div>`;
    for (const c of data) {
      const pg = c.pg || {};
      const penalty = c.penalty || {};

      const typePills = (pg.grievance_types || [])
        .map(t => `<span class="pg-type-pill">${_PG_TYPE_LABELS[t] || t.replace(/_/g,' ')}</span>`)
        .join('');

      const reinBadge = pg.reinstatement_ordered === true
        ? '<span class="badge-reinstate">Reinstatement ordered</span>'
        : pg.reinstatement_ordered === false
        ? '<span class="badge-no-reinstate">Reinstatement declined</span>'
        : '';

      const facts = [];
      if (penalty.awarded_amount != null)      facts.push(`<span class="sent-fact"><strong>Compensation:</strong> $${penalty.awarded_amount.toLocaleString()}</span>`);
      if (pg.contributory_conduct_pct != null) facts.push(`<span class="sent-fact"><strong>Contributory conduct:</strong> ${pg.contributory_conduct_pct}% reduction</span>`);
      else if (pg.has_contributory_conduct)    facts.push(`<span class="sent-fact"><em>Contributory conduct discussed</em></span>`);

      cardsHtml += `
        <div class="pg-card">
          <div class="pg-card-header">
            <div class="notable-card-title">
              <a href="${c.url || '#'}" target="_blank">${c.title || c.case_id}</a>
              <div class="notable-card-meta">${c.court_name} &bull; ${c.date || 'n/d'}</div>
            </div>
            ${reinBadge}
          </div>
          ${typePills ? `<div class="pg-types">${typePills}</div>` : ''}
          <div class="sent-facts">${facts.join('')}</div>
        </div>`;
    }

    el.innerHTML = statsHtml + cardsHtml;
  } catch (e) {
    el.innerHTML = '<div class="notable-empty">Could not reach the server. Is the API running?</div>';
  } finally {
    btn.disabled = false; btn.textContent = 'Search PG Tracker';
  }
}

function clearPG() {
  document.querySelectorAll('.pg-type-cb').forEach(cb => cb.checked = false);
  ['pg_reinstatement','pg_court'].forEach(id => document.getElementById(id).value = '');
  ['pg_min_contrib','pg_max_contrib','pg_min_comp','pg_max_comp','pg_year_from','pg_year_to']
    .forEach(id => document.getElementById(id).value = '');
  document.getElementById('pg-results').innerHTML =
    '<div class="notable-empty">Select filters above then click Search.</div>';
}

// ---- Flag definitions (must match ingest/flags.py FLAG_LABELS) ----

const FLAG_LABELS = {
  "self_defence":              "Self-defence",
  "provocation":               "Provocation",
  "diminished_responsibility": "Diminished responsibility",
  "necessity":                 "Necessity defence",
  "duress":                    "Duress",
  "mental_health":             "Mental health",
  "intoxication":              "Intoxication",
  "youth":                     "Youth / young person",
  "tikanga_maori":             "Tikanga Maori",
  "cultural_factors":          "Cultural factors",
  "novel_argument":            "Novel legal argument",
  "jurisdictional_challenge":  "Jurisdictional challenge",
  "procedural_irregularity":   "Procedural irregularity",
  "exemplary_damages":         "Exemplary damages",
  "contempt":                  "Contempt of court",
  "suppressed_identity":       "Suppressed identity",
  "whistleblower":             "Whistleblower / protected disclosure",
  "lack_of_motive":            "Lack of motive",
  "self_represented":          "Self-represented party",
};

// Build flag checkboxes for the Notable Cases tab
(function buildFlagGrid() {
  const grid = document.getElementById('flag-checkboxes');
  for (const [key, label] of Object.entries(FLAG_LABELS)) {
    const lbl = document.createElement('label');
    const cb  = document.createElement('input');
    cb.type  = 'checkbox';
    cb.value = key;
    cb.id    = 'flag_' + key;
    lbl.appendChild(cb);
    lbl.appendChild(document.createTextNode(label));
    grid.appendChild(lbl);
  }
})();

// ---- OSI helpers ----

function osiBadgeClass(osi) {
  if (osi >= 0.80) return 'osi-severe';
  if (osi >= 0.55) return 'osi-high';
  if (osi >= 0.30) return 'osi-mid';
  return 'osi-low';
}

function penaltyBadgeText(penalty) {
  if (!penalty || !penalty.has_data) return null;
  const ct = penalty.court_type;
  if (ct === 'criminal') {
    const osi = penalty.outcome_osi;
    return `OSI ${osi.toFixed(2)} - ${penalty.outcome_label || ''}`;
  }
  if (ct === 'civil_financial' || ct === 'civil_mixed' || ct === 'civil_disciplinary') {
    if (penalty.recovery_rate != null && penalty.awarded_amount != null) {
      const pct = (penalty.recovery_rate * 100).toFixed(0);
      return `$${penalty.awarded_amount.toLocaleString()} (${pct}% of claim)`;
    }
    if (penalty.awarded_amount != null) {
      return `Awarded $${penalty.awarded_amount.toLocaleString()}`;
    }
    if (penalty.remedies && penalty.remedies.length) {
      return penalty.remedies.join(', ');
    }
  }
  if (ct === 'civil_nonfinancial') return penalty.outcome_class || 'outcome';
  if (ct === 'coronal') return 'Coronial finding';
  return null;
}

function cardBorderClass(penalty) {
  if (!penalty || !penalty.has_data) return '';
  if (penalty.court_type === 'criminal') return 'criminal';
  if (['civil_financial', 'civil_mixed', 'civil_disciplinary'].includes(penalty.court_type)) return 'civil';
  return '';
}

// ---- Notable Cases search ----

async function searchNotable() {
  const btn = document.getElementById('notable-search-btn');
  btn.disabled = true;
  btn.textContent = 'Searching...';

  const resultsEl = document.getElementById('notable-results');
  resultsEl.innerHTML = '<div class="loading-dots">Searching...</div>';

  const flags = Array.from(document.querySelectorAll('#flag-checkboxes input:checked')).map(cb => cb.value);
  const minOsi      = document.getElementById('min_osi').value;
  const maxOsi      = document.getElementById('max_osi').value;
  const minAwarded  = document.getElementById('min_awarded').value;
  const maxAwarded  = document.getElementById('max_awarded').value;
  const minRecovery = document.getElementById('min_recovery').value;
  const maxRecovery = document.getElementById('max_recovery').value;
  const court          = document.getElementById('notable_court').value;
  const yearFrom       = document.getElementById('notable_year_from').value;
  const yearTo         = document.getElementById('notable_year_to').value;
  const counselSurname = document.getElementById('counsel_surname').value.trim();
  const crownCounsel   = document.getElementById('crown_counsel').value.trim();

  const body = { limit: 50 };
  if (flags.length)     body.flags           = flags;
  if (minOsi)           body.min_osi     = parseFloat(minOsi);
  if (maxOsi)           body.max_osi     = parseFloat(maxOsi);
  if (minAwarded)       body.min_awarded  = parseFloat(minAwarded);
  if (maxAwarded)       body.max_awarded  = parseFloat(maxAwarded);
  if (minRecovery)      body.min_recovery = parseFloat(minRecovery);
  if (maxRecovery)      body.max_recovery = parseFloat(maxRecovery);
  if (court)            body.courts          = [court];
  if (yearFrom)         body.year_from       = parseInt(yearFrom);
  if (yearTo)           body.year_to         = parseInt(yearTo);
  if (counselSurname)   body.counsel_surname = counselSurname;
  if (crownCounsel)     body.crown_counsel   = crownCounsel;

  try {
    const res  = await fetch('/notable', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const data = await res.json();

    if (!res.ok) {
      resultsEl.innerHTML = `<div class="notable-empty">Error: ${data.detail || 'Something went wrong.'}</div>`;
      return;
    }

    if (!data.length) {
      resultsEl.innerHTML = '<div class="notable-empty">No cases match these filters. Try broader criteria.</div>';
      return;
    }

    let html = `<div class="notable-count">${data.length} case${data.length !== 1 ? 's' : ''} found</div>`;
    for (const c of data) {
      const penalty    = c.penalty || {};
      const badgeText  = penaltyBadgeText(penalty);
      const borderCls  = cardBorderClass(penalty);
      const osiVal     = penalty.outcome_osi;
      const badgeCls   = osiVal != null ? osiBadgeClass(osiVal) : 'osi-low';

      let gapHtml = '';
      if (penalty.gap != null && penalty.prosecution_label) {
        const dir = penalty.gap_class === 'heavier' ? 'heavier than' : (penalty.gap_class === 'lighter' ? 'lighter than' : 'matches');
        gapHtml = `<div class="penalty-line">Prosecution sought: <strong>${penalty.prosecution_label}</strong> (outcome ${dir} sought)</div>`;
      }

      const pillsHtml = c.flags.length
        ? `<div class="flag-pills">${c.flags.map(f => `<span class="flag-pill">${FLAG_LABELS[f] || f}</span>`).join('')}</div>`
        : '';

      let counselHtml = '';
      const cc = c.counsel || {};
      if (cc.has_data && cc.entries && cc.entries.length) {
        const parts = cc.entries
          .filter(e => e.role !== 'child')
          .map(e => `${e.names.join(', ')} <em>(${e.role.replace(/_/g,' ')})</em>`)
          .join(' &nbsp;|&nbsp; ');
        if (parts) counselHtml = `<div class="penalty-line" style="margin-top:5px;">Counsel: ${parts}</div>`;
      }

      html += `
        <div class="notable-card ${borderCls}">
          <div class="notable-card-header">
            <div class="notable-card-title">
              <a href="${c.url || '#'}" target="_blank">${c.title || c.case_id}</a>
              <div class="notable-card-meta">${c.court_name} &bull; ${c.date || 'n/d'}</div>
            </div>
            ${badgeText ? `<span class="osi-badge ${badgeCls}">${badgeText}</span>` : ''}
          </div>
          ${gapHtml}
          ${counselHtml}
          ${pillsHtml}
        </div>`;
    }
    resultsEl.innerHTML = html;
  } catch (e) {
    resultsEl.innerHTML = '<div class="notable-empty">Could not reach the server. Is the API running?</div>';
  } finally {
    btn.disabled = false;
    btn.textContent = 'Search Notable Cases';
  }
}

function clearNotable() {
  document.querySelectorAll('#flag-checkboxes input').forEach(cb => cb.checked = false);
  ['min_osi','max_osi','min_awarded','max_awarded','min_recovery','max_recovery',
   'notable_year_from','notable_year_to','counsel_surname','crown_counsel'].forEach(id => {
    document.getElementById(id).value = '';
  });
  document.getElementById('notable_court').value = '';
  document.getElementById('notable-results').innerHTML =
    '<div class="notable-empty">Select one or more filters above then click Search.</div>';
}

// ---- ASK TAB ----

const chat    = document.getElementById('chat');
const input   = document.getElementById('input');
const sendBtn = document.getElementById('send');

input.addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendMessage(); }
});

input.addEventListener('input', () => autoResize(input));

function autoResize(el) {
  el.style.height = 'auto';
  el.style.height = Math.min(el.scrollHeight, 140) + 'px';
}

function suggest(el) {
  input.value = el.textContent;
  autoResize(input);
  sendMessage();
}

function formatSourceLabel(s, i) {
  const title = s.title || s.case_id || 'Source';
  const court = s.court_name || s.court || 'Court unknown';
  const date = s.date || 'date unknown';
  return `[${i+1}] ${title} (${court}, ${date})`;
}

function appendMessage(role, text, sources, trace, cv) {
  const empty = document.getElementById('empty');
  if (empty) empty.remove();

  const msg = document.createElement('div');
  msg.className = `msg ${role}`;

  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  bubble.textContent = text;

  if (role === 'assistant') {
    const copyBtn = document.createElement('button');
    copyBtn.className = 'copy-btn';
    copyBtn.title = 'Copy response';
    copyBtn.innerHTML = '&#128203;';
    copyBtn.addEventListener('click', () => {
      navigator.clipboard.writeText(text).then(() => {
        copyBtn.classList.add('copied');
        copyBtn.innerHTML = '&#10003;';
        setTimeout(() => { copyBtn.classList.remove('copied'); copyBtn.innerHTML = '&#128203;'; }, 1500);
      });
    });
    bubble.appendChild(copyBtn);
  }

  msg.appendChild(bubble);

  if (role === 'assistant') {
    const disc = document.createElement('div');
    disc.className = 'disclaimer';
    disc.textContent = 'This answer is generated from retrieved public sources and may be incomplete or incorrect. Verify all citations against the original source. Not legal advice - consult a qualified NZ lawyer for your specific situation.';
    msg.appendChild(disc);
  }

  if (sources && sources.length > 0) {
    const srcDiv = document.createElement('div');
    srcDiv.className = 'sources';
    const label = document.createElement('strong');
    label.textContent = 'Sources';
    srcDiv.appendChild(label);
    sources.forEach((s, i) => {
      const item = document.createElement('div');
      item.className = 'source-item';
      const a = document.createElement('a');
      a.href = s.url;
      a.target = '_blank';
      a.textContent = formatSourceLabel(s, i);
      item.appendChild(a);
      srcDiv.appendChild(item);
    });
    msg.appendChild(srcDiv);
  }

  if (role === 'assistant') renderTrace(msg, trace, cv);

  chat.appendChild(msg);
  chat.scrollTop = chat.scrollHeight;
  return msg;
}

function showTyping() {
  const wrapper = document.createElement('div');
  wrapper.className = 'msg assistant';
  wrapper.id = 'typing';
  const dots = document.createElement('div');
  dots.className = 'typing';
  dots.innerHTML = '<span></span><span></span><span></span>';
  wrapper.appendChild(dots);
  chat.appendChild(wrapper);
  chat.scrollTop = chat.scrollHeight;
}

function removeTyping() {
  const t = document.getElementById('typing');
  if (t) t.remove();
}

async function sendMessage() {
  const q = input.value.trim();
  if (!q || sendBtn.disabled) return;
  const ack = document.getElementById('disclaimer-ack');
  if (ack && !ack.checked) {
    ack.closest('label').style.color = '#e53e3e';
    ack.focus();
    return;
  }

  appendMessage('user', q);
  input.value = '';
  input.style.height = 'auto';
  sendBtn.disabled = true;
  showTyping();

  const body = { question: q, trace: devMode };
  const court    = document.getElementById('court').value;
  const yearFrom = document.getElementById('year_from').value;
  const yearTo   = document.getElementById('year_to').value;
  if (court)    body.courts    = [court];
  if (yearFrom) body.year_from = parseInt(yearFrom);
  if (yearTo)   body.year_to   = parseInt(yearTo);

  let res;
  try {
    res = await fetch('/ask/stream', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
  } catch (e) {
    removeTyping();
    appendMessage('assistant', 'Could not reach the server. Is the API running?', null);
    sendBtn.disabled = false;
    input.focus();
    return;
  }

  if (!res.ok) {
    removeTyping();
    let msg = 'Something went wrong.';
    try { const d = await res.json(); msg = (d.detail && d.detail.error) || d.detail || msg; } catch (_) {}
    appendMessage('assistant', 'Error: ' + msg, null);
    sendBtn.disabled = false;
    input.focus();
    return;
  }

  removeTyping();
  const msg = appendMessage('assistant', '', null, null, null);
  const bubble = msg.querySelector('.bubble');
  let fullText = '';
  let streamedSources = null;
  let streamedTrace = null;
  let streamedCv = null;

  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buf = '';

  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += decoder.decode(value, { stream: true });
      const lines = buf.split('\n');
      buf = lines.pop();
      for (const line of lines) {
        if (!line.startsWith('data: ')) continue;
        const raw = line.slice(6);
        if (raw === '[DONE]') break;
        let ev;
        try { ev = JSON.parse(raw); } catch (_) { continue; }
        if (ev.type === 'token') {
          fullText += ev.text || '';
          bubble.textContent = fullText;
          chat.scrollTop = chat.scrollHeight;
        } else if (ev.type === 'sources') {
          streamedSources = ev.sources || [];
        } else if (ev.type === 'confidence') {
          streamedCv = ev;
        } else if (ev.type === 'debug' || ev.type === 'trace') {
          streamedTrace = ev;
        }
      }
    }
  } catch (e) {
    if (!fullText) bubble.textContent = 'Stream error. Please try again.';
  }

  // Attach sources and trace now that stream is complete
  if (streamedSources && streamedSources.length > 0) {
    const srcDiv = document.createElement('div');
    srcDiv.className = 'sources';
    const label = document.createElement('strong');
    label.textContent = 'Sources';
    srcDiv.appendChild(label);
    streamedSources.forEach((s, i) => {
      const item = document.createElement('div');
      item.className = 'source-item';
      const a = document.createElement('a');
      a.href = s.url;
      a.target = '_blank';
      a.textContent = formatSourceLabel(s, i);
      item.appendChild(a);
      srcDiv.appendChild(item);
    });
    msg.appendChild(srcDiv);
  }
  if (streamedTrace) renderTrace(msg, streamedTrace, streamedCv);

  chat.scrollTop = chat.scrollHeight;
  sendBtn.disabled = false;
  input.focus();
}

// ---- Dev mode ----

let devMode = false;

function toggleDev() {
  devMode = !devMode;
  const btn = document.getElementById('dev-toggle');
  btn.classList.toggle('active', devMode);
  btn.textContent = devMode ? 'DEV ON' : 'DEV';
}

function confidenceClass(level) {
  if (level === 'high')   return 'conf-high';
  if (level === 'medium') return 'conf-medium';
  return 'conf-low';
}

function renderTrace(msg, trace, cv) {
  if (!trace) return;

  const btn = document.createElement('button');
  btn.className = 'trace-toggle';
  btn.textContent = 'Show trace';

  const panel = document.createElement('div');
  panel.className = 'trace-panel';
  panel.style.display = 'none';

  const lat = trace.latency_ms || {};
  const counts = trace.counts || {};
  const models = trace.models || {};

  let sqlLine = '';
  if (trace.strategy === 'sql_first_hybrid') {
    const filters = trace.sql_filters ? Object.entries(trace.sql_filters)
      .map(([k, v]) => `${k}=${JSON.stringify(v)}`).join(', ') : '';
    sqlLine = `<div><span class="t-label">SQL</span>  ${filters || 'no filters'} &rarr; ${trace.sql_point_ids_count} IDs</div>`;
  }

  let cvLine = '';
  if (cv) {
    const confCls = confidenceClass(cv.evidence_confidence);
    const warn = cv.has_warning ? '<span class="t-warn"> [!]</span>' : '';
    const orphans = cv.orphan_citations.length ? ` | orphan: [${cv.orphan_citations.join(',')}]` : '';
    cvLine = `<div><span class="t-label">Citations</span>  <span class="${confCls}">${cv.evidence_confidence.toUpperCase()}</span>${warn} &mdash; ${cv.cited_count} cited${orphans}</div>`;
  }

  panel.innerHTML = `
    <div><span class="t-label">Strategy</span> ${trace.strategy}</div>
    ${sqlLine}
    <div><span class="t-label">Qdrant</span>   ${counts.qdrant_candidates} &rarr; ${counts.after_dedup} (dedup) &rarr; ${counts.after_rerank} (final)</div>
    <div><span class="t-label">Latency</span>  embed ${lat.embed}ms | sql ${lat.sql}ms | qdrant ${lat.qdrant}ms | rerank ${lat.rerank}ms | gen ${lat.generate}ms | <strong>total ${lat.total}ms</strong></div>
    <div><span class="t-label">Scores</span>   ${(trace.top_scores || []).map(s => s.toFixed(3)).join('  ')}</div>
    <div><span class="t-label">Models</span>   ${models.llm || '-'} | embed: ${models.embedding || '-'} | reranker: ${models.reranker_enabled ? 'on' : 'off'}</div>
    ${cvLine}
  `;

  btn.addEventListener('click', () => {
    const hidden = panel.style.display === 'none';
    panel.style.display = hidden ? 'block' : 'none';
    btn.textContent = hidden ? 'Hide trace' : 'Show trace';
  });

  msg.appendChild(btn);
  msg.appendChild(panel);
}

// ---- Health polling ----

let _llmOnline = true;

async function checkHealth() {
  try {
    const res  = await fetch('/health');
    const data = await res.json();
    const llmOk = data.status === 'ok';

    if (llmOk !== _llmOnline) {
      _llmOnline = llmOk;
      updateLLMStatus(llmOk);
    }
  } catch (_) {}
}

function updateLLMStatus(online) {
  const banner  = document.getElementById('llm-banner');
  const sendBtn = document.getElementById('send');
  const input   = document.getElementById('input');

  banner.style.display = online ? 'none' : 'flex';

  if (sendBtn) {
    sendBtn.disabled = !online;
    sendBtn.title    = online ? '' : 'LLM server is offline';
  }
  if (input) {
    input.placeholder = online
      ? 'Ask a question about NZ law...'
      : 'LLM offline - answers unavailable. Search and trackers still work.';
  }
}

// ---- Wire all event listeners (replaces inline onclick/oninput handlers) ----

(function wireEvents() {
  document.getElementById('legal-banner-close').addEventListener('click', () => {
    document.getElementById('legal-banner').style.display = 'none';
  });

  document.getElementById('about-link').addEventListener('click', e => {
    e.preventDefault();
    showTab('about');
  });

  document.querySelectorAll('.tab-btn[data-tab]').forEach(btn => {
    btn.addEventListener('click', () => showTab(btn.dataset.tab));
  });

  document.querySelectorAll('.suggestion').forEach(el => {
    el.addEventListener('click', () => suggest(el));
  });

  document.getElementById('send').addEventListener('click', sendMessage);

  document.querySelectorAll('#tab-notable .btn-search').forEach(btn => {
    btn.addEventListener('click', searchNotable);
  });
  document.querySelectorAll('#tab-notable .btn-clear').forEach(btn => {
    btn.addEventListener('click', clearNotable);
  });

  document.getElementById('sent-search-btn').addEventListener('click', searchSentencing);
  document.querySelector('#tab-sentencing .btn-clear').addEventListener('click', clearSentencing);

  document.getElementById('pg-search-btn').addEventListener('click', searchPG);
  document.querySelector('#tab-pg .btn-clear').addEventListener('click', clearPG);

  document.getElementById('dev-toggle').addEventListener('click', toggleDev);
})();

// ---- Start health check ----
checkHealth();
setInterval(checkHealth, 30000);

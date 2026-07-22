# Boundary-Aware Route Matching — Final Revised Plan (v3)

## Background

Both the C++ Aho-Corasick engine and the Python `routing.py` matcher perform raw
substring matching with no word boundaries. Route terms like `"ant"`, `"meth"`,
`"appeal"`, `"bond"` silently match inside unrelated words (`tenant`, `method`,
`unappealing`, `bonded`), causing routes to fire, suppress, or pass context gates
incorrectly. The worst case is `exclude_any`, where a false match silently
suppresses the correct route with no visible error signal.

The fix must live in the matcher, not be patched term-by-term in the route table.
Sequenced into four PRs, each independently mergeable, engine first, data second,
enforcement third, validation last.

---

## PR 1 — Boundary-aware matcher only

Deliberately narrow: boundary matching and suffix/inflection handling are
different semantic layers. Mixing them makes any post-merge regression
ambiguous to diagnose.

### Boundary predicate — explicit, not `isalnum`

```cpp
// normalize_query lowercases all ASCII letters unconditionally (see
// normalize_query, [line ref]); A-Z cannot reach this function and is
// deliberately omitted, not assumed safe.
static constexpr bool is_route_word_char(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static constexpr bool has_route_boundaries(
    std::string_view text, std::size_t start, std::size_t end) noexcept {
    const bool left_ok =
        start == 0 || !is_route_word_char(static_cast<unsigned char>(text[start - 1]));
    const bool right_ok =
        end == text.size() || !is_route_word_char(static_cast<unsigned char>(text[end]));
    return left_ok && right_ok;
}
```

No `isalnum` — locale-sensitive, with unsigned-char UB traps if called carelessly.
Half-open `[start, end)` offsets throughout, not inclusive end positions.

**Known, documented gap:** bytes `>= 0x80` (UTF-8 continuation bytes) are treated
as non-word, i.e. as boundaries, by `is_route_word_char`. This is an accepted
limitation, not an oversight — flag explicitly in the code comment, since the
route table already contains Māori-language terms (`"kainga ora"`) that could be
affected if diacritics survive normalization as raw UTF-8 in future terms.

### Scope check — required, not conditional

- Grep every call site of `AhoCorasick::search()` / `AhoCorasick` construction
  across the codebase. Paste the actual `grep -rn` output in the PR description.
- If routing is the sole consumer: `search()` is boundary-aware by default, full
  stop, with a comment stating this was verified single-consumer at time of
  change, and a note for any future second consumer to revisit.
- If a second consumer exists today: add an explicit `AcSearchMode` enum
  (`Substring`, `WordBoundary`), routing calls `WordBoundary` only, and the
  existing consumer's call sites are updated to `Substring` explicitly so their
  behavior provably does not change.
- Either way: leave a comment directly on the relevant declaration warning that
  `Substring`/raw-substring behavior is a deliberate, separate choice for any
  future consumer — not a safe default to assume.

### Engine and parity

- **C++**: index-based loop in `search()`, boundary check via the predicate
  above before `hits.push_back(h)`.
- **Python**: `_tok(term, query)` helper, same semantics, replacing all 16
  `t in q` / `term in q` call sites in `core/routing.py`.
- **Cross-language parity fixture set**: shared `(term, query)` pairs asserting
  identical accept/reject between C++ and Python, including normalization edge
  cases (smart quotes, em-dashes). Must pass before merge.
- No suffix allowlist, no substring fallback, no `TermMatchMode` enum yet.

---

## PR 2 — Full-coverage audit, explicit terms, route-interaction fixtures

### Audit — shown, not asserted

- Re-run the length/risk audit against all **six** matchable fields
  (`include_any`, `include_any_precise`, `include_any_broad`, `exclude_any`,
  `require_context_any`, `include_all`), enumerated programmatically from the
  `StatuteRoute` struct definition.
- Paste actual output in the PR description.
- For every flagged term, check both substring risk (fixed by PR 1) and
  polysemy risk (not fixed by PR 1 — `"appealing"` is boundary-correct and
  still wrong).
- Cross-reference every proposed addition against the table's current
  contents to avoid adding terms that are already present, or redundant with
  an existing literal entry.

### Explicit term additions — split by justification

**(a) Morphological inflections**, justified purely by grammatical relation to
an existing base term:

- `repair` → `repaired`, `repairing`, `repairs`
- `fix` → `fixed`, `fixing`, `fixes`
- `evict` → `evicting`, `evicted`, `eviction`
- `harass` → `harassing`, `harassed`
- `sublet` → `sublets`, `subletting`
- `install` → `installs`, `installing`, `installed`
- `consent` → `consents`, `consented`, `consenting`
- `lock` → `locks`, `locked`, `locking`
- `leak` → `leaks`, `leaked`, `leaking`
- (verify each against current table contents before adding — some may
  already be present, e.g. `crack`/`cracked` already coexist in
  `wear_and_tear`)

**(b) New phrases**, not pure inflections — each cites the real/representative
query that motivated it, same evidentiary bar as the original false-positive
fixes. Do not expand this bucket on "seems plausible" grounds alone.

**Polysemy exception** — terms with a common non-legal sense get phrase-level
treatment, not bare inflections. This applies to the base verb form itself,
not just its inflections: "this option does not appeal to me" is a standalone,
boundary-correct match on bare `"appeal"` with no legal meaning at all, so the
base form is removed too, not just `"appealing"`/`"appealed"`/`"appeals"`:

```
exclude_any for repair_notice_s56, repairs_landlord_not_fixing,
tribunal_mediation_enforcement:

  appeal the decision
  appeal the order
  appeal to the district court
  file an appeal
  filed an appeal
  filing an appeal
  notice of appeal
  appealing the decision
  appealed the decision
```

No bare `"appeal"`, `"appealing"`, `"appealed"`, or `"appeals"` anywhere in
this list.

### Fixtures — four kinds

- `must_fire` / `must_not_fire` — minimum one `must_fire` fixture per route
  across all 41+ routes, plus coverage for every term flagged in the
  six-field audit.
- `must_dominate` / `must_not_dominate` — which route wins the
  priority/allow-list contest when several fire on the same query.
- `must_force_sections` / `must_not_force_sections` — asserts the resolved
  `forced_sections` output, not just which route nominally matched.

```json
{
  "query": "my guest damaged the property",
  "must_fire": ["guest_damage_liability"],
  "must_dominate": "guest_damage_liability",
  "must_not_dominate": ["repairs_tenant_not_at_fault"],
  "must_force_sections": ["NZLEG/RTA/s40"]
}
```

### Smoke check (non-gating, after PR 1 and again after PR 2)

- 10 route-ownership tests, the known false positives from Part A, and 5 known
  historical eval questions. Runs in seconds, reported inline in the PR
  description. The smoke check is diagnostic rather than a score gate — it is
  not a second full eval and not a list that grows over time. But a failure
  must be inspected before merge; if it reveals an actual route correctness
  issue, fix it before merging, the same as any other discovered bug.

---

## PR 3 — CI linter, enforced

- Scans all six matchable fields, enumerated the same programmatic way as
  PR 2's audit, on every route-table PR.
- Fails the build on:
  - Suspicious short alphabetic terms, unless explicitly whitelisted by the
    linter config with a stated reason. There is no `TermMatchMode` to
    annotate with after PR 1/2 — the mechanism is a CI-config whitelist, not
    an in-struct annotation:
    ```json
    {
      "term": "ko",
      "field": "include_any",
      "route": "kainga_ora",
      "reason": "Accepted acronym for Kainga Ora in tenancy queries"
    }
    ```
  - Terms matching inside a seeded list of common tenancy-adjacent words,
    seeded from every false positive found across all prior audits.
  - Terms with known polysemy risk lacking a phrase-level alternative.
  - **Terms that appear to rely on partial/non-boundary matching rather than
    explicit word/phrase entries** — since a route author cannot literally
    declare "substring mode" after PR 1/2, this is checked practically: flag
    base stems lacking their explicit inflected forms (e.g. `"repair"` present
    without `"repaired"`/`"repairing"` alongside it, where the route's own
    phrasing implies the inflected form is needed), and any other pattern
    where a short base term is the only entry covering a concept that
    naturally has common variants. (Revisit only if PR 2's explicit-term
    approach is later found insufficient at scale — that would be new
    evidence justifying a future `TermMatchMode` PR, not something committed
    to now.)
- Runs in CI, blocking — not a script someone remembers to invoke by hand.

---

## PR 4 — Cleanup, full-trace diff-and-triage, rebaseline

### Cleanup

- Remove workaround phrases that existed purely to dodge the substring bug.
- Keep phrases that are good practice independently. Document the distinction
  per removal, not blanket.

### Full-trace diff

For every eval question, diff against the pre-PR-1 baseline:

```
matched_intents
dominant_route
forced_sections
suppressed_routes
rule_cards
synthetic_query
context_debug / statute_routing block
```

Manually triage each diff into:

- **(a)** Bug fixed correctly.
- **(b)** Regression — needs a term/inflection/allow-list fix.
- **(c)** Eval case was passing for the wrong reason — relabel, don't revert.

### Acceptance gates — mechanism-level, not a single score threshold

- [ ] All PR 2 semantic / dominance / forced-section fixtures pass.
- [ ] All known substring false positives eliminated.
- [ ] No unresolved class-(b) regression remaining after triage.
- [ ] No severe `no_violation` collapse.

`OVERALL` score delta is recorded as a **rebaseline signal**, annotated with
how much is attributable to (a), (b), (c) — not a pass/fail criterion.

### Rebaseline

- Treat resulting eval scores as the new baseline, documented with what
  changed and why, never compared to the pre-fix baseline without that context.

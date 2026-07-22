# Boundary-Aware Route Matching - Problem Analysis and Fix Plan

## Problem Statement

The AhoCorasick engine (`src/aho_corasick.cpp`) and the Python `t in q` patterns
(`core/routing.py`) both perform raw substring matching with no word boundaries.
A route term fires whenever its byte sequence appears anywhere inside the normalized
query string, regardless of what surrounds it.

This is wrong for legal routing. Route terms are linguistic and legal tokens -
"appeal", "bond", "viewing", "ant", "meth" - each carrying a specific legal
meaning. A match should only fire when the term appears as a standalone token,
not embedded inside an unrelated word.

The consequences are not minor. A false match in any routing field can:

1. **exclude_any** - silently suppress the correct route entirely
2. **include_any** - fire the wrong route, inject wrong sections and rule card
3. **require_context_any** - pass or fail the context gate incorrectly
4. **include_all** - satisfy or block an all-must-match requirement incorrectly
5. **forced_sections** - (indirect) wrong sections reach the LLM
6. **rule_card / synthetic_query** - wrong legal framing sent to the model

The worst field is exclude_any because the failure is silent: the correct route
disappears, the model receives generic context, and the answer is wrong with no
visible signal that routing failed.

Length alone is not a reliable safety rule. All of the following are unsafe:
- "ant" (3) inside "tenant", "want", "grant"
- "meth" (4) inside "method"
- "bond" (4) inside "bonded", "bonding"
- "appeal" (6) inside "unappealing", "appealing", "appealed"
- "viewing" (7) inside "reviewing"
- "notice" (6) inside "unnoticed", "noticeable"
- "alter" (5) inside "alternative" (already fixed by hand in a prior commit)
- "paint" (5) inside "painting", "painted", "repainting" (fixed by hand this session)

The systematic fix must be in the matcher, not in individual terms.

---

## Root Cause

Both implementations match terms as raw byte substrings:

**C++ (aho_corasick.cpp, lines 82-86):**
```cpp
for (unsigned char c : text) {
    ...
    cur = _nodes[cur].next[c];
    for (const auto& h : _nodes[cur].out)
        hits.push_back(h);   // no boundary check
}
```

**Python (routing.py, 16 call sites):**
```python
any(t in q for t in route.include_any)        # line 113
any(term in q for term in route.exclude_any)   # line 120
all(t in q for t in route.include_all)         # line 124
# ... and 13 more identical patterns
```

The fix in both cases is identical in semantics: require that the character
immediately before the match start and the character immediately after the match
end are both non-alphanumeric (or absent). This is standard word-boundary
(`\b`) behavior over the alphabet `[a-z0-9]` (the normalized query character
set after `normalize_query()` runs).

---

## Part A: Current False-Match Risks (Remaining After Prior Hotfixes)

All 7 remaining cases are in `exclude_any` (highest severity - route suppressed):

| Route | Term | False-matches in | Attack scenario |
|---|---|---|---|
| `fixed_term_sell` | `"viewing"` | "reviewing" | "I am reviewing my options" suppresses the sell-notice route |
| `repair_notice_s56` | `"bond"` | "bonded", "bonding" | "bonded contractor" suppresses the s56 notice route |
| `repair_notice_s56` | `"appeal"` | "unappealing", "appealing", "appealed" | "I find this unappealing" suppresses the route |
| `repairs_landlord_not_fixing` | `"appeal"` | same | same |
| `painter_landlord_access` | `"bond"` | "bonded", "bonding" | "bonded tradesperson" suppresses painter access route |
| `property_uninhabitable_rent_abatement` | `"bond"` | "bonded", "bonding" | same |
| `tribunal_mediation_enforcement` | `"appeal"` | "unappealing", "appealing", "appealed" | "unappealing situation" suppresses enforcement route |

0 remaining cases in include_any or other fields - the include_any terms are
predominantly multi-word phrases which are inherently boundary-safe.

---

## Part B: Inflected-Form Dependencies

After adding word boundaries, `"repair"` will no longer match inside `"repairs"`,
`"fix"` inside `"fixing"`, etc. These are the genuine gaps where the route lists
only the base form without its common inflected variants:

| Route | Field | Base form | Missing real inflections |
|---|---|---|---|
| `repairs_tenant_not_at_fault` | include_any | `"repair"` | "repaired", "repairing" |
| `repairs_tenant_not_at_fault` | include_any | `"fix"` | "fixing", "fixes" |
| `repairs_tenant_not_at_fault` | require_context_any | `"lock"` | "locks", "locked", "locking" |
| `repairs_maintenance` | include_any | `"leak"` | "leaks", "leaked" |
| `wear_and_tear` | include_any | `"crack"` | "cracking", "cracks" |
| `termination_notice` | include_any | `"evict"` | "evicting", "evicted" |
| `quiet_enjoyment` | include_any | `"harass"` | "harassing", "harassed" |
| `sham_flatmate_agreement` | include_any | `"sublet"` | "sublets" |
| `subletting_without_consent` | include_any | `"sublet"` | "sublets" |
| `property_change` | include_any_broad | `"install"` | "installs", "installing" |
| `property_change` | require_context_any | `"consent"` | "consents", "consented" |

**Special case - `"appeal"` in three exclude_any blocks:**
This term is simultaneously a false-match risk (matches "unappealing") AND has a
legitimate exclusion intent that extends to its inflected forms. After the boundary
fix, standalone `"appeal"` still matches correctly. But "appealing", "appealed",
"appeals" - which represent REAL cases to exclude (e.g. "I have appealed the
decision") - become unreachable. These inflections must be added explicitly.

Routes affected: `repair_notice_s56`, `repairs_landlord_not_fixing`,
`tribunal_mediation_enforcement`.

---

## Part C: Call Sites to Change

### C++ - 1 location

**File:** `src/aho_corasick.cpp`
**Function:** `AhoCorasick::search()`
**Lines:** 82-86 (the inner loop)

Change from range-for over bytes to index-based loop. At each hit, compute
`match_start = i + 1 - term.size()`. Accept the hit only if:
- `match_start == 0` OR `!isalnum(text[match_start - 1])` (left boundary)
- `i + 1 == text.size()` OR `!isalnum(text[i + 1])` (right boundary)

No header changes. No API changes. No caller changes. The fix is fully contained
inside `search()`.

### Python - 16 call sites

**File:** `core/routing.py`
**Lines:** 108, 110, 111, 113, 120, 124, 186, 188, 191, 192, 195, 205, 207, 208, 209, 270

All are `t in q` or `term in combined_query` patterns. Add one helper function
`_tok(term, query)` (pure Python, no regex, uses `str.find()` loop) and replace
every occurrence. No public API changes.

---

## Implementation Plan

Steps are ordered to minimize risk: fix the engine first, then fix the data.

### Step 1 - Fix `src/aho_corasick.cpp`

Change `search()` to index-based loop. Add left+right boundary check before
`hits.push_back(h)`. ~12 line change.

### Step 2 - Fix `core/routing.py`

Add `_tok(term: str, query: str) -> bool` helper using `str.find()` loop.
Replace all 16 `t in q` / `term in q` call sites with `_tok(t, q)`.

### Step 3 - Add missing inflected forms to `routes.cpp`

For the 11 routes in Part B, add the missing inflections alongside the existing
base form. This makes the required behavior explicit rather than relying on
substring accidents.

Additions required:
- `repairs_tenant_not_at_fault` include_any: add "repaired", "repairing", "fixing", "fixes"
- `repairs_tenant_not_at_fault` require_context_any: add "locks", "locked", "locking"
- `repairs_maintenance` include_any: add "leaks", "leaked"
- `wear_and_tear` include_any: add "cracking", "cracks"
- `termination_notice` include_any: add "evicting", "evicted"
- `quiet_enjoyment` include_any: add "harassing", "harassed"
- `sham_flatmate_agreement` include_any: add "sublets"
- `subletting_without_consent` include_any: add "sublets"
- `property_change` include_any_broad: add "installs", "installing"
- `property_change` require_context_any: add "consented"

### Step 4 - Fix `"appeal"` in three exclude_any blocks

Add `"appeals"`, `"appealing"`, `"appealed"` to the exclude_any of:
- `repair_notice_s56`
- `repairs_landlord_not_fixing`
- `tribunal_mediation_enforcement`

The false-match on "unappealing" is eliminated by Step 1. The legitimate
inflected-form exclusions are restored by Step 4.

### Step 5 - Build and smoke-test

```bash
cmake --build build-prod --target nz_tenancy -j$(nproc)
systemctl --user restart astraea-nz-tenancy
# spot check: pet question, repair question, appeal question
curl -s -N -H "X-No-Log: 1" -H "X-API-Key: ..." \
  -d '{"question":"..."}' http://localhost:8001/ask/stream | grep route_debug
```

### Step 6 - Single eval run

Run `eval_judge.py` once with the new binary and routes hash. Accept if
OVERALL >= 65.6 (baseline) and no_violation does not regress. A meaningful
improvement is expected from the combined routing fixes.

---

## Acceptance Criteria

- [ ] `"ant"` does NOT match inside "tenant" or "want" in any route field
- [ ] `"meth"` does NOT match inside "method"
- [ ] `"appeal"` does NOT match inside "unappealing"
- [ ] `"viewing"` does NOT match inside "reviewing"
- [ ] `"bond"` does NOT match inside "bonded" or "bonding"
- [ ] `"repair"` still matches standalone "repair" and "repairs" (explicitly listed)
- [ ] `"fix"` still matches standalone "fix", "fixing", "fixed" (explicitly listed)
- [ ] `"evict"` still matches "evicting", "evicted" (explicitly listed after Step 3)
- [ ] Eval OVERALL does not regress below 65.6
- [ ] `no_violation` score does not regress

---

## Files Changed

| File | Change |
|---|---|
| `src/aho_corasick.cpp` | Step 1: boundary check in `search()` |
| `core/routing.py` | Step 2: `_tok()` helper + 16 call sites |
| `jurisdictions/nz_tenancy/routes.cpp` | Steps 3+4: inflected forms + appeal exclude fix |

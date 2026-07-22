# PROBLEM.md — Analysis & Feedback

Reviewed against `src/anchor.cpp`, `jurisdictions/nz_tenancy/routes.cpp`, and
`include/astraea/jurisdiction.hpp` on `main` @ `78876f2`.

The document is well-structured and the corruption-pattern history (§6) is
accurate against current code. Two factual drifts, one bug worth fixing
before any of the proposed changes, and concrete answers to the open
questions follow.

---

## A. Factual drifts in PROBLEM.md (fix these before reasoning further)

### A.1 Line number wrong — easy, but indicates the doc was edited locally

> "In anchor.cpp **line 391** (retrieve_anchor), retrieved section text is
> capped at 600 characters"

Actual cap is at `src/anchor.cpp:378` on `main`. Update before the next pass
or readers will lose 30 seconds confirming.

### A.2 The "fetch all chunks (limit=64)" claim is wrong — and this matters

§6b says the fix for random-chunk selection was *"fetch all chunks
(**limit=64**), sort by chunk_index, select lowest non-amendment chunk"*.

The actual code (`src/anchor.cpp:192`) uses **`limit=8`**:

```cpp
miss_reqs.push_back({query_vector, 8, 0.0f, std::move(case_filt)});
```

This is the **first concrete latent bug** the analysis surfaced:

- §3 says "for long sections split across multiple chunks, only the opening
  chunk is used" — but with `limit=8`, if a section has > 8 chunks AND
  Qdrant ranks them by vector similarity (it does), the **lowest-chunk_index
  candidate may not even be in the result set we're sorting**. The "select
  lowest chunk_index" guarantee silently degrades to "select lowest among
  the 8 most similar chunks", which is a probabilistic guarantee, not the
  deterministic one the comments claim.
- The corpus integrity check presumably uses a true exhaustive fetch (the
  Python ingest/check side that the doc references), so the production
  path is weaker than the validated path. This is exactly the kind of
  drift that makes integrity-check pass rates look better than the live
  user experience.
- For s40, s51, s66 (the sections §3 lists as long), check `chunks_per_case_id`
  in the live collection. If any exceed 8, this bug is silently active right
  now and is a partial root cause of the must_include / section_recall gaps
  in §7.

**Recommendation:** raise the limit to a value strictly greater than the
maximum `chunks_per_case_id` in the v2 corpus. The PR description for the
nz_legal_v2 build presumably has that number; if not, a one-time Qdrant
query gets it. Use that number + a 4× safety factor (e.g. 32 if max is 6).
Cost is one extra Qdrant batch result of size ≤ 32 per forced section per
request — negligible vs the 30-50 ms anchor budget.

---

## B. One real bug I found while validating the doc

### `chunk_index_of()` returns 0 on missing payload — collides with real chunk 0

`src/anchor.cpp:180-184`:

```cpp
auto chunk_index_of = [](const QdrantPoint& pt) -> int {
    auto it = pt.payload.find("chunk_index");
    if (it == pt.payload.end()) return 0;      // ← silent default
    try { return std::stoi(it->second); } catch (...) { return 0; }
};
```

The function is used in `std::min_element` to pick the operative opening
chunk. If a chunk's `chunk_index` payload is missing OR contains garbage
that doesn't parse as int, it gets treated as chunk 0 and **wins the
selection** over a legitimate chunk_index 1, 2, 3…

For the v2 collection this may be a non-issue (ingest script always
sets it). But:

- It silently hid the 56.3% bug rate in the v1 collection per §6: a
  chunk missing `chunk_index` would still claim to be the opening.
- Future schema changes that rename or omit the field will silently
  regress the selection without any log.
- The "stoi catch-all returning 0" hides actual bad data.

**Recommendation:**

```cpp
auto chunk_index_of = [](const QdrantPoint& pt) -> int {
    auto it = pt.payload.find("chunk_index");
    if (it == pt.payload.end()) return INT_MAX;  // never preferred
    try { return std::stoi(it->second); } catch (...) {
        SPDLOG_WARN("chunk_index_of: non-int chunk_index='{}' for {}",
                    it->second, pt.id);
        return INT_MAX;
    }
};
```

INT_MAX makes missing/bad data sort to the back, where it cannot win
against any well-formed chunk. The log surfaces v3-era data quality
regressions early. Five-line change.

---

## C. Direct answers to the open questions

### Q1: Raise the 600-char cap? To what?

**Yes — to 1500.** Reasoning:

- Average RTA section text is ~800-1200 chars. 600 cuts mid-list for s40,
  s51, s66 (the sections §3 calls out).
- The LLM has shown it doesn't suffer attention dilution at the current
  prompt sizes we ship (~3-8 KB of context per request — see context_chars
  in route_debug.jsonl). Adding 5 anchor sections × 900 extra chars =
  4.5 KB → roughly doubles current anchor budget. Still well inside the
  8192-token ctx_limit reported in `ContextBudget`.
- 1500 vs 1200: the cost difference is one extra paragraph per section.
  When a long section has a critical late sub-clause (e.g. s40(g)-(k)),
  the difference between "answer correct" and "model invented a section
  number" is whether we shipped that clause. 1500 is the right anchor at
  current context budgets; revisit if context_chars approaches 6 KB on
  the p95.

**Do NOT** raise to "unlimited" — some sections (s127, s136) run 4000+ chars
and would dominate the anchor block.

### Q2: Add routes for s14, s38A, s42, s55, s78, s100-s105?

**Yes for some, no for others. Triage:**

| Section | Add route? | Why |
|---|---|---|
| s14 (Access to premises) | **Yes** | Lock-out / quiet enjoyment is a high-traffic question with sharp legal stakes; missing forced injection here means model guesses. |
| s38A (Landlord's right to require bond) | **No** | Procedural; rarely the operative section. Vector search is fine. |
| s42 (Subtenancy without consent) | **Yes** | Sharp consent rule; user phrasings ("can my flatmate sublet to X?") don't naturally embed near s42's text — exactly the case routes exist for. |
| s55 (Failure to repair - Tribunal order) | **Yes** | Already mentioned in route at line 1109 (`s55, s56` triggers) — verify the route exists and fires. If not, add. |
| s78 (Orders Tribunal can make) | **Maybe** | §4 says "model sometimes cites this wrong" — that's a faithfulness symptom not a recall symptom. A rule_card may be better than a forced section here (e.g. *"Do not invent specific orders; if the question asks what the Tribunal can do, reference s78's enumerated list"*). |
| s100-s105 (Tribunal jurisdiction/procedure) | **Yes for s104** | Jurisdiction is operative for "can I take this to the Tribunal?" type questions. s100-s103, s105 are procedural — let vector search handle them. |

**Suggested new routes** (concrete code patterns following the existing style):

```cpp
// Around line 956 (where s38 lives)
{
    .intent = "landlord_entry_or_access",
    .include_any = {
        "landlord came in", "landlord entered", "landlord access",
        "right to enter", "without permission", "landlord let himself in",
        "without notice", "48 hours notice",
        "locked out", "lock out", "changed the locks",
    },
    .forced_sections = {"NZLEG/RTA/s14"},  // + s56 if exists
    .notes = "Access to premises (s14), lock-out (s56 if exists).",
},
{
    .intent = "subtenancy_without_consent",
    .include_any = {
        "flatmate sublet", "subletting without", "sublet without",
        "sub-let without permission", "another person moved in",
    },
    .forced_sections = {"NZLEG/RTA/s42"},
    .notes = "Subletting without landlord consent.",
},
{
    .intent = "tribunal_jurisdiction",
    .include_any = {
        "take this to tribunal", "can the tribunal", "tribunal jurisdiction",
        "is this a tribunal matter", "does the tribunal cover",
    },
    .forced_sections = {"NZLEG/RTA/s104"},
    .notes = "Tribunal jurisdiction (s104).",
},
```

Each addition: ~10 LoC, no behavioural risk to existing routes (AC matching
is additive).

### Q3: Subsection-level indexing?

**No — fix the retrieval, not the indexing.** The proposal would:

- Triple the chunk count (s40 has 13 paragraphs × current 1-3 chunks ≈ 30+
  chunks just for s40)
- Move the "what is the right granularity for citation" problem into ingest
  rather than retrieval (e.g. is "(3)(a)(ii)" a chunk?)
- Require the integrity check, retrieval, anchor display, and the
  `is_amendment_chunk` heuristic all to handle the new granularity

The right fix is **(1) the limit=8 bug from §A.2** plus **(2) the 1500-char
cap from §C-Q1**. Combined, those let the LLM see the full opening chunk of
a long section, and `chunk_index ≤ 1-2` covers the typical 1200-2000 char
operative body. The model is good at extracting the right subsection given
the full section text — that's a much smaller ask than is implied by §3.

The subsection-case_ids in the integrity check (§2b) should be updated to
parent IDs. That's an integrity-check-side change with zero corpus impact.

### Q4: Is 600-char truncation causing wrong-subsection citations?

**Probably partly, but not the dominant cause.** Evidence:

- `section_faithfulness = 67.6` (§7) means ~32% of section citations don't
  fully match the cited text. Truncation accounts for *some* of those (model
  cites s51 generally when s51(3) is the answer because (3) was truncated
  away). But the no_violation = 74.6 score suggests the model is also
  fabricating subsection numbers that aren't in any text the corpus has —
  which truncation alone doesn't cause.
- Recommend: instrument a one-off log line per request that emits
  `(cited_section, anchor_section_chars)` so the correlation between
  truncation depth and faithfulness can be measured. Two-line change in
  `assemble_request`.

### Q5: Are no_violation failures correlated with specific routes?

**The data to answer this exists in `route_debug.jsonl` but is unjoined.**
Each route_debug entry has `matched_intents` + `answer`. An external
evaluator score over the same answers gives the no_violation pass/fail.
Joining the two and grouping by `matched_intents[0]` would surface the
correlation in 20 lines of Python.

Worth doing — the answer informs whether the next round of work should be
more rule_cards (route-specific guards) or a more universal "don't
fabricate" system prompt addition.

---

## D. Higher-order observations not in PROBLEM.md

### D.1 Confidence config is at default but routes drive most retrievals

`include/astraea/jurisdiction.hpp:74-77` shows the default
`ConfidenceConfig{ high_score=0.82, medium_score=0.77 }`. The corpus is
small (934 chunks) and routes force specific sections — meaning many
correct answers ride on **forced injection**, not vector score. The
confidence labels reported to the user (§3 confidence event from #69)
therefore measure "did vector search find a high-score chunk" — which is
**uncorrelated** with "did the rule-card / forced section fire". A
forced-section answer can ship with "low confidence" because the vector
similarity to other chunks was bad, even though the answer is grounded in
authoritative legislation.

**Recommendation:** add an "anchor_quality" field to the confidence
event that reports whether any forced section fired. The frontend can then
distinguish "answer is shaky" from "vector search was shaky but the rule
fires regardless". Already have all the data in `assembled.route_dec`.

### D.2 §6c HHS prefix fix relies on `section_prefix=r`

The fix in §6c (case_id collision via section prefix) is a one-line ingest
fix. There's no runtime guard that detects the regression — if a future
jurisdiction or corpus refresh forgets to set `section_prefix`, the prefix
will silently default and we'll get the same bug. Worth adding a startup-time
assertion: for any jurisdiction with `forced_sections`, verify each one is
fetchable from the live Qdrant collection before serving traffic. Goes in
the existing `HealthProber::probe()` flow.

### D.3 The integrity check should be CI

The 87-anchor / 78-OK / 9-MISSING result was presumably run manually. If
this is the gating signal between "corpus is good" and "corpus is broken",
it should run on every commit to `jurisdictions/nz_tenancy/routes.cpp` and
on a nightly cron against the live Qdrant. Otherwise the rate will drift
silently — exactly the failure mode that produced the v1 56.3% problem.

---

## E. Priority order I'd attack

1. **Fix the limit=8 retrieval bug** (§A.2) — silently degrading the
   guarantee on every multi-chunk forced section. 1-line change.
2. **Fix `chunk_index_of` missing-payload returning 0** (§B) — defensive,
   5-line change.
3. **Raise 600 → 1500 char cap** (§C-Q1) — measurable section_recall
   improvement from one literal change.
4. **Add 3 new routes** (§C-Q2: s14, s42, s104) — closes the highest-impact
   uncovered sections from §4. ~40 LoC.
5. **Wire integrity check into CI** (§D.3) — prevents the next regression
   silently shipping.
6. **Add anchor_quality to confidence event** (§D.1) — restores correct
   user-facing signal under route-driven retrieval.

Items 1-3 land in one PR (~30 LoC across `src/anchor.cpp`) and should
move `section_recall` and `must_include` measurably with minimal risk.
Items 4-6 follow in separate PRs by area.

The subsection-indexing question (Q3) and the section-version-currency
work (§5) can wait — neither blocks the eval improvements, and both are
larger surface-area changes that benefit from running on whatever the
eval looks like after items 1-3 land.

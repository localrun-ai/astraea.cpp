This `PROBLEM.md` is very good. It is clear enough to hand to another reasoning model, and it captures the current state without mixing old fixed bugs with current open issues. The most important thing now is to avoid overreacting: `nz_legal_v2` is much healthier than the old corpus, but the **retrieval context assembly** is still too lossy for long sections.

My recommendations:

## 1. Fix the 600-char cap first

This is the most urgent open issue.

A 600-character cap is too small for legal sections. It almost guarantees that sections like `s40`, `s51`, `s66`, and `s100-s105` will be under-explained. Your own file notes that long sections only expose the opening text, while later subsections are missed.

I would change this:

```cpp
text.size() > 600 ? text.substr(0, 600) : text
```

to something like:

```cpp
constexpr size_t kLegislationSectionCharCap = 1800;
```

But I would not simply dump unlimited section text. Use a tiered approach:

```text
forced section:      1800-2400 chars
soft anchor:         1200-1600 chars
vector-only section: 800-1200 chars
```

For now, the simplest safe change:

```text
Raise forced-section cap from 600 to 1800.
Fetch chunks 0 and 1 for multi-chunk sections.
```

That is likely to improve `section_faithfulness` and reduce wrong subsection citations.

## 2. Parent section IDs are enough for now

Do not add subsection-level indexing immediately.

The subsection issue in the integrity check is mostly an artifact: the corpus indexes parent sections, while the check expects IDs like `NZLEG/RTA/s40(3)` or `s45(1)(bb)`. The parent sections are present.

I would normalize subsection references during retrieval:

```text
NZLEG/RTA/s40(3)      -> NZLEG/RTA/s40
NZLEG/RTA/s45(1)(bb)  -> NZLEG/RTA/s45
NZLEG/RTA/s51A(2)     -> NZLEG/RTA/s51A
```

Then preserve the subsection as a hint:

```json
{
  "requested_case_id": "NZLEG/RTA/s45(1)(bb)",
  "parent_case_id": "NZLEG/RTA/s45",
  "subsection_hint": "(1)(bb)"
}
```

Later, if eval still shows subsection misses, add subsection-level indexing only for high-traffic sections:

```text
s40
s45
s48
s51
s51A
s55
s66
s78
s100-s105
```

## 3. Add routes only for high-impact uncovered sections

The uncovered section list is useful, but do not route everything blindly. Start with sections linked to actual eval failures. The file identifies several uncovered sections that matter, including `s14`, `s38A`, `s42`, `s55`, `s78`, and `s100-s105`.

I would prioritize:

```text
P0:
  s55 - landlord failure to repair / Tribunal repair orders
  s100-s105 - Tribunal jurisdiction/procedure
  s78 - Tribunal orders/remedies

P1:
  s14 - access to premises / locked-out cases
  s38A - bond requirement
  s42 - subtenancy without consent
```

Do not make all of them hard forced routes. Some should be medium/soft route cards.

## 4. Treat external Acts as out-of-scope, but make the answer say so

The missing external Acts are intentional: HRA, Privacy Act, WPA, BoRA are not in the corpus. That is fine.

But the model should not pretend to answer those fully. Add a context/prompt guard:

```text
If the issue appears to depend mainly on an Act not present in the retrieved context, say the tenancy context may be incomplete and suggest checking the relevant agency/source. Do not invent rules from that Act.
```

This matters for discrimination and privacy questions.

## 5. Version currency: add metadata and a warning path

The document says the corpus was ingested around 2026-05, and legislation can change.

Add metadata to every legislation block:

```text
version_date: 2026-05
source_url: legislation.govt.nz
text_quality: verified
```

Then in debug:

```json
"legislation_version": "2026-05"
```

For public answers, you do not need to mention this every time, but for edge cases/new law/pets/Healthy Homes, the system should prefer official guidance if newer.

## 6. Most likely next gain

Given the post-rule-card scores, the next improvement is probably:

```text
600-char cap fix + multi-chunk forced sections
```

not more prompt work.

Expected effect:

```text
section_faithfulness: +3 to +6
accuracy:             +2 to +5
no_violation:         +1 to +4
must_include:         small improvement
```

The `must_include` gap is probably answer planning/style, not legislation retrieval. But the faithfulness and accuracy gaps should improve from better context.

## My recommended order

```text
1. Raise forced-section cap to 1800.
2. Fetch and concatenate chunks 0+1 for forced sections.
3. Normalize subsection case_ids to parent section IDs.
4. Add routes for s55, s78, and s100-s105.
5. Re-run the 50-question eval.
6. Only if subsection misses remain, add subsection-level indexing for high-traffic sections.
```

## Short answer to the open questions

1. **Raise the 600-char cap?**
   Yes. Use 1800 for forced sections, 1200 for soft anchors.

2. **Add more forced-section routes?**
   Yes, but only high-impact ones first: `s55`, `s78`, `s100-s105`, then `s14`.

3. **Subsection-level indexing now?**
   Not yet. Normalize to parent sections first and pass subsection hints.

4. **Is truncation causing wrong subsection citation?**
   Very likely, especially for `s51`, `s66`, `s40`, and Tribunal procedure sections.

5. **Are no_violation failures route/topic correlated?**
   Almost certainly. Continue the root-cause taxonomy and report no_violation by route/topic.

Overall: `nz_legal_v2` is a big improvement. The remaining issue is no longer corpus corruption; it is **context completeness and section granularity**.

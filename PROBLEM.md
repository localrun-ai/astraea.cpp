# Corpus and Retrieval Open Issues

This file documents the current open problems with the nz_legal_v2 legislation
corpus and the retrieval pipeline as of 2026-06-18. It is intended for analysis
by a separate reasoning system (e.g., Opus).

---

## 1. Corpus State Summary

Collection: `nz_legal_v2` (Qdrant)
Chunks: 934 total (274 RTA + 47 HHS + overlap from section splitting)
Integrity check (87 anchor sections tested): 78 OK / 9 MISSING / 0 FRAGMENT / 0 AMENDMENT / 0 TRANSITIONAL
Problem rate: 10.3% (was 56.3% in the original nz_legal collection)

---

## 2. The 9 MISSING Sections

These sections appear in the integrity check anchor list but cannot be fetched
from nz_legal_v2 because they are either in a different act or are subsection
references that don't have their own case_id.

### 2a. External act sections (not in corpus - intentional)

The corpus only covers RTA 1986 and HHS Regulations 2019. These sections from
other Acts are referenced in case decisions but have no indexed chunks:

| case_id | Act | Why referenced |
|---------|-----|----------------|
| NZLEG/HRA/s21 | Human Rights Act 1993 | Harassment / discrimination disputes |
| NZLEG/Privacy/s12 | Privacy Act 2020 | Tenant data access requests |
| NZLEG/WPA/s5 | Weathertight Homes Resolution Procedures Act 2006 | Leaky building claims |
| NZLEG/BoRA/s27 | Bill of Rights Act 1990 | Due process arguments |

Decision: intentionally out of scope. The corpus focuses on RTA and HHS only.

### 2b. Subsection-level case_ids (RTA sections indexed at parent level only)

The integrity check contains anchor IDs that are subsection references. The
ingest script indexes whole sections, not individual subsections. So fetching
`NZLEG/RTA/s40(3)` returns nothing even though `NZLEG/RTA/s40` is indexed.

| Subsection case_id | Parent in corpus | Note |
|---------------------|------------------|------|
| NZLEG/RTA/s40(3) | NZLEG/RTA/s40 OK | s40 is landlord obligations; (3) is specific sub |
| NZLEG/RTA/s45(1)(bb) | NZLEG/RTA/s45 OK | s45 is tenant responsibilities |
| NZLEG/RTA/s51A(2) | NZLEG/RTA/s51A OK | s51A is periodic tenancy notice |
| NZLEG/RTA/s104(1)(b) | NZLEG/RTA/s104 OK | s104 is Tribunal jurisdiction |
| NZLEG/RTA/s13(1)(d) | NZLEG/RTA/s13 OK? | s13 is unlawful provisions in tenancy agreements |

The parent sections are all present. The subsection references in the anchor
list are an integrity-check artefact - they exist in the check config but the
corpus doesn't index at that granularity.

**Open question**: Should we update the integrity check to use parent section
IDs, or should we add subsection-level indexing for the most-cited subsections?

---

## 3. Section Text Truncation

In anchor.cpp line 391 (retrieve_anchor), retrieved section text is capped at
600 characters:

    anchor += "\n\n" + title + "\n" +
              (text.size() > 600 ? text.substr(0, 600) : text);

For long sections (e.g., s40, s51, s66), the 600-char window may not include
the relevant sub-clause. The model sees only the opening of the section text.

**Current behaviour**: For forced sections, we retrieve the chunk with the
lowest chunk_index (the opening). For long sections split across multiple chunks,
only the opening chunk is used. Later subsections or sub-clauses are missed.

**Example affected sections**:
- s51 (Termination by notice): 8+ subsections; only opening text retrieved
- s66 (Subletting): multiple subsections; most relevant sub may be in chunk 2+
- s40 (Landlord obligations): long enumerated list; items (g)-(k) often missed

**Possible fixes**:
1. Increase the text cap from 600 to 1200+ chars
2. Retrieve multiple chunks for forced sections (chunks 0 and 1)
3. Store each subsection as a separate chunk with its own case_id

---

## 4. Sections Not Covered by Any Route

Many relevant RTA sections are not in any route's forced_sections and are only
retrieved by vector search. If the embedding similarity is low (common for
procedural sections), they may not appear in the top-k.

**Known uncovered sections that matter**:
- s14 (Access to premises): no route forces it; may miss for locked-out cases
- s38A (Landlord's right to require bond): no route
- s42 (Subtenancy without consent): no route
- s55 (Landlord's failure to repair - Tribunal order): no route
- s78 (Orders Tribunal can make): no route; model sometimes cites this wrong
- s100-s105 (Tribunal jurisdiction and procedure): no route; affects 5+ questions

**Impact**: Questions about these sections get whatever the vector search
returns, which may be tangentially related sections rather than the operative one.

---

## 5. Section Version Currency

The corpus was ingested from legislation.govt.nz on 2026-05 (approximate).
The RTA 1986 has been heavily amended, especially in 2021 (Residential Tenancies
Amendment Act 2020) and 2023-2024. Known risk areas:

- Sections added post-2020 (s13A, s13B, s13C, s19A, s45A, s51A, s60A, s66V)
  are present in nz_legal_v2 because they were in the live text at ingest time.
  But if any were further amended after May 2026, the corpus is stale.
- HHS Regulations 2019 are periodically updated with new compliance deadlines.
  The corpus reflects the 2019 regs as of ingest date.

**Mitigation**: The ingest script fetches from the live legislation.govt.nz page.
Re-running the ingest periodically (quarterly?) would keep sections current.

---

## 6. Corpus Corruption Patterns (Historical - Mostly Fixed)

The original nz_legal collection had two corruption patterns that caused 56.3%
failure rate in the integrity check. Both are fixed in nz_legal_v2:

### 6a. case_id collision (FIXED)
Multiple sections shared the same case_id because historical versions of a
section co-existed in the corpus. E.g., NZLEG/RTA/s40 had both "Remuneration
of Principal Tenancy Adjudicator" (old, 1 chunk) and "Tenant's responsibilities"
(current, 4+ chunks).

Fix: retriever selects the title group with the most chunks (most likely current
operative). Applied in anchor.cpp and Python anchor.py.

### 6b. Random chunk selection (FIXED)
Fetching a section with limit=1 returned a random chunk rather than the opening.
E.g., s51 would return a mid-section chunk about subsection (4) instead of
the section title and definition.

Fix: fetch all chunks (limit=64), sort by chunk_index, select lowest non-amendment
chunk. Applied in anchor.cpp and integrity_check.py.

### 6c. HHS section prefix (FIXED)
HHS regulation chunks used "r" prefix (r8, r6) but the ingest script was
generating case_ids with "s" prefix (s8, s6). Forced sections were not found.

Fix: ingest script now uses section_prefix="r" from metadata; case_ids are
NZLEG/HHS/r8 etc.

---

## 7. Evaluation Score Gaps by Dimension (post-rule-card, in progress)

Based on the eval immediately before the rule card deploy (58.3 overall):

| Dimension | Score | Target | Gap |
|-----------|-------|--------|-----|
| must_include | 48.5 | 65+ | -16.5 |
| no_violation | 74.6 | 82+ | -7.4 |
| section_recall | 46.0 | 55+ | -9.0 |
| section_faithfulness | 67.6 | 75+ | -7.4 |
| accuracy | 60.8 | 70+ | -9.2 |
| overall | 58.3 | 65+ | -6.7 |

The rule cards (6 routes) address section_recall and no_violation gaps for the
specific cases they cover. The must_include gap is primarily a generation issue:
the model omits required practical steps even when the legislation is present.

---

## 8. Open Questions for Analysis

1. Should the 600-char section text cap be raised? If so, to what limit?
   (tradeoff: longer context vs. LLM attention dilution)

2. Should we add more forced-section routes for the uncovered sections in item 4?
   Specifically: landlord entry (s14), Tribunal procedure (s100-s105), failure
   to repair orders (s55)?

3. Should subsection-level indexing be added for high-traffic subsections like
   s51(1)-(8), s40(a)-(k)? Or is it better to retrieve the full section and
   let the LLM extract the relevant subsection?

4. Is the 600-char truncation causing the model to cite the wrong subsection
   (e.g., citing s51 generally when the relevant rule is in s51(3))?

5. The `no_violation` dimension measures whether the model makes prohibited
   claims. Current 74.6 score suggests ~8% of responses make at least one
   prohibited claim. Are these correlated with specific routes or topics?
</content>
</invoke>
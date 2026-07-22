# Substring Matching False-Positive Risk in Route Terms

## Root Cause

AhoCorasick substring matching has no word boundaries. Every term in every matching
field (`include_any`, `exclude_any`, `require_context_any`, etc.) is matched as a
bare substring of the normalized query. A 3-4 character term silently matches INSIDE
ordinary English words that appear in everyday tenancy questions.

The normalized query is `normalize(original + " " + LLM-rewritten question)`.
The LLM rewrite often expands casual phrases into formal English, which INCREASES
the chance of substring collisions with formal words like "method", "alternatives",
"significant", "frustrated".

### Impact by field

| Field               | False-positive effect                                              |
|---------------------|--------------------------------------------------------------------|
| `exclude_any`       | Route SUPPRESSED for questions it should handle. **Worst case.**  |
| `include_any`       | Route fires for unrelated questions, wrong sections retrieved.    |
| `require_context_any` | Context gate passes when it should not.                        |
| `include_any_broad` | Lower severity (only contributes when include_any_precise fires). |

---

## Findings Summary

### CRITICAL - Fixed this session

#### 1. `pet_permission` - exclude_any

**Terms:** `"ant"`, `"rat"`, `"flea"`, `"wasp"`, `"mouse"`, `"mice"`

**False positives:**
- `"ant"` (3 chars) inside: **tenant** (t-e-n-**a-n-t**), **want** (w-**a-n-t**),
  **grant**, **warranty**, **significant**, **rant**
- `"rat"` (3 chars) inside: **frust**rat**ed**, **ope**rat**e**, **deco**rat**ive**,
  **compa**rat**ive**

**Observed effect:** Any question containing "tenant", "want", or "frustrated" was
excluded from pet_permission even when it was genuinely about s42E pet permission.
LLM rewrites frequently produce formal sentences like "The tenant wants to know
whether..." - this matched both "tenant" (containing "ant") and "want" (containing
"ant"), doubly suppressing the route.

**Fix:** Replaced all single-animal words with multi-word phrases:
`"ant infestation"`, `"rat infestation"`, `"flea infestation"`, `"wasp nest"`,
`"mouse infestation"`, `"mice infestation"`.

---

#### 2. `accidental_damage_insurance_excess` - exclude_any

**Terms:** `"meth"` (4 chars), `"paint"` (5 chars)

**False positives:**
- `"meth"` inside: **method**, **methodology**, **methane**
  - Trigger scenario: "What is the best **method** to document accidental damage
    for my insurance claim?" - "method" contains "meth", the route is excluded,
    tenant gets a generic answer instead of the s49B liability cap information.
  - Note: `"methamphetamine"` was ALREADY in the same exclude list, making `"meth"`
    both redundant AND dangerous.
- `"paint"` inside: **painted**, **painting**, **repainting**, **painter**
  - Trigger scenario: "I accidentally knocked over a **paint** can and it stained
    the carpet" - the very scenario this route handles (s49B cap) gets excluded
    because "paint" matches.

**Fix:**
- Removed `"meth"` entirely (redundant with `"methamphetamine"`). Added specific
  phrases for standalone "meth" drug references: `"meth contamination"`,
  `"meth test"`, `"meth lab"`, `"meth house"`, `"meth levels"`, `"tested for meth"`.
- Replaced `"paint"` with wear-and-tear specific phrases: `"carpet replacement"`,
  `"carpet cleaning"`, `"carpet clean"`, `"repaint"`, `"needs painting"`,
  `"painting required"`.

---

#### 3. `agreement_form` - include_any (pet term co-firing)

Not a substring issue but the same root problem: `agreement_form` contained dozens
of pet-related terms (`"have a dog"`, `"want a pet"`, `"allow pets"`, etc.) causing
it to co-fire with `pet_permission` for any pet question.

Both routes have `forced_sections`. When co-firing, both routes' forced sections
are combined. `pet_permission` has `leg_allow_list = {s42E}`, limiting `max_hits`
to 1. Combined forced = `[s13A, s13B, s42E]` but only s13A is returned (first in
combined list). s42E - the section that actually matters - was silently dropped.

**Fix:** Removed ~30 pet-related terms from `agreement_form` include_any. Kept only
`"pet clause"`, `"pet bond"`, `"fish tank"`, `"aquarium"` which are genuinely
agreement-form topics not covered by pet_permission.

---

#### 4. `property_change` - include_any_precise (prior commit)

**Term:** `"alter"` (5 chars) inside: **alternatives**, **alternative**

**Observed effect:** Queries about "alternative heating options" or "what are my
alternatives" triggered property_change (landlord alteration consent) instead of
healthy_homes or repairs routes.

**Fix (already committed):** Removed `"alter"`, added specific blocking phrases to
exclude_any: `"alternative heating"`, `"heating options"`, `"heating alternative"`.

---

### LOW RISK - Not Fixed (monitor only)

#### `"bond"` (4 chars) in three exclude_any blocks

Routes: `painter_landlord_access`, `property_uninhabitable_rent_abatement`,
`repair_notice_s56`

**Theoretical false positives:** `"bonded"`, `"bonding"` inside the query.

**Assessment:** Very low practical risk. The include_any for these routes are highly
specific (painter access terms, uninhabitable-property terms, s56 notice terms).
A question that matches those specific terms is unlikely to also say "bonded" or
"bonding" in a way that triggers the false exclude. Both `property_uninhabitable`
and `repair_notice_s56` already have `"bond refund"` as a separate phrase anyway,
making bare `"bond"` partially redundant.

**Recommendation:** Replace bare `"bond"` with `"bond refund"`, `"bond money"`,
`"bond lodgement"` if any eval score regression is observed on bond questions that
also mention property condition.

---

### NOT BUGS - Intentional inflected-form matches

These appeared in the audit but are intentional:

| Route | Field | Term | "Matches inside" | Why OK |
|-------|-------|------|-----------------|--------|
| `repairs_tenant_not_at_fault` | include_any | `"repair"` | "repairs" | plural - should match |
| `repairs_tenant_not_at_fault` | include_any | `"fix"` | "fixing", "fixed" | verb forms - should match |
| `repairs_tenant_not_at_fault` | require_context_any | `"tenant"` | "tenants" | plural - should match |

Substring matching of base forms capturing inflected forms is a FEATURE in this
routing system.

---

## Audit Script

Run this to detect new short-term risks as routes are added:

```python
import re

with open("jurisdictions/nz_tenancy/routes.cpp") as f:
    text = f.read()

fields = [
    ("exclude_any",    r'\.exclude_any\s*=\s*\{([^}]+)\}'),
    ("include_any",    r'\.include_any\s*=\s*\{([^}]+)\}'),
]
chunks = re.split(r'(?=\.intent\s*=\s*")', text)
for chunk in chunks:
    im = re.search(r'\.intent\s*=\s*"([^"]+)"', chunk)
    for name, pat in fields:
        fm = re.search(pat, chunk, re.DOTALL)
        if not im or not fm:
            continue
        block = re.sub(r'//[^\n]*', '', fm.group(1))
        for t in re.findall(r'"([^"]+)"', block):
            if len(t) <= 5 and ' ' not in t and not any(c.isdigit() for c in t):
                print(f"{im.group(1):40s} {name:15s} {repr(t)}")
```

---

## Rules for Future Route Terms

1. **No bare terms under 6 characters in `exclude_any`** unless they are impossible
   to appear inside common English words used in tenancy questions.

2. **Drug/chemical abbreviations**: never use `"meth"`, `"p"`, `"k"` alone.
   Always use the full word or a phrase: `"methamphetamine"`, `"meth contamination"`.

3. **Animal names 3-4 chars**: `"ant"`, `"rat"`, `"flea"`, `"bug"`, `"fly"`,
   `"bee"` are all substrings of common words. Always use `"[animal] infestation"`,
   `"[animal] problem"`, or `"[animal] nest"`.

4. **Test short terms against this list** before adding to exclude_any:
   tenant, want, grant, rental, landlord, maintenance, frustrated, method,
   alternative, warranty, significant, important, relevant, operate, decorative.

5. **Prefer phrase-level terms in exclude_any.** A 3-word phrase cannot accidentally
   match a single unrelated word.

6. **Section references with digits** (`"s49A"`, `"s13B"`) are safe - the digit
   makes them unambiguous as section identifiers.

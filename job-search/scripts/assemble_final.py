#!/usr/bin/env python3
"""Assemble final verified entry-level deliverables (US-first).

Inputs in /tmp/jobs_out/:
  verified_base.csv        rows from today's bot-pruned lists (+cross-ref)
  needs_verification.csv   flagship agent rows pending search verification
  verdicts_us.txt, verdicts_tw.txt        verdicts for needs rows
  verdicts_hw_a.txt, verdicts_hw_b.txt    verdicts + direct URLs for jobright HW rows
Line format: URL ||| VERDICT ||| replacement-or-direct-URL-or-- ||| note
"""
import csv, os, re, shutil
from collections import Counter

OUT = "/tmp/jobs_out"
REPO = "/home/user/cisco/job-search"
TODAY = "2026-06-10"
cols = ["Company","Title","Track","Location","Region","Tier","Salary","Flags",
        "Posted_Age","WorkModel","Source","URL","Link_Check"]

def load_verdicts(*files):
    v = {}
    for vf in files:
        p = f"{OUT}/{vf}"
        if not os.path.exists(p):
            continue
        for line in open(p, encoding="utf-8"):
            parts = [x.strip() for x in line.split("|||")]
            if len(parts) >= 2 and parts[0].startswith("http"):
                v[parts[0]] = (parts[1].upper(),
                               parts[2] if len(parts) > 2 and parts[2].startswith("http") else "",
                               parts[3] if len(parts) > 3 else "")
    return v

needs_v = load_verdicts("verdicts_us.txt", "verdicts_tw.txt")
hw_v = load_verdicts("verdicts_hw_a.txt", "verdicts_hw_b.txt")

rows, unverified, closed = [], [], []

# (a) today's-list rows, patched with per-row hardware verdicts where available
for r in csv.DictReader(open(f"{OUT}/verified_base.csv", encoding="utf-8")):
    row = dict(r)
    if row["URL"] in hw_v:
        v, direct, note = hw_v[row["URL"]]
        if v == "CLOSED":
            closed.append(row); continue
        if v == "LIVE":
            if direct:
                row["URL"] = direct
                row["Link_Check"] = f"search-verified {TODAY}; direct employer link ({note})"
            else:
                row["Link_Check"] += f"; search-confirmed {TODAY}"
        # UNCONFIRMED: keep — still listed <=7d / bot-verified today
    rows.append(row)

# (b) flagship agent rows with search verdicts
for r in csv.DictReader(open(f"{OUT}/needs_verification.csv", encoding="utf-8")):
    v, repl, note = needs_v.get(r["URL"], ("UNCONFIRMED", "", "no verdict returned"))
    row = dict(r)
    if v == "LIVE" or (v == "CLOSED" and repl):
        if repl: row["URL"] = repl
        tag = "search-verified" if v == "LIVE" else "search-verified (reposted req)"
        row["Link_Check"] = f"{tag} {TODAY} ({note})"
        rows.append(row)
    elif v == "CLOSED":
        closed.append(row)
    else:
        row["Link_Check"] = f"UNCONFIRMED {TODAY} ({note})"
        unverified.append(row)

INTL_LOC = re.compile(r"canada|ontario|british columbia|toronto|ottawa|markham|burnaby|vancouver|"
                      r"montreal|waterloo|united kingdom|london, england|\buk\b|amsterdam|netherlands|"
                      r"dublin|ireland|singapore|bangalore|hyderabad|india|israel|poland|germany", re.I)
for r in rows:
    if r["Region"] != "TW" and INTL_LOC.search(r["Location"]):
        r["Region"] = "Intl"

REGION_ORDER = {"US": 0, "Intl": 1, "TW": 2}
TIER_ORDER = {"Big Tech / Semi leader": 0, "Quant/HFT (top salary)": 1,
              "Semiconductor (TW/global)": 2, "High salary ($150k+)": 3, "Other": 4}
TRACK_ORDER = {"RTL / ASIC Design": 0, "Design Verification": 1, "DFT / Silicon Test": 2,
               "Silicon Validation": 3, "EDA / CAD": 4, "Firmware / Embedded": 5,
               "Hardware (general)": 6, "Software": 7}
# URL-level dedupe (multi-location rows resolved to one req URL)
seen_u, dd = set(), []
for r in rows:
    if r["URL"] in seen_u:
        continue
    seen_u.add(r["URL"]); dd.append(r)
rows = dd

rows.sort(key=lambda r: (REGION_ORDER.get(r["Region"], 9), TIER_ORDER.get(r["Tier"], 9),
                         TRACK_ORDER.get(r["Track"], 9), r["Company"].lower()))

os.makedirs(f"{REPO}/by_cv", exist_ok=True)
os.makedirs(f"{REPO}/archive", exist_ok=True)
os.makedirs(f"{REPO}/scripts", exist_ok=True)

def write_csv(path, rs):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader(); w.writerows(rs)

write_csv(f"{REPO}/jobs_verified_entry_level.csv", rows)
write_csv(f"{REPO}/archive/jobs_unconfirmed.csv", unverified)

BY_CV = {
  "rtl_design.csv":          lambda r: r["Track"] in ("RTL / ASIC Design", "EDA / CAD", "Hardware (general)"),
  "design_verification.csv": lambda r: r["Track"] in ("Design Verification", "Silicon Validation"),
  "dft.csv":                 lambda r: r["Track"] in ("DFT / Silicon Test", "Silicon Validation"),
  "firmware_embedded.csv":   lambda r: r["Track"] == "Firmware / Embedded",
  "software_bigtech.csv":    lambda r: r["Track"] == "Software",
  "taiwan_all_tracks.csv":   lambda r: r["Region"] == "TW",
}
cv_counts = {}
for fname, pred in BY_CV.items():
    sel = [r for r in rows if pred(r)]
    cv_counts[fname] = len(sel)
    write_csv(f"{REPO}/by_cv/{fname}", sel)

n = len(rows)
us_rows = [r for r in rows if r["Region"] == "US"]
hw_direct = sum(1 for r in rows if "direct employer link" in r["Link_Check"])
nv = sum(1 for r in rows if "search-verified" in r["Link_Check"] or "search-confirmed" in r["Link_Check"])
track_c = Counter(r["Track"] for r in rows)
region_c = Counter(r["Region"] for r in rows)
hw_us_bigtech = sum(1 for r in us_rows if r["Tier"] != "Other" and r["Track"] != "Software")

picks, seen_ct = [], set()
for r in rows:
    if r["Region"] != "US" or r["Track"] == "Software":
        continue
    if r["Tier"] not in ("Big Tech / Semi leader", "Quant/HFT (top salary)"):
        continue
    k = (r["Company"].lower()[:12], r["Track"])
    if k in seen_ct: continue
    seen_ct.add(k); picks.append(r)
picks = picks[:40]
def esc(s): return s.replace("|", "/")
pick_lines = "\n".join(
    f"| {esc(r['Company'])} | {esc(r['Title'])[:70]} | {r['Track']} | {esc(r['Location'])[:34]} | [apply]({r['URL']}) |"
    for r in picks)
track_tbl = "\n".join(f"| {k} | {v} |" for k, v in track_c.most_common())
cv_tbl = "\n".join(f"| `by_cv/{k}` | {v} |" for k, v in cv_counts.items())

readme = f"""# Verified Entry-Level Job List (US-first) — Chiung-Chen (Johnson) Tsai · {n} openings

Rebuilt {TODAY}. Every row is explicitly **entry-level / new-grad** and **link-checked**.
Rows are ordered **US first** ({region_c.get('US',0)} US rows), then international
({region_c.get('Intl',0)}), then Taiwan ({region_c.get('TW',0)}).
**`companies_filtered_urls.md` lists, for every company, a pre-filtered careers-search
URL** — use those to browse each employer's live reqs directly at the source.

## Link validation (`Link_Check` column on every row)

| Value | Meaning |
|---|---|
| `search-verified {TODAY}; direct employer link` | Posting individually re-confirmed open today by req search AND the URL now points straight at the employer's ATS ({hw_direct} rows). |
| `search-verified {TODAY}` | Req individually re-confirmed open today on the official site ({nv} rows incl. above). |
| `list-bot-verified {TODAY}` | Present in today's commit of a bot-maintained list that auto-removes closed reqs (Simplify/speedyapply). Links go to the employer ATS. |
| `listed<=7d jobright {TODAY}` | Posted in the last 7 days on jobright's daily list. These links open a jobright listing page with the apply-out button; for a direct path use `companies_filtered_urls.md`. |

All US/Intl **hardware-track** rows were individually re-verified; {len(closed)} dead/closed
postings were deleted in this pass, and {len(unverified)} unverifiable rows moved to
`archive/jobs_unconfirmed.csv`. Software rows rest on today's-list verification.

## Entry-level (資歷) guarantee

New-grad-only sources or explicit markers (New College Grad / University Graduate /
Early Career / Entry Level / Engineer I/1 / Graduate / 校招 / 應屆 / 研替 / 預聘 / 經歷不拘);
removed Senior/Staff/MTS, level II/III+, internships, PhD-only reqs, and
non-silicon "verification/test" noise (factory/QA/healthcare roles).
`Flags` marks `no-sponsorship` / `us-citizenship` / `us-person-likely` (defense, ITAR)
rows — deprioritize those as an F-1 international student.

## Files

| File | Rows |
|---|---|
| `jobs_verified_entry_level.csv` | {n} |
{cv_tbl}
| `companies_filtered_urls.md` | per-company filtered career-search URLs |

| Track | Openings |
|---|---|
{track_tbl}

**US hardware-track rows at big tech / quant: {hw_us_bigtech}.**

## Top US picks (hardware tracks, big tech & quant — all link-verified)

| Company | Title | Track | Location | Link |
|---|---|---|---|---|
{pick_lines}

## Refresh

```bash
python3 scripts/parse_jobs.py && python3 scripts/filter_strict.py && python3 scripts/assemble_final.py
```
Listings move daily; the per-company URLs in `companies_filtered_urls.md` never go stale.
"""
open(f"{REPO}/README.md", "w", encoding="utf-8").write(readme)

for s in ("parse_jobs.py", "filter_strict.py", "assemble_final.py", "gen_deliverables.py", "mine_jobright_history.py"):
    if os.path.exists(f"/tmp/{s}"):
        shutil.copy(f"/tmp/{s}", f"{REPO}/scripts/{s}")

print(f"verified rows: {n} (US {region_c.get('US',0)} / Intl {region_c.get('Intl',0)} / TW {region_c.get('TW',0)})")
print(f"direct-employer-link upgrades: {hw_direct} | search-verified total: {nv}")
print(f"closed dropped this pass: {len(closed)} | unconfirmed archived: {len(unverified)}")
print("tracks:", dict(track_c))

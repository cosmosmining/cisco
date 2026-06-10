#!/usr/bin/env python3
"""Generate job-search deliverables (README + CSV splits) from harvested data."""
import csv, os, re
from collections import Counter

REPO = "/home/user/cisco/job-search"
OUT = "/tmp/jobs_out"
os.makedirs(f"{REPO}/by_cv", exist_ok=True)
os.makedirs(f"{REPO}/scripts", exist_ok=True)

cols = ["Company","Title","Track","Location","Region","Tier","Salary","Flags","Posted_Age","WorkModel","Source","URL"]
rows = list(csv.DictReader(open(f"{OUT}/master.csv", encoding="utf-8")))

# optional agent-sourced files (same columns), deduped against list rows
STOP = {"us", "usa", "united", "states", "america", "of", "see", "posting"}
def rid_of(url):
    for pat in (r"(JR\d{6,})", r"[_/](R\d{5,})\b", r"jobs/(\d{6,})", r"/job/(\d{6,})",
                r"details/(\d{6,})", r"gh_jid=(\d+)", r"jobs/info/([a-f0-9]{12,})"):
        m = re.search(pat, url)
        if m: return m.group(1)
    return None
def soft_key(r):
    titlenorm = re.sub(r"new (college )?grad(uate)?|university grad(uate)?|early career|entry level|20\d\d|[^a-z0-9]",
                       "", r["Title"].lower())
    loctok = "".join(sorted(set(re.findall(r"[a-z]+", r["Location"].lower())) - STOP))
    comp = re.sub(r"[^a-z0-9]", "", r["Company"].lower())[:10]
    return comp + titlenorm + loctok

seen_rid = {rid_of(r["URL"]) for r in rows if rid_of(r["URL"])}
seen_soft = {soft_key(r) for r in rows}
for extra in ("taiwan.csv", "us_verified.csv"):
    p = f"{OUT}/{extra}"
    if not os.path.exists(p):
        continue
    added = 0
    for r in csv.DictReader(open(p, encoding="utf-8")):
        row = {c: r.get(c, "") for c in cols}
        rid, sk = rid_of(row["URL"]), soft_key(row)
        if (rid and rid in seen_rid) or sk in seen_soft:
            continue
        if rid: seen_rid.add(rid)
        seen_soft.add(sk)
        rows.append(row); added += 1
    print(f"{extra}: +{added} new rows after dedupe")

TIER_ORDER = {"Big Tech / Semi leader": 0, "Quant/HFT (top salary)": 1,
              "Semiconductor (TW/global)": 2, "High salary ($150k+)": 3, "Other": 4}
TRACK_ORDER = {"RTL / ASIC Design": 0, "Design Verification": 1, "DFT / Silicon Test": 2,
               "Silicon Validation": 3, "EDA / CAD": 4, "Firmware / Embedded": 5,
               "Hardware (general)": 6, "Software": 7}
rows.sort(key=lambda r: (TIER_ORDER.get(r["Tier"], 9), TRACK_ORDER.get(r["Track"], 9), r["Company"].lower()))

def write_csv(path, rs):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols); w.writeheader(); w.writerows(rs)

write_csv(f"{REPO}/jobs_master.csv", rows)

BY_CV = {
  "rtl_design.csv":            lambda r: r["Track"] in ("RTL / ASIC Design", "EDA / CAD", "Hardware (general)"),
  "design_verification.csv":   lambda r: r["Track"] in ("Design Verification", "Silicon Validation"),
  "dft.csv":                   lambda r: r["Track"] in ("DFT / Silicon Test", "Silicon Validation"),
  "firmware_embedded.csv":     lambda r: r["Track"] == "Firmware / Embedded",
  "software_bigtech.csv":      lambda r: r["Track"] == "Software",
}
cv_counts = {}
for fname, pred in BY_CV.items():
    sel = [r for r in rows if pred(r)]
    cv_counts[fname] = len(sel)
    write_csv(f"{REPO}/by_cv/{fname}", sel)

tw = [r for r in rows if r["Region"] == "TW"]
if tw:
    write_csv(f"{REPO}/by_cv/taiwan_all_tracks.csv", tw)
    cv_counts["taiwan_all_tracks.csv"] = len(tw)

track_c = Counter(r["Track"] for r in rows)
tier_c = Counter(r["Tier"] for r in rows)
n = len(rows)
hw_bigtech = sum(1 for r in rows if r["Tier"] != "Other" and r["Track"] != "Software")

ENTRY = re.compile(r"new (college )?grad|university grad|early career|engineer (i|1)\b|junior|campus|2026|2027", re.I)
picks = [r for r in rows if r["Tier"] in ("Big Tech / Semi leader", "Quant/HFT (top salary)")
         and r["Track"] != "Software" and ENTRY.search(r["Title"])]
# one per company+track for variety
seen_ct, top = set(), []
for r in picks:
    k = (r["Company"].lower()[:12], r["Track"])
    if k in seen_ct: continue
    seen_ct.add(k); top.append(r)
top = top[:40]

def md_escape(s): return s.replace("|", "/")

pick_lines = "\n".join(
    f"| {md_escape(r['Company'])} | {md_escape(r['Title'])[:70]} | {r['Track']} | {md_escape(r['Location'])[:36]} | {r['Salary'] or '—'} | [apply]({r['URL']}) |"
    for r in top)

track_tbl = "\n".join(f"| {k} | {v} |" for k, v in track_c.most_common())
tier_tbl = "\n".join(f"| {k} | {v} |" for k, v in tier_c.most_common())
cv_tbl = "\n".join(f"| `by_cv/{k}` | {v} |" for k, v in cv_counts.items())

readme = f"""# Job Search — Chiung-Chen (Johnson) Tsai · {n} matched openings

Generated 2026-06-10. Entry-level (new-grad / 0–2 yr) openings matched to the five CVs
(Design Verification, DFT, RTL Design ×2, Firmware/Software), restricted to roles a
**CMU MS ECE Dec-2026 grad** actually qualifies for (the 資歷 filter, see below).

## What's here

| File | Rows |
|---|---|
| `jobs_master.csv` | {n} |
{cv_tbl}

Columns: Company, Title, Track, Location, Region, Tier, Salary (where published),
Flags, Posted_Age, WorkModel, Source, URL (direct application link).

## Counts

| Track | Openings |
|---|---|
{track_tbl}

| Tier | Openings |
|---|---|
{tier_tbl}

**Hardware-track openings at Big Tech / top-salary firms: {hw_bigtech}.**

## How the 資歷 (qualification) filter was applied

1. **Sources are new-grad-only curated lists** (SimplifyJobs New-Grad-Positions,
   jobright-ai 2026 Engineering & SWE New-Grad — including 10 weeks of their git
   history with original posted dates, speedyapply 2026-SWE-College-Jobs,
   vanshb03 New-Grad-2026 — all updated daily) plus live career-site sweeps for
   Taiwan and US flagship programs. Every row is an entry-level requisition.
   Rows posted in April–May are mostly still open but check the link; the
   `Posted_Age` column tells you what to re-verify first.
2. Titles containing Senior/Staff/Principal/Lead/Manager and internships were removed.
3. Closed postings (🔒) were removed; same req across sources deduplicated.
4. Non-matching disciplines (mechanical, analog/RF, PCB, process, etc.) were removed.
5. `Flags` column marks 資歷 risks that go beyond skills:
   - `no-sponsorship` (🛂) / `us-citizenship` (🇺🇸) — from the source lists;
   - `us-person-likely` — defense/ITAR employers (Lockheed, Northrop, SpaceX, Anduril…).
   As an international student on F-1/OPT you should deprioritize flagged rows.

## Track ↔ CV mapping

| Your CV | Use file | Typical matching titles |
|---|---|---|
| Resume_RTL_Design / RTL_Design3 | `by_cv/rtl_design.csv` | RTL/ASIC Design Engineer NCG, Low Power ASIC, Logic Design, CAD/EDA |
| Resume_Design_Verification | `by_cv/design_verification.csv` | ASIC DV Engineer 1, Verification NCG, Emulation, Silicon Validation |
| Resume_DFT | `by_cv/dft.csv` | DFT Engineer NCG, Silicon Test, Product/Test Engineering |
| Resume_Software (firmware-focused) | `by_cv/firmware_embedded.csv` | Embedded SW Engineer I, Firmware NCG |
| Resume_Software (general SWE) | `by_cv/software_bigtech.csv` | SWE I / New Grad at Big Tech, AI labs, quant ($150k+) |

## Top picks (Big Tech & quant, hardware tracks)

| Company | Title | Track | Location | Salary | Link |
|---|---|---|---|---|---|
{pick_lines}

## Timing advice (graduating Dec 2026)

- You can apply NOW to "New College Grad 2026" reqs (start dates within 2026) and
  any "Engineer I / Early Career" req — you're available from Jan 2027.
- The big 2027-start NCG wave (NVIDIA/Apple/Google/AMD/Qualcomm) opens **Aug–Oct 2026**;
  re-run the refresh below weekly from August.
- Taiwan campus hiring (MediaTek 聯發科/TSMC 台積電 校園徵才) for 2027 starts ~Sep 2026;
  MediaTek/TSMC accept 預聘 applications year-round.

## Refresh

```bash
# re-pull the source lists and regenerate every file in this folder
python3 scripts/parse_jobs.py && python3 scripts/gen_deliverables.py
```

*Listings move fast — rows here were live as of 2026-06-10; always confirm on the linked page.*
"""
open(f"{REPO}/README.md", "w", encoding="utf-8").write(readme)

import shutil
shutil.copy("/tmp/parse_jobs.py", f"{REPO}/scripts/parse_jobs.py")
shutil.copy("/tmp/gen_deliverables.py", f"{REPO}/scripts/gen_deliverables.py")
print(f"deliverables written: {n} total rows; hw@bigtech {hw_bigtech}; top picks {len(top)}")

#!/usr/bin/env python3
"""Assemble final verified entry-level deliverables.

Inputs:
  /tmp/jobs_out/verified_base.csv        rows from today's bot-pruned lists (+cross-ref)
  /tmp/jobs_out/needs_verification.csv   agent rows pending search verification
  /tmp/jobs_out/verdicts_us.txt, verdicts_tw.txt   lines: URL ||| VERDICT ||| replacement ||| note
Output: repo job-search/ rewritten with verified-only files; unverified + old master to archive/.
"""
import csv, os, re, shutil
from collections import Counter

OUT = "/tmp/jobs_out"
REPO = "/home/user/cisco/job-search"
TODAY = "2026-06-10"
cols = ["Company","Title","Track","Location","Region","Tier","Salary","Flags",
        "Posted_Age","WorkModel","Source","URL","Link_Check"]

verdicts = {}
for vf in ("verdicts_us.txt", "verdicts_tw.txt"):
    p = f"{OUT}/{vf}"
    if not os.path.exists(p):
        continue
    for line in open(p, encoding="utf-8"):
        parts = [x.strip() for x in line.split("|||")]
        if len(parts) >= 2 and parts[0].startswith("http"):
            verdicts[parts[0]] = (parts[1].upper(),
                                  parts[2] if len(parts) > 2 and parts[2].startswith("http") else "",
                                  parts[3] if len(parts) > 3 else "")

rows = list(csv.DictReader(open(f"{OUT}/verified_base.csv", encoding="utf-8")))
unverified, closed = [], []
for r in csv.DictReader(open(f"{OUT}/needs_verification.csv", encoding="utf-8")):
    v, repl, note = verdicts.get(r["URL"], ("UNCONFIRMED", "", "no verdict returned"))
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

TIER_ORDER = {"Big Tech / Semi leader": 0, "Quant/HFT (top salary)": 1,
              "Semiconductor (TW/global)": 2, "High salary ($150k+)": 3, "Other": 4}
TRACK_ORDER = {"RTL / ASIC Design": 0, "Design Verification": 1, "DFT / Silicon Test": 2,
               "Silicon Validation": 3, "EDA / CAD": 4, "Firmware / Embedded": 5,
               "Hardware (general)": 6, "Software": 7}
rows.sort(key=lambda r: (TIER_ORDER.get(r["Tier"], 9), TRACK_ORDER.get(r["Track"], 9), r["Company"].lower()))

os.makedirs(f"{REPO}/by_cv", exist_ok=True)
os.makedirs(f"{REPO}/archive", exist_ok=True)
os.makedirs(f"{REPO}/scripts", exist_ok=True)

def write_csv(path, rs):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader(); w.writerows(rs)

write_csv(f"{REPO}/jobs_verified_entry_level.csv", rows)
write_csv(f"{REPO}/archive/jobs_unconfirmed.csv", unverified)

# archive the previous wide-net master if present at old path
old_master = f"{REPO}/jobs_master.csv"
if os.path.exists(old_master):
    shutil.move(old_master, f"{REPO}/archive/jobs_master_unfiltered.csv")

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
track_c = Counter(r["Track"] for r in rows)
tier_c = Counter(r["Tier"] for r in rows)
hw_bigtech = sum(1 for r in rows if r["Tier"] != "Other" and r["Track"] != "Software")
nv = sum(1 for r in rows if "search-verified" in r["Link_Check"])

ENTRY = re.compile(r"new ?(college )?grad|university grad|early career|entry|graduate|engineer (i|1)\b|junior|校招|研替|預聘|應屆", re.I)
picks, seen_ct = [], set()
for r in rows:
    if r["Tier"] not in ("Big Tech / Semi leader", "Quant/HFT (top salary)") or r["Track"] == "Software":
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
tier_tbl = "\n".join(f"| {k} | {v} |" for k, v in tier_c.most_common())
cv_tbl = "\n".join(f"| `by_cv/{k}` | {v} |" for k, v in cv_counts.items())

readme = f"""# Verified Entry-Level Job List — Chiung-Chen (Johnson) Tsai · {n} openings

Rebuilt {TODAY} after a strict pass: **every row is (a) explicitly entry-level /
new-grad and (b) link-validated today.** Wide-net data from the previous pass is
preserved in `archive/` but is NOT part of the verified list.

## Link validation — what "valid" means here

Career sites block direct fetching from this environment, so validity is
established two ways (see the `Link_Check` column on every row):

| Link_Check value | Meaning |
|---|---|
| `list-bot-verified {TODAY}` | Row is in **today's commit** of a bot-maintained list that continuously prunes/locks closed reqs (SimplifyJobs — updated 13 min before harvest; speedyapply — 4 h). |
| `listed<=7d jobright {TODAY}` | Row was posted within the last 7 days on jobright's daily-updated new-grad list. |
| `search-verified {TODAY} (...)` | Req-ID/title re-confirmed live today via fresh web search of the official ATS ({nv} rows). |

Rows that could not be confirmed were moved to `archive/jobs_unconfirmed.csv`
({len(unverified)} rows) — usable as leads, but check before applying. {len(closed)} rows
found closed were deleted outright.

## Entry-level (資歷) guarantee

- Sources are new-grad-only curated lists, or postings whose title/level says
  new grad / university graduate / early career / entry level / Engineer I/1 /
  graduate / 校招 / 應屆 / 研替 / 預聘 / 經歷不拘.
- Removed: Senior/Staff/Lead titles, level II/III+ codes, internships,
  **PhD-only reqs** (candidate is MS), closed (🔒) postings.
- `Flags` marks visa risks: `no-sponsorship`, `us-citizenship`, `us-person-likely`
  (defense/ITAR) — deprioritize these as an international student.

## What's here

| File | Rows |
|---|---|
| `jobs_verified_entry_level.csv` | {n} |
{cv_tbl}

| Track | Openings |
|---|---|
{track_tbl}

| Tier | Openings |
|---|---|
{tier_tbl}

**Hardware-track verified openings at Big Tech / quant / TW semis: {hw_bigtech}.**

## Top verified picks (Big Tech & quant, hardware tracks)

| Company | Title | Track | Location | Link |
|---|---|---|---|---|
{pick_lines}

## Notes for a Dec-2026 CMU MS grad

- "New College Grad 2026" reqs are appliable now; the 2027-start wave
  (NVIDIA/Apple/Google/AMD/Qualcomm) opens Aug–Oct 2026 — re-run the refresh then.
- 台灣: 聯發科/台積電/瑞昱 校招・研替・預聘 cycles accept 2026 應屆 applications now;
  研發替代役 rows matter if military service is pending.
- Listings move daily. Refresh everything with:
  `python3 scripts/parse_jobs.py && python3 scripts/filter_strict.py` (see scripts/).
"""
open(f"{REPO}/README.md", "w", encoding="utf-8").write(readme)

for s in ("parse_jobs.py", "filter_strict.py", "assemble_final.py", "gen_deliverables.py", "mine_jobright_history.py"):
    if os.path.exists(f"/tmp/{s}"):
        shutil.copy(f"/tmp/{s}", f"{REPO}/scripts/{s}")

print(f"verified rows: {n} (search-verified {nv}) | unconfirmed->archive: {len(unverified)} | closed-dropped: {len(closed)}")
print("tracks:", dict(track_c))

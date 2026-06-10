#!/usr/bin/env python3
"""Strict entry-level + link-validity pass.

Verified = (a) rows in TODAY's commit of bot-pruned new-grad lists (simplify /
speedyapply / jobright<=7d), or (b) agent-sourced rows whose req-id appears in
(a), or (c) agent rows re-confirmed live by web search (merged separately).
vansh rows (list stale since Apr 25) and history-mined rows are excluded.
"""
import csv, re, sys, os

OUT = "/tmp/jobs_out"
TODAY = "2026-06-10"

def rid_of(url):
    for pat in (r"(JR\d{6,})", r"(MTK1\d{8,})", r"[_/](R\d{5,})\b", r"jobs/(\d{6,})",
                r"/job/(\d{6,})", r"details/(\d{6,})", r"gh_jid=(\d+)",
                r"jobs/info/([a-f0-9]{12,})", r"job/([a-z0-9]{5,8})$",
                r"JobDetail/[^/]+/(\d{3,})", r"careers/job/(\d{8,})",
                r"smartrecruiters\.com/[^/]+/(\d{12,})", r"greenhouse\.io/[^/]+/jobs/(\d{7,})",
                r"breezy\.hr/p/([a-f0-9]{12})", r"lever\.co/[^/]+/([a-f0-9-]{36})",
                r"1111\.com\.tw/job/(\d{6,})", r"104\.com\.tw/job/(\w{4,6})\b"):
        m = re.search(pat, url)
        if m: return m.group(1)
    return None

# ---- strict entry-level tests ----
EXPLICIT = re.compile(
    r"new ?(college )?grad|university grad|college grad|early career|entry[ -]level|"
    r"\bgraduate\b|engineer (i|1)\b|\bjunior\b|campus|rotation|\bncg\b|"
    r"新鮮人|應屆|校招|校園|研替|研發替代役|預聘|經歷不拘|經驗不拘|all levels|RDSS", re.I)
LEVEL2PLUS = re.compile(r"\b(ii|iii|iv|2|3)\b\s*$|engineer (ii|iii|iv|2|3)\b|\b[mr][2-9]\b", re.I)
PHD_ONLY = re.compile(r"phd", re.I)
HAS_MS = re.compile(r"\bmasters?\b|\bms\b", re.I)

def entry_ok(title, posted_age=""):
    t = title.strip()
    if LEVEL2PLUS.search(t) and not EXPLICIT.search(t):
        return False
    if PHD_ONLY.search(t) and not HAS_MS.search(t):
        return False  # MS candidate: PhD-only reqs don't meet the 資歷
    return bool(EXPLICIT.search(t)) or posted_age.startswith("entry:")

cols = ["Company","Title","Track","Location","Region","Tier","Salary","Flags",
        "Posted_Age","WorkModel","Source","URL","Link_Check"]

# ---- (a) current bot-pruned lists ----
base, base_rids = [], set()
for r in csv.DictReader(open(f"{OUT}/master.csv", encoding="utf-8")):
    src = r["Source"]
    if src.startswith("vansh") or "@hist" in src:
        continue
    if src.startswith("simplify") or src.startswith("speedy"):
        check = f"list-bot-verified {TODAY}"
        listed_entry = True            # new-grad-only, status-checked lists
    elif src.startswith("jobright"):
        check = f"listed<=7d jobright {TODAY}"
        listed_entry = True            # new-grad-only daily list
    else:
        continue
    # strict level scrub even for list rows
    if LEVEL2PLUS.search(r["Title"]) and not EXPLICIT.search(r["Title"]):
        continue
    if PHD_ONLY.search(r["Title"]) and not HAS_MS.search(r["Title"]):
        continue
    row = dict(r); row["Link_Check"] = check
    base.append(row)
    rid = rid_of(r["URL"])
    if rid: base_rids.add(rid)

# ---- (b)(c) agent rows: cross-ref or queue for search verification ----
confirmed, needs = [], []
for fname in ("us_verified.csv", "taiwan.csv"):
    p = f"{OUT}/{fname}"
    if not os.path.exists(p):
        continue
    for r in csv.DictReader(open(p, encoding="utf-8")):
        if not entry_ok(r["Title"], r.get("Posted_Age", "")):
            continue
        row = dict(r)
        rid = rid_of(r["URL"])
        if rid and rid in base_rids:
            row["Link_Check"] = f"list-bot-verified {TODAY} (cross-ref {rid})"
            confirmed.append(row)
        else:
            needs.append(row)

with open(f"{OUT}/verified_base.csv", "w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
    w.writeheader(); w.writerows(base + confirmed)
with open(f"{OUT}/needs_verification.csv", "w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
    w.writeheader(); w.writerows(needs)

from collections import Counter
print(f"base (today's lists, strict): {len(base)}  | agent cross-ref confirmed: {len(confirmed)}")
print(f"agent rows needing search verification: {len(needs)}  ({sum(1 for r in needs if r['Region']=='TW')} TW)")
print("base by track:", dict(Counter(r['Track'] for r in base)))

#!/usr/bin/env python3
"""Mine weekly README snapshots from jobright repos' git history for extra rows."""
import csv, re, subprocess, sys, datetime

REPOS = [
    ("/tmp/jobsrc/jobright_engineering", "jobright:ENG"),
    ("/tmp/jobsrc/jobright-ai_2026-Software-Engineer-New-Grad", "jobright:SW"),
]
CUTOFF = datetime.date(2026, 4, 1)
MONTHS = {m: i+1 for i, m in enumerate(
    ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"])}

out = csv.writer(open("/tmp/jobsrc/jobright_history_rows.csv", "w", newline="", encoding="utf-8"))
out.writerow(["company","title","url","location","workmodel","posted","source"])

seen_ids = set()
total = 0
for repo, tag in REPOS:
    subprocess.run(["git", "-C", repo, "fetch", "--unshallow", "--filter=blob:none", "origin"],
                   capture_output=True, timeout=300)
    log = subprocess.run(["git", "-C", repo, "log", "--format=%H %cs", "--", "README.md"],
                         capture_output=True, text=True).stdout.split()
    pairs = list(zip(log[0::2], log[1::2]))
    # one snapshot per ISO week
    by_week, order = {}, []
    for sha, ds in pairs:
        d = datetime.date.fromisoformat(ds)
        wk = d.isocalendar()[:2]
        if wk not in by_week:
            by_week[wk] = (sha, d); order.append(wk)
    print(f"{tag}: {len(pairs)} commits -> {len(order)} weekly snapshots", file=sys.stderr)
    for wk in order:
        sha, snap_date = by_week[wk]
        if snap_date < CUTOFF:
            continue
        blob = subprocess.run(["git", "-C", repo, "show", f"{sha}:README.md"],
                              capture_output=True, text=True).stdout
        last_company = ""
        for line in blob.splitlines():
            m = re.match(r"^\|\s*(.+?)\s*\|\s*\*\*\[(.+?)\]\((.+?)\)\*\*\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*$", line)
            if not m:
                continue
            comp_cell, title, url, loc, model, date_s = m.groups()
            cm = re.match(r"\*\*\[(.+?)\]", comp_cell)
            if cm:
                company = cm.group(1); last_company = company
            elif "↳" in comp_cell:
                company = last_company
            else:
                company = re.sub(r"<[^>]+>", "", comp_cell).strip(" *")
                if company: last_company = company
            jm = re.search(r"jobs/info/([a-f0-9]{12,})", url)
            jid = jm.group(1) if jm else url
            if jid in seen_ids:
                continue
            dm = re.match(r"([A-Z][a-z]{2}) (\d{1,2})", date_s.strip())
            if not dm or dm.group(1) not in MONTHS:
                continue
            mo, day = MONTHS[dm.group(1)], int(dm.group(2))
            year = snap_date.year if mo <= snap_date.month else snap_date.year - 1
            posted = datetime.date(year, mo, day)
            if posted < CUTOFF:
                continue
            seen_ids.add(jid)
            out.writerow([company, title, url, loc, model, posted.strftime("%b %d"), tag])
            total += 1
print(f"history rows kept: {total}")

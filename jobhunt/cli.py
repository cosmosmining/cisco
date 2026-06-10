"""jobhunt command line interface.

  python -m jobhunt fetch [--notify]        one fetch cycle: pull, filter, diff, report
  python -m jobhunt watch [--interval 3600] keep fetching forever (local live mode)
  python -m jobhunt doctor                  test every configured source
  python -m jobhunt list [--category dft]   print tracked roles from the state file
  python -m jobhunt resume score  --resume cv.pdf --job-file jd.txt [--category verification]
  python -m jobhunt resume tailor --resume cv.pdf --job-file jd.txt --out tailored.md
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import sys
import time

import yaml

from .filters import filter_jobs, categorize
from .models import Job, CATEGORIES, CATEGORY_LABELS
from .notify import notify_all
from .report import write_report
from .sources import build_sources
from .store import Store

PKG_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONFIG = os.path.join(PKG_DIR, "config.yaml")


def load_config(path: str | None) -> dict:
    with open(path or DEFAULT_CONFIG, encoding="utf-8") as f:
        return yaml.safe_load(f)


def fetch_all(cfg: dict) -> tuple[list[Job], dict[str, str]]:
    """Fetch every source in parallel; collect per-source errors."""
    sources = build_sources(cfg["companies"])
    queries = cfg.get("search_queries") or ["asic"]
    jobs: list[Job] = []
    errors: dict[str, str] = {}

    def run(src):
        try:
            found = src.fetch(queries)
            return src.company, found, None
        except Exception as e:  # noqa: BLE001
            return src.company, [], f"{type(e).__name__}: {e}"

    with cf.ThreadPoolExecutor(max_workers=8) as pool:
        for company, found, err in pool.map(run, sources):
            if err:
                errors[company] = err
                print(f"  {company:<28} FAIL  {err[:90]}")
            else:
                jobs.extend(found)
                print(f"  {company:<28} {len(found):>4} raw postings")
    return jobs, errors


def cmd_fetch(args) -> int:
    cfg = load_config(args.config)
    data_dir = cfg.get("data_dir", "jobhunt-data")
    print(f"Fetching {len(cfg['companies'])} sources...")
    raw, errors = fetch_all(cfg)

    matched = filter_jobs(raw, only_new_grad=cfg.get("only_new_grad", False))
    store = Store(os.path.join(data_dir, "seen_jobs.json"))
    new = store.diff_and_update(matched)
    store.save()
    write_report(store.all_records(), os.path.join(data_dir, "JOBS.md"), errors)

    ng = sum(1 for j in new if j.new_grad)
    print(f"\n{len(raw)} raw → {len(matched)} matched roles "
          f"({len(store.data)} tracked total) | {len(new)} NEW ({ng} 🎓)")
    print(f"Report: {os.path.join(data_dir, 'JOBS.md')}")
    if errors:
        print(f"{len(errors)} sources failed — run `python -m jobhunt doctor` for details.")

    if new:
        notify_cfg = cfg.get("notify") or {}
        to_send = [j for j in new if j.new_grad] if notify_cfg.get("only_new_grad") else new
        for j in to_send[:25]:
            flag = "🎓" if j.new_grad else "  "
            print(f"  NEW {flag} [{CATEGORY_LABELS.get(j.category, j.category)}] "
                  f"{j.company} — {j.title}\n        {j.url}")
        if args.notify and to_send:
            used = notify_all(to_send, email_to=notify_cfg.get("email_to", ""))
            print(f"Notified via: {', '.join(used) if used else 'no channels configured'}")
    return 0


def cmd_watch(args) -> int:
    interval = max(300, args.interval)
    print(f"Watching every {interval}s. Ctrl-C to stop.")
    while True:
        try:
            cmd_fetch(args)
        except KeyboardInterrupt:
            raise
        except Exception as e:  # noqa: BLE001 - keep the watcher alive
            print(f"[watch] cycle failed: {e}")
        try:
            time.sleep(interval)
        except KeyboardInterrupt:
            print("\nStopped.")
            return 0


def cmd_doctor(args) -> int:
    cfg = load_config(args.config)
    sources = build_sources(cfg["companies"])
    print(f"Testing {len(sources)} sources with query 'asic'...\n")
    ok = bad = 0

    def run(src):
        try:
            found = src.fetch(["asic"])
            return src, len(found), None
        except Exception as e:  # noqa: BLE001
            return src, 0, f"{type(e).__name__}: {e}"

    with cf.ThreadPoolExecutor(max_workers=8) as pool:
        for src, n, err in pool.map(run, sources):
            if err:
                bad += 1
                print(f"  ✗ {src.company:<28} {err[:100]}")
            else:
                ok += 1
                print(f"  ✓ {src.company:<28} {n} postings for 'asic'")
    print(f"\n{ok} OK, {bad} failing.")
    if bad:
        print("Fix failing Workday entries by opening the company careers page and copying\n"
              "the host/tenant/site from the '/wday/cxs/<tenant>/<site>/jobs' request in\n"
              "the browser network tab, then update jobhunt/config.yaml.")
    return 0 if bad == 0 else 1


def cmd_list(args) -> int:
    cfg = load_config(args.config)
    store = Store(os.path.join(cfg.get("data_dir", "jobhunt-data"), "seen_jobs.json"))
    records = store.all_records()
    if args.category:
        records = [r for r in records if r.get("category") == args.category]
    if args.new_grad:
        records = [r for r in records if r.get("new_grad")]
    for r in records:
        flag = "🎓" if r.get("new_grad") else "  "
        print(f"{flag} [{r.get('category','?'):<15}] {r.get('company','?'):<22} "
              f"{r.get('title','?')}\n     {r.get('url','')}")
    print(f"\n{len(records)} roles. Run `python -m jobhunt fetch` to refresh.")
    return 0


def _load_jd(args) -> str:
    from .resume.parser import read_text
    if args.job_file:
        return read_text(args.job_file)
    if args.job_text:
        return args.job_text
    if args.job_url:
        import requests
        from .sources.greenhouse import strip_html
        r = requests.get(args.job_url, timeout=30,
                         headers={"User-Agent": "Mozilla/5.0"})
        r.raise_for_status()
        text = strip_html(r.text)
        if len(text.split()) < 80:
            raise SystemExit("Could not extract a useful JD from that URL "
                             "(JS-rendered page?). Paste it into a file and use --job-file.")
        return text
    raise SystemExit("Provide the job description via --job-file, --job-url or --job-text.")


def _resume_common(args) -> tuple[str, str, str]:
    from .resume.parser import read_text
    resume_text = read_text(args.resume)
    jd_text = _load_jd(args)
    category = args.category or categorize(jd_text[:200], jd_text) or "asic_design"
    return resume_text, jd_text, category


def cmd_resume_score(args) -> int:
    from .resume.analyzer import score
    resume_text, jd_text, category = _resume_common(args)
    print(score(resume_text, jd_text, category).render())
    return 0


def cmd_resume_tailor(args) -> int:
    from .resume.tailor import tailor
    resume_text, jd_text, category = _resume_common(args)
    out = tailor(resume_text, jd_text, category)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(out)
        print(f"\nSaved tailored resume to {args.out}")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="jobhunt", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--config", help="path to config.yaml (default: bundled)")
    sub = p.add_subparsers(dest="cmd", required=True)

    f = sub.add_parser("fetch", help="fetch all boards once, update report, list new roles")
    f.add_argument("--notify", action="store_true", help="send notifications for new roles")
    f.set_defaults(func=cmd_fetch)

    w = sub.add_parser("watch", help="fetch on a loop (live local mode)")
    w.add_argument("--interval", type=int, default=3600, help="seconds between cycles (min 300)")
    w.add_argument("--notify", action="store_true")
    w.set_defaults(func=cmd_watch)

    d = sub.add_parser("doctor", help="test every configured source")
    d.set_defaults(func=cmd_doctor)

    l = sub.add_parser("list", help="print tracked roles")
    l.add_argument("--category", choices=CATEGORIES)
    l.add_argument("--new-grad", action="store_true", help="only 🎓-tagged roles")
    l.set_defaults(func=cmd_list)

    r = sub.add_parser("resume", help="resume scoring / tailoring")
    rsub = r.add_subparsers(dest="rcmd", required=True)
    for name, fn, hlp in (
        ("score", cmd_resume_score, "offline keyword/ATS score against a JD"),
        ("tailor", cmd_resume_tailor, "rewrite the resume for a JD (Claude API)"),
    ):
        rp = rsub.add_parser(name, help=hlp)
        rp.add_argument("--resume", required=True, help=".pdf/.docx/.md/.txt resume")
        rp.add_argument("--job-file", help="file containing the job description")
        rp.add_argument("--job-url", help="URL of the posting (best effort)")
        rp.add_argument("--job-text", help="job description pasted inline")
        rp.add_argument("--category", choices=CATEGORIES,
                        help="role category (default: auto-detect from JD)")
        if name == "tailor":
            rp.add_argument("--out", help="write tailored resume markdown here")
        rp.set_defaults(func=fn)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

"""Render JOBS.md — every tracked role grouped by category, with links."""
from __future__ import annotations

import os
from datetime import datetime, timezone

from .models import CATEGORIES, CATEGORY_LABELS


def _esc(text: str) -> str:
    return (text or "").replace("|", "\\|").strip()


def write_report(records: list[dict], path: str, errors: dict[str, str] | None = None):
    by_cat: dict[str, list[dict]] = {c: [] for c in CATEGORIES}
    for r in records:
        by_cat.setdefault(r.get("category", "asic_design"), []).append(r)

    lines = [
        "# Silicon / Firmware New-Grad Job Board",
        "",
        f"_Updated {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} — "
        f"{len(records)} tracked roles. 🎓 = explicit new-grad / early-career signal._",
        "",
    ]

    # summary table
    lines += ["| Category | Roles | 🎓 New-grad tagged |", "|---|---|---|"]
    for cat in CATEGORIES:
        rows = by_cat.get(cat, [])
        ng = sum(1 for r in rows if r.get("new_grad"))
        lines.append(f"| {CATEGORY_LABELS[cat]} | {len(rows)} | {ng} |")
    lines.append("")

    for cat in CATEGORIES:
        rows = by_cat.get(cat, [])
        if not rows:
            continue
        lines += [f"## {CATEGORY_LABELS[cat]} ({len(rows)})", ""]
        lines += ["| | Company | Role | Location | Posted |", "|---|---|---|---|---|"]
        rows.sort(key=lambda r: (not r.get("new_grad"), r.get("company", ""), r.get("title", "")))
        for r in rows:
            flag = "🎓" if r.get("new_grad") else ""
            title = _esc(r.get("title"))
            url = r.get("url", "")
            lines.append(
                f"| {flag} | {_esc(r.get('company'))} | [{title}]({url}) "
                f"| {_esc(r.get('location'))[:60]} | {_esc(r.get('posted'))[:24]} |"
            )
        lines.append("")

    if errors:
        lines += ["## Source errors (fix via `python -m jobhunt doctor`)", ""]
        for name, err in sorted(errors.items()):
            lines.append(f"- **{name}**: `{err[:160]}`")
        lines.append("")

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

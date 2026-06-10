"""Categorisation and new-grad filtering of job postings.

All matching is regex-on-lowercased-text. Category patterns are checked in
priority order (most specific first) so "ASIC Physical Design Engineer" lands
in physical_design rather than asic_design.
"""
from __future__ import annotations

import re
from .models import Job

# ---------------------------------------------------------------- categories
# Priority order matters: first category whose pattern hits the title wins.
CATEGORY_PATTERNS: dict[str, list[str]] = {
    "dft": [
        r"\bdft\b", r"design[- ]for[- ]test", r"\bmbist\b", r"\batpg\b",
        r"scan (?:insertion|architect)", r"\bsilicon test\b", r"\btest engineer.*(asic|soc|silicon)",
    ],
    "physical_design": [
        r"physical design", r"place[- ](?:and|&)[- ]route", r"\bpnr\b", r"p&r",
        r"timing closure", r"static timing", r"\bsta engineer", r"physical implementation",
        r"floor[- ]?plan", r"chip implementation", r"physical verification", r"\bsignoff\b",
        r"\bcad engineer.*(physical|timing|pnr)", r"mask design", r"\blayout (?:design )?engineer",
    ],
    "verification": [
        r"verification", r"\bdv engineer", r"\bdv\b", r"\buvm\b", r"\bemulation\b",
        r"post[- ]silicon", r"silicon validation", r"\bvalidation engineer",
        r"\bformal (?:verification|methods)",
    ],
    "firmware": [
        r"firmware", r"embedded software", r"embedded systems? (?:software )?engineer",
        r"\brtos\b", r"bare[- ]metal", r"\bbootloader\b", r"device driver",
        r"\bbsp engineer", r"microcontroller",
    ],
    "asic_design": [
        r"\basic\b", r"\brtl\b", r"digital design", r"\bsoc design", r"logic design",
        r"\bcpu (?:design|core)", r"\bgpu (?:design|asic|hardware)", r"micro[- ]?architect",
        r"digital ic design", r"silicon design", r"\bip design", r"\bsilicon engineer",
        r"hardware design(?:er)? engineer", r"\bdesign engineer.*(silicon|soc|chip|asic)",
        r"\bsoc\b.*engineer", r"\bvlsi\b",
    ],
}

# ------------------------------------------------------------ level signals
NEW_GRAD_PATTERNS = [
    r"new (?:college )?grad", r"\bncg\b", r"university grad", r"college grad",
    r"recent(?:ly)? graduat", r"new graduate", r"campus", r"early career",
    r"early[- ]in[- ]career", r"entry[- ]level", r"early talent", r"graduate engineer",
    r"graduate program", r"\b(?:20(?:2[5-9]))\s*(?:grad|start)", r"phd graduate",
    r"masters? graduate", r"\bgrad\b", r"\bbs/ms\b", r"0\s*[-–+]\s*[0-3]?\s*years?",
    r"\bengineer\s*(?:i|1)\b", r"\bjunior\b",
]

# Hard exclusions, checked on the title only.
EXCLUDE_TITLE_PATTERNS = [
    r"\bintern(?:ship)?\b", r"\bco[- ]?op\b", r"\bsenior\b", r"\bsr\.?\b",
    r"\bstaff\b", r"\bprincipal\b", r"\bdistinguished\b", r"\bfellow\b",
    r"\bdirector\b", r"\bmanager\b", r"\bmgr\b", r"\bhead of\b", r"\blead\b",
    r"\bexperienced\b", r"\bexpert\b", r"\b(?:[4-9]|1[0-9])\+\s*(?:years|yrs)\b",
    r"\bvp\b", r"\bvice president\b", r"\barchitect\b",
]

# If the description states a minimum years-of-experience >= this, drop it.
MAX_MIN_YEARS = 4
_YEARS_RE = re.compile(r"(\d{1,2})\s*\+?\s*(?:or more\s*)?(?:years|yrs)", re.I)


def _compile(patterns: list[str]) -> re.Pattern:
    return re.compile("|".join(f"(?:{p})" for p in patterns), re.I)


_CATEGORY_RES = {cat: _compile(pats) for cat, pats in CATEGORY_PATTERNS.items()}
_NEW_GRAD_RE = _compile(NEW_GRAD_PATTERNS)
_EXCLUDE_RE = _compile(EXCLUDE_TITLE_PATTERNS)


def categorize(title: str, description: str = "") -> str | None:
    """Return the first matching category for a posting, or None."""
    for cat, rx in _CATEGORY_RES.items():
        if rx.search(title):
            return cat
    # fall back to the description only for strong, unambiguous signals
    desc = description[:4000]
    if desc:
        for cat in ("dft", "physical_design", "verification"):
            if _CATEGORY_RES[cat].search(desc) and _CATEGORY_RES["asic_design"].search(title + " " + desc):
                return cat
    return None


def is_excluded(title: str, description: str = "") -> bool:
    if _EXCLUDE_RE.search(title):
        return True
    if description:
        years = [int(m.group(1)) for m in _YEARS_RE.finditer(description[:6000])]
        years = [y for y in years if y <= 30]
        if years and min(years) >= MAX_MIN_YEARS:
            return True
    return False


def has_new_grad_signal(title: str, description: str = "") -> bool:
    return bool(_NEW_GRAD_RE.search(title) or (description and _NEW_GRAD_RE.search(description[:6000])))


def filter_jobs(jobs: list[Job], only_new_grad: bool = False) -> list[Job]:
    """Categorise, drop excluded/senior roles, tag new-grad signals, dedupe."""
    out: dict[str, Job] = {}
    for job in jobs:
        cat = categorize(job.title, job.description)
        if not cat:
            continue
        if is_excluded(job.title, job.description):
            continue
        job.category = cat
        job.new_grad = has_new_grad_signal(job.title, job.description)
        if only_new_grad and not job.new_grad:
            continue
        out.setdefault(job.uid, job)
    return list(out.values())

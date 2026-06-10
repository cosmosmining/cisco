"""ATS-style resume scoring against a job description.

Matches the curated silicon-skills taxonomy plus JD-specific acronyms, and
runs basic ATS hygiene checks. Pure offline — no API key needed.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

from .skills import skills_for

_ACRONYM_RE = re.compile(r"\b[A-Z][A-Z0-9]{1,5}\b")
_ACRONYM_NOISE = {
    "AND", "THE", "FOR", "YOU", "OUR", "ARE", "NOT", "ALL", "NEW", "USA", "US",
    "EEO", "LLC", "INC", "PTO", "401K", "BS", "MS", "PHD", "BSEE", "MSEE", "GPA",
    "ID", "EU", "UK", "CA", "TX", "WA", "NY", "AZ", "OR", "II", "III", "IV",
    "TO", "OF", "IN", "ON", "AT", "BE", "WE", "AN", "IT", "BY", "AS", "OK",
}

SECTIONS = {
    "education": r"\beducation\b",
    "experience": r"\b(?:experience|employment|work history)\b",
    "skills": r"\b(?:skills|technical skills|technologies)\b",
    "projects": r"\bprojects?\b",
}


@dataclass
class ScoreResult:
    category: str
    coverage: float                      # 0..1 over JD-required taxonomy skills
    matched: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    extra: list[str] = field(default_factory=list)        # in resume, not in JD
    jd_acronyms_missing: list[str] = field(default_factory=list)
    ats_warnings: list[str] = field(default_factory=list)

    def render(self) -> str:
        bar_len = 30
        filled = int(round(self.coverage * bar_len))
        bar = "#" * filled + "-" * (bar_len - filled)
        lines = [
            f"Keyword coverage vs JD ({self.category}): "
            f"[{bar}] {self.coverage * 100:.0f}%",
            "",
            f"✅ Matched skills the JD asks for ({len(self.matched)}):",
            "   " + (", ".join(self.matched) or "(none)"),
            "",
            f"❌ JD skills MISSING from your resume ({len(self.missing)}) — "
            "add the ones you actually have:",
            "   " + (", ".join(self.missing) or "(none)"),
        ]
        if self.jd_acronyms_missing:
            lines += ["", "🔎 Other JD terms not on your resume (check relevance): "
                      + ", ".join(self.jd_acronyms_missing[:20])]
        if self.extra:
            lines += ["", "ℹ️  On your resume but not in this JD (fine to keep, "
                      "consider demoting): " + ", ".join(self.extra[:15])]
        if self.ats_warnings:
            lines += ["", "⚠️  ATS checks:"] + [f"   - {w}" for w in self.ats_warnings]
        return "\n".join(lines)


def _find_skills(text: str, category: str) -> set[str]:
    return {name for name, rx in skills_for(category) if rx.search(text)}


def ats_checks(resume_text: str) -> list[str]:
    warns = []
    if not re.search(r"[\w.+-]+@[\w-]+\.[\w.]+", resume_text):
        warns.append("No email address found.")
    if not re.search(r"(?:\+?\d[\d ()./-]{8,}\d)", resume_text):
        warns.append("No phone number found.")
    if not re.search(r"(?:linkedin\.com|github\.com)", resume_text, re.I):
        warns.append("No LinkedIn/GitHub link — recruiters for NG roles expect one.")
    words = len(resume_text.split())
    if words > 900:
        warns.append(f"~{words} words — likely over one page; new-grad resumes should be one page.")
    if words < 150:
        warns.append(f"Only ~{words} words extracted — content may be too thin or extraction failed.")
    for section, rx in SECTIONS.items():
        if not re.search(rx, resume_text, re.I):
            warns.append(f"No '{section}' section header detected.")
    if re.search(r"\t{2,}", resume_text):
        warns.append("Multiple consecutive tabs — possible table layout; ATS parsers mangle tables.")
    if not re.search(r"(?:gpa|/ ?4\.0)", resume_text, re.I):
        warns.append("No GPA found — include it if ≥ 3.0 (many NG screens filter on it).")
    return warns


def score(resume_text: str, jd_text: str, category: str) -> ScoreResult:
    resume_skills = _find_skills(resume_text, category)
    jd_skills = _find_skills(jd_text, category)

    matched = sorted(resume_skills & jd_skills)
    missing = sorted(jd_skills - resume_skills)
    extra = sorted(resume_skills - jd_skills)
    coverage = (len(matched) / len(jd_skills)) if jd_skills else 0.0

    jd_acros = {a for a in _ACRONYM_RE.findall(jd_text)} - _ACRONYM_NOISE
    resume_upper = resume_text.upper()
    acros_missing = sorted(a for a in jd_acros if a not in resume_upper)

    return ScoreResult(
        category=category,
        coverage=coverage,
        matched=matched,
        missing=missing,
        extra=extra,
        jd_acronyms_missing=acros_missing,
        ats_warnings=ats_checks(resume_text),
    )

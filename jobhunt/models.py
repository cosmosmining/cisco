"""Core data model for job postings."""
from __future__ import annotations

import hashlib
from dataclasses import dataclass, field, asdict

CATEGORIES = ["dft", "physical_design", "verification", "firmware", "asic_design"]

CATEGORY_LABELS = {
    "asic_design": "ASIC / RTL Design",
    "verification": "Design Verification",
    "dft": "DFT (Design for Test)",
    "physical_design": "Physical Design",
    "firmware": "Firmware / Embedded",
}


@dataclass
class Job:
    company: str
    title: str
    url: str
    location: str = ""
    source: str = ""          # adapter that produced it (workday, greenhouse, ...)
    category: str = ""        # one of CATEGORIES
    posted: str = ""          # free-form date string from the board
    description: str = ""     # only populated when the list API returns it cheaply
    new_grad: bool = False    # explicit new-grad/early-career signal found

    @property
    def uid(self) -> str:
        raw = f"{self.company}|{self.url}".encode("utf-8", "replace")
        return hashlib.sha1(raw).hexdigest()[:16]

    def to_record(self) -> dict:
        d = asdict(self)
        d.pop("description", None)  # keep the state file small
        return d

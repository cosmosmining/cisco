"""Lever postings adapter — Zoox, PsiQuantum, etc.

GET https://api.lever.co/v0/postings/{org}?mode=json
Returns the whole board; queries ignored, filtering happens locally.
"""
from __future__ import annotations

from .base import Source
from .greenhouse import strip_html
from ..models import Job


class LeverSource(Source):
    type_name = "lever"

    def fetch(self, queries: list[str]) -> list[Job]:
        org = self.opts["org"]
        url = f"https://api.lever.co/v0/postings/{org}?mode=json"
        data = self.get_json(url)
        out = []
        for j in data if isinstance(data, list) else []:
            cats = j.get("categories") or {}
            out.append(Job(
                company=self.company,
                title=(j.get("text") or "").strip(),
                url=j.get("hostedUrl") or "",
                location=cats.get("location") or "",
                description=strip_html(j.get("descriptionPlain") or j.get("description") or "")[:8000],
                source=self.type_name,
            ))
        return out

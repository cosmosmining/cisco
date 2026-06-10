"""Greenhouse public boards adapter — SiFive, Cerebras, Groq, SpaceX, Anduril...

GET https://boards-api.greenhouse.io/v1/boards/{token}/jobs?content=true
Returns the whole board, so queries are ignored and filtering happens locally.
`content=true` includes the HTML job description (useful for new-grad and
years-of-experience checks, and for resume scoring).
"""
from __future__ import annotations

import html
import re

from .base import Source
from ..models import Job

_TAG_RE = re.compile(r"<[^>]+>")


def strip_html(text: str) -> str:
    return html.unescape(_TAG_RE.sub(" ", text or ""))


class GreenhouseSource(Source):
    type_name = "greenhouse"

    def fetch(self, queries: list[str]) -> list[Job]:
        token = self.opts["board"]
        url = f"https://boards-api.greenhouse.io/v1/boards/{token}/jobs?content=true"
        data = self.get_json(url)
        out = []
        for j in data.get("jobs", []):
            out.append(Job(
                company=self.company,
                title=(j.get("title") or "").strip(),
                url=j.get("absolute_url") or "",
                location=((j.get("location") or {}).get("name")) or "",
                posted=(j.get("updated_at") or "")[:10],
                description=strip_html(j.get("content") or "")[:8000],
                source=self.type_name,
            ))
        return out

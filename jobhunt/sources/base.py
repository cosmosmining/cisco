"""Shared HTTP plumbing for job-board adapters."""
from __future__ import annotations

import requests

from ..models import Job

UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")
TIMEOUT = 25


def session() -> requests.Session:
    s = requests.Session()
    s.headers.update({
        "User-Agent": UA,
        "Accept": "application/json",
        "Accept-Language": "en-US,en;q=0.9",
    })
    return s


class Source:
    """One company on one ATS. Subclasses implement fetch()."""

    type_name = "base"

    def __init__(self, company: str, **kwargs):
        self.company = company
        self.opts = kwargs
        self.http = session()

    def __repr__(self):
        return f"<{self.type_name}:{self.company}>"

    def fetch(self, queries: list[str]) -> list[Job]:
        """Return raw (unfiltered) postings. May raise; caller handles."""
        raise NotImplementedError

    # -- helpers -------------------------------------------------------
    def get_json(self, url: str, **kw) -> dict | list:
        r = self.http.get(url, timeout=TIMEOUT, **kw)
        r.raise_for_status()
        return r.json()

    def post_json(self, url: str, payload: dict, **kw) -> dict:
        r = self.http.post(url, json=payload, timeout=TIMEOUT, **kw)
        r.raise_for_status()
        return r.json()

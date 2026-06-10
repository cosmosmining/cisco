"""Seen-jobs state: JSON file diffed on every fetch to find new postings."""
from __future__ import annotations

import json
import os
from datetime import datetime, timezone

from .models import Job


class Store:
    def __init__(self, path: str):
        self.path = path
        self.data: dict[str, dict] = {}
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                self.data = json.load(f)

    def diff_and_update(self, jobs: list[Job]) -> list[Job]:
        """Record all jobs; return only those never seen before."""
        now = datetime.now(timezone.utc).isoformat(timespec="seconds")
        new = []
        for job in jobs:
            if job.uid not in self.data:
                rec = job.to_record()
                rec["first_seen"] = now
                self.data[job.uid] = rec
                new.append(job)
            else:
                self.data[job.uid]["last_seen"] = now
        return new

    def save(self):
        os.makedirs(os.path.dirname(self.path) or ".", exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as f:
            json.dump(self.data, f, indent=1, sort_keys=True)

    def all_records(self) -> list[dict]:
        return sorted(self.data.values(),
                      key=lambda r: (r.get("category", ""), r.get("company", ""), r.get("title", "")))

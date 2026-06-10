"""Workday CXS adapter — covers NVIDIA, AMD, Broadcom, Micron, Marvell, etc.

Endpoint shape:
  POST https://{host}/wday/cxs/{tenant}/{site}/jobs
  body: {"appliedFacets": {}, "limit": 20, "offset": 0, "searchText": "asic"}

Config options: host, tenant, site.
"""
from __future__ import annotations

from .base import Source
from ..models import Job

PAGE = 20


class WorkdaySource(Source):
    type_name = "workday"

    def fetch(self, queries: list[str]) -> list[Job]:
        host = self.opts["host"]
        tenant = self.opts["tenant"]
        site = self.opts["site"]
        api = f"https://{host}/wday/cxs/{tenant}/{site}/jobs"
        max_pages = int(self.opts.get("max_pages", 2))

        jobs: dict[str, Job] = {}
        for q in queries:
            for page in range(max_pages):
                payload = {
                    "appliedFacets": {},
                    "limit": PAGE,
                    "offset": page * PAGE,
                    "searchText": q,
                }
                data = self.post_json(api, payload)
                postings = data.get("jobPostings") or []
                for p in postings:
                    path = p.get("externalPath") or ""
                    if not path:
                        continue
                    url = f"https://{host}/en-US/{site}{path}"
                    job = Job(
                        company=self.company,
                        title=(p.get("title") or "").strip(),
                        url=url,
                        location=p.get("locationsText") or "",
                        posted=p.get("postedOn") or "",
                        source=self.type_name,
                    )
                    jobs[job.uid] = job
                if len(postings) < PAGE:
                    break
        return list(jobs.values())

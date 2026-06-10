"""Eightfold AI adapter — Qualcomm, Western Digital, TI (and other
careers.{company}.com sites powered by Eightfold).

GET https://{careers_host}/api/apply/v2/jobs?domain={domain}&query={q}&start=0&num=20
"""
from __future__ import annotations

from .base import Source
from .greenhouse import strip_html
from ..models import Job

PAGE = 20


class EightfoldSource(Source):
    type_name = "eightfold"

    def fetch(self, queries: list[str]) -> list[Job]:
        host = self.opts["host"]            # e.g. careers.qualcomm.com
        domain = self.opts["domain"]        # e.g. qualcomm.com
        max_pages = int(self.opts.get("max_pages", 2))

        jobs: dict[str, Job] = {}
        for q in queries:
            for page in range(max_pages):
                url = (f"https://{host}/api/apply/v2/jobs?domain={domain}"
                       f"&query={q}&start={page * PAGE}&num={PAGE}")
                data = self.get_json(url)
                positions = data.get("positions") or []
                for p in positions:
                    job_url = p.get("canonicalPositionUrl") or ""
                    if not job_url:
                        pid = p.get("id")
                        job_url = f"https://{host}/careers?pid={pid}"
                    loc = p.get("location") or ""
                    if isinstance(loc, list):
                        loc = "; ".join(loc[:3])
                    job = Job(
                        company=self.company,
                        title=(p.get("name") or "").strip(),
                        url=job_url,
                        location=loc,
                        posted=str(p.get("t_create") or ""),
                        description=strip_html(p.get("job_description") or "")[:8000],
                        source=self.type_name,
                    )
                    jobs[job.uid] = job
                if len(positions) < PAGE:
                    break
        return list(jobs.values())

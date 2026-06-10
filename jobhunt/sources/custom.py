"""Custom adapters for big-tech boards with bespoke APIs:
Amazon, Google, Microsoft, Tesla, Apple.

These endpoints are unofficial and may change; every adapter is defensive and
failures are reported per-source by `doctor` / the fetch summary rather than
crashing the run.
"""
from __future__ import annotations

from .base import Source
from .greenhouse import strip_html
from ..models import Job

PAGE = 50


class AmazonSource(Source):
    """amazon.jobs public search JSON (includes Annapurna Labs silicon roles)."""

    type_name = "amazon"

    def fetch(self, queries: list[str]) -> list[Job]:
        jobs: dict[str, Job] = {}
        for q in queries:
            url = (f"https://www.amazon.jobs/en/search.json?base_query={q}"
                   f"&result_limit={PAGE}&offset=0&sort=recent")
            data = self.get_json(url)
            for j in data.get("jobs", []):
                path = j.get("job_path") or ""
                desc = " ".join(filter(None, [
                    j.get("description_short"), j.get("basic_qualifications"),
                ]))
                job = Job(
                    company=self.company,
                    title=(j.get("title") or "").strip(),
                    url=f"https://www.amazon.jobs{path}",
                    location=j.get("normalized_location") or j.get("location") or "",
                    posted=j.get("posted_date") or "",
                    description=strip_html(desc)[:8000],
                    source=self.type_name,
                )
                jobs[job.uid] = job
        return list(jobs.values())


class GoogleSource(Source):
    """careers.google.com v3 search API."""

    type_name = "google"

    def fetch(self, queries: list[str]) -> list[Job]:
        jobs: dict[str, Job] = {}
        for q in queries:
            url = f"https://careers.google.com/api/v3/search/?q={q}&page=1"
            data = self.get_json(url)
            for j in data.get("jobs", []):
                jid = (j.get("id") or "").split("/")[-1]
                if not jid:
                    continue
                locs = "; ".join(
                    (l.get("display") or "") for l in (j.get("locations") or [])[:3]
                )
                job = Job(
                    company=self.company,
                    title=(j.get("title") or "").strip(),
                    url=("https://www.google.com/about/careers/applications/"
                         f"jobs/results/{jid}"),
                    location=locs,
                    posted=(j.get("created") or "")[:10],
                    description=strip_html(" ".join(filter(None, [
                        j.get("description"),
                        " ".join(j.get("qualifications") or [])
                        if isinstance(j.get("qualifications"), list)
                        else j.get("qualifications") or "",
                    ])))[:8000],
                    source=self.type_name,
                )
                jobs[job.uid] = job
        return list(jobs.values())


class MicrosoftSource(Source):
    """gcsservices.careers.microsoft.com search API."""

    type_name = "microsoft"

    def fetch(self, queries: list[str]) -> list[Job]:
        jobs: dict[str, Job] = {}
        for q in queries:
            url = (f"https://gcsservices.careers.microsoft.com/search/api/v1/search"
                   f"?q={q}&l=en_us&pg=1&pgSz={PAGE}&o=Relevance&flt=true")
            data = self.get_json(url)
            result = ((data.get("operationResult") or {}).get("result") or {})
            for j in result.get("jobs", []):
                jid = j.get("jobId") or ""
                props = j.get("properties") or {}
                locs = props.get("locations") or []
                job = Job(
                    company=self.company,
                    title=(j.get("title") or "").strip(),
                    url=f"https://jobs.careers.microsoft.com/global/en/job/{jid}/",
                    location="; ".join(locs[:3]) if isinstance(locs, list) else str(locs),
                    posted=(j.get("postingDate") or "")[:10],
                    description=strip_html(props.get("description") or "")[:8000],
                    source=self.type_name,
                )
                jobs[job.uid] = job
        return list(jobs.values())


class TeslaSource(Source):
    """tesla.com careers state dump (all listings in one call)."""

    type_name = "tesla"

    def fetch(self, queries: list[str]) -> list[Job]:
        data = self.get_json("https://www.tesla.com/cua-api/apps/careers/state")
        listings = (data.get("listings") or
                    (data.get("geo") or {}).get("listings") or [])
        lookup = data.get("lookup") or {}
        departments = lookup.get("departments") or {}
        locations = lookup.get("locations") or {}

        out = []
        for j in listings:
            jid = j.get("id")
            title = (j.get("t") or j.get("title") or "").strip()
            if not jid or not title:
                continue
            loc_key = str(j.get("l") or j.get("location") or "")
            loc = locations.get(loc_key, loc_key) if isinstance(locations, dict) else loc_key
            dep_key = str(j.get("dp") or "")
            dep = departments.get(dep_key, "") if isinstance(departments, dict) else ""
            out.append(Job(
                company=self.company,
                title=title,
                url=f"https://www.tesla.com/careers/search/job/{jid}",
                location=str(loc),
                description=str(dep),
                source=self.type_name,
            ))
        return out


class AppleSource(Source):
    """jobs.apple.com search API (needs a cookie warm-up GET first)."""

    type_name = "apple"

    def fetch(self, queries: list[str]) -> list[Job]:
        # Warm up cookies + CSRF token; Apple rejects bare POSTs.
        warm = self.http.get("https://jobs.apple.com/en-us/search",
                             timeout=25)
        csrf = warm.headers.get("X-Apple-CSRF-Token") or ""
        headers = {"Content-Type": "application/json"}
        if csrf:
            headers["X-Apple-CSRF-Token"] = csrf

        jobs: dict[str, Job] = {}
        for q in queries:
            payload = {"query": q, "filters": {"postingpostLocation": []},
                       "page": 1, "locale": "en-us", "sort": "newest"}
            data = self.post_json("https://jobs.apple.com/api/role/search",
                                  payload, headers=headers)
            results = (data.get("searchResults")
                       or (data.get("res") or {}).get("searchResults") or [])
            for j in results:
                pid = j.get("positionId") or j.get("id") or ""
                slug = (j.get("transformedPostingTitle")
                        or (j.get("postingTitle") or "").lower().replace(" ", "-"))
                if not pid:
                    continue
                job = Job(
                    company=self.company,
                    title=(j.get("postingTitle") or "").strip(),
                    url=f"https://jobs.apple.com/en-us/details/{pid}/{slug}",
                    location=((j.get("locations") or [{}])[0].get("name") or ""),
                    posted=(j.get("postingDate") or "")[:10],
                    source=self.type_name,
                )
                jobs[job.uid] = job
        return list(jobs.values())

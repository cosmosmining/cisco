"""Notification fan-out for newly discovered jobs.

Channels activate based on environment variables (set them locally or as
GitHub Actions secrets):

  NTFY_TOPIC            push to https://ntfy.sh/<topic>  (phone push, no signup)
  NTFY_SERVER           optional, defaults to https://ntfy.sh
  DISCORD_WEBHOOK_URL   Discord channel webhook
  SLACK_WEBHOOK_URL     Slack incoming webhook
  SMTP_HOST/SMTP_PORT/SMTP_USER/SMTP_PASS + EMAIL_TO   email (e.g. Gmail app password)
"""
from __future__ import annotations

import os
import smtplib
import textwrap
from email.mime.text import MIMEText

import requests

from .models import Job, CATEGORY_LABELS


def format_lines(jobs: list[Job], limit: int = 40) -> str:
    lines = []
    for j in jobs[:limit]:
        flag = "🎓 " if j.new_grad else ""
        cat = CATEGORY_LABELS.get(j.category, j.category)
        lines.append(f"{flag}[{cat}] {j.company} — {j.title}\n{j.url}")
    if len(jobs) > limit:
        lines.append(f"...and {len(jobs) - limit} more (see JOBS.md)")
    return "\n\n".join(lines)


def notify_all(jobs: list[Job], email_to: str = "") -> list[str]:
    """Send to every configured channel; returns list of channels used."""
    if not jobs:
        return []
    ng = sum(1 for j in jobs if j.new_grad)
    title = f"{len(jobs)} new silicon/firmware roles ({ng} new-grad tagged)"
    body = format_lines(jobs)
    used = []

    topic = os.environ.get("NTFY_TOPIC")
    if topic:
        server = os.environ.get("NTFY_SERVER", "https://ntfy.sh").rstrip("/")
        try:
            requests.post(f"{server}/{topic}", data=body.encode("utf-8"),
                          headers={"Title": title.encode("latin-1", "replace"),
                                   "Tags": "briefcase", "Priority": "default"},
                          timeout=20).raise_for_status()
            used.append("ntfy")
        except Exception as e:  # noqa: BLE001 - notification failures must not kill the run
            print(f"[notify] ntfy failed: {e}")

    discord = os.environ.get("DISCORD_WEBHOOK_URL")
    if discord:
        try:
            for chunk in textwrap.wrap(f"**{title}**\n\n{body}", 1900,
                                       replace_whitespace=False, drop_whitespace=False):
                requests.post(discord, json={"content": chunk}, timeout=20).raise_for_status()
            used.append("discord")
        except Exception as e:  # noqa: BLE001
            print(f"[notify] discord failed: {e}")

    slack = os.environ.get("SLACK_WEBHOOK_URL")
    if slack:
        try:
            requests.post(slack, json={"text": f"*{title}*\n\n{body[:38000]}"},
                          timeout=20).raise_for_status()
            used.append("slack")
        except Exception as e:  # noqa: BLE001
            print(f"[notify] slack failed: {e}")

    host = os.environ.get("SMTP_HOST")
    to = os.environ.get("EMAIL_TO") or email_to
    if host and to:
        try:
            msg = MIMEText(body, _charset="utf-8")
            msg["Subject"] = title
            msg["From"] = os.environ.get("SMTP_USER", "jobhunt")
            msg["To"] = to
            port = int(os.environ.get("SMTP_PORT", "587"))
            with smtplib.SMTP(host, port, timeout=30) as s:
                s.starttls()
                user, pw = os.environ.get("SMTP_USER"), os.environ.get("SMTP_PASS")
                if user and pw:
                    s.login(user, pw)
                s.sendmail(msg["From"], [to], msg.as_string())
            used.append(f"email→{to}")
        except Exception as e:  # noqa: BLE001
            print(f"[notify] email failed: {e}")

    return used

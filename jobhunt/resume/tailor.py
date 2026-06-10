"""Claude-powered resume tailoring: rewrite a resume toward a specific JD.

Requires ANTHROPIC_API_KEY. Never fabricates experience — anything inferred is
flagged [VERIFY] for the user to confirm or delete.
"""
from __future__ import annotations

import os

from .analyzer import score

SYSTEM = """\
You are an expert resume writer for the semiconductor industry (ASIC design, \
design verification, DFT, physical design, embedded firmware), specializing in \
new-grad and early-career candidates.

Hard rules:
- NEVER invent experience, tools, projects, metrics, or credentials that are \
not in the source resume. If you reasonably infer something (e.g. a tool \
implied by a described flow), mark it inline with [VERIFY].
- Keep the result to one page worth of content.
- Bullets: strong action verb + what was built + tool/technology + measurable \
outcome where the source provides one. Mirror the job description's exact \
terminology (e.g. say "UVM testbench" not "verification environment" when the \
JD says UVM) wherever it is truthful.
- Order sections and skills so the most JD-relevant items appear first.
- Plain Markdown output, ATS-safe: no tables, no images, no columns."""

PROMPT = """\
Tailor the resume below to the target job description.

<job_description>
{jd}
</job_description>

<resume>
{resume}
</resume>

Keyword-gap analysis (offline scorer output, for your reference):
<analysis>
{analysis}
</analysis>

Produce exactly these sections:

## Tailored Resume
The complete rewritten resume in Markdown.

## Changes & Rationale
Bulleted list of every substantive change and why it helps for THIS role.

## Gaps You Cannot Paper Over
JD requirements the candidate genuinely lacks, with the fastest credible way \
to close each (e.g. a weekend project, an open-source contribution, a course).

## 30-Second Pitch
A 3-4 sentence elevator pitch for this exact role, for the recruiter screen."""


def tailor(resume_text: str, jd_text: str, category: str) -> str:
    if not os.environ.get("ANTHROPIC_API_KEY"):
        raise SystemExit(
            "Resume tailoring uses the Claude API. Set ANTHROPIC_API_KEY "
            "(https://platform.claude.com) or use the offline `resume score` instead.")
    import anthropic

    analysis = score(resume_text, jd_text, category).render()
    client = anthropic.Anthropic()
    with client.messages.stream(
        model="claude-opus-4-8",
        max_tokens=64000,
        thinking={"type": "adaptive"},
        system=SYSTEM,
        messages=[{
            "role": "user",
            "content": PROMPT.format(jd=jd_text[:30000], resume=resume_text[:30000],
                                     analysis=analysis),
        }],
    ) as stream:
        for text in stream.text_stream:
            print(text, end="", flush=True)
        final = stream.get_final_message()
    print()
    return next((b.text for b in final.content if b.type == "text"), "")

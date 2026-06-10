# jobhunt — new-grad silicon & firmware job aggregator + resume toolkit

Tracks **new-grad / early-career** openings in **ASIC design, design
verification, DFT, physical design, and firmware** across ~40 big-tech and
semiconductor companies, writes them all (with apply links) to
`jobhunt-data/JOBS.md`, **notifies you when new roles appear**, and includes a
**resume scorer/tailor** to customize your CV per posting.

## Quick start (local)

```bash
pip install -r jobhunt/requirements.txt

python -m jobhunt doctor        # 1. check which job boards respond from your network
python -m jobhunt fetch         # 2. pull everything -> jobhunt-data/JOBS.md
python -m jobhunt list --new-grad
python -m jobhunt watch --interval 3600 --notify   # live local mode
```

Run all commands from the repository root.

## What it covers

| ATS | Companies |
|---|---|
| Workday | NVIDIA, AMD, Broadcom, Intel, Micron, Marvell, Analog Devices, NXP, onsemi, GlobalFoundries, Samsung Semi, Synopsys, Cadence, Arm, Skyworks, HPE/Juniper, Blue Origin, Rivian, Lucid, Keysight |
| Eightfold | Qualcomm, Western Digital, Texas Instruments |
| Greenhouse | SiFive, Cerebras, Groq, Tenstorrent, Anduril, Astera Labs, d-Matrix, SambaNova, SpaceX, Waymo, xAI, Rivos, Skydio |
| Lever | Zoox, PsiQuantum |
| Custom APIs | Amazon (incl. Annapurna Labs), Google, Microsoft, Tesla, Apple |

Meta, IBM and MediaTek have no stable public API — manual links are listed at
the bottom of `jobhunt/config.yaml`.

**These are unofficial endpoints.** Companies occasionally rename Workday
tenants or move ATSes. `python -m jobhunt doctor` pings every source and tells
you exactly which ones are broken; fix the entry in `jobhunt/config.yaml`
(open the company's careers page, look for the
`/wday/cxs/<tenant>/<site>/jobs` request in your browser's network tab, and
copy host/tenant/site). Adding a company is one YAML line.

## How filtering works

1. **Category match** on the title (priority order: DFT → physical design →
   verification → firmware → ASIC/RTL design), patterns in `jobhunt/filters.py`.
2. **Exclusions**: intern/co-op, senior/staff/principal/lead/manager/architect
   titles, and descriptions demanding ≥ 4 years experience.
3. **🎓 New-grad tagging**: "new college grad", "university graduate", "early
   career", "entry level", "0-2 years", etc. in the title or description.
   Set `only_new_grad: true` in config to keep *only* tagged roles — by default
   untagged entry-level-looking roles are kept too, since many companies don't
   label NG reqs.

## Live fetching + auto-notification

### Option A — GitHub Actions (recommended: runs even when your laptop is off)

`.github/workflows/job-watch.yml` polls every 4 hours, commits the refreshed
`jobhunt-data/JOBS.md`, and pushes notifications for new roles.

1. Merge this branch to your default branch (**schedules only run on the
   default branch**) and make sure Actions are enabled.
2. Add repo secrets (Settings → Secrets and variables → Actions) for the
   channels you want — any subset works:

   | Channel | Secrets | Notes |
   |---|---|---|
   | **ntfy (phone push — easiest)** | `NTFY_TOPIC` | Install the ntfy app, subscribe to a hard-to-guess topic like `passsp-jobs-x7k2`, done. No account needed. |
   | Discord | `DISCORD_WEBHOOK_URL` | Channel → Integrations → Webhooks |
   | Slack | `SLACK_WEBHOOK_URL` | Incoming webhook |
   | Email | `SMTP_HOST`, `SMTP_PORT`, `SMTP_USER`, `SMTP_PASS`, `EMAIL_TO` | For Gmail: `smtp.gmail.com` / `587` / your address / an [App Password](https://myaccount.google.com/apppasswords). `EMAIL_TO` defaults to the address in `config.yaml`. |

3. Test immediately: Actions → **Job Watch** → *Run workflow*.

### Option B — locally

```bash
NTFY_TOPIC=passsp-jobs-x7k2 python -m jobhunt watch --interval 3600 --notify
```

## Resume scorer & tailor

```bash
# Offline ATS-style score: keyword coverage vs the JD, missing skills, hygiene checks
python -m jobhunt resume score --resume my_cv.pdf --job-file jd.txt

# Claude-powered rewrite toward a specific posting (needs ANTHROPIC_API_KEY)
export ANTHROPIC_API_KEY=sk-ant-...
python -m jobhunt resume tailor --resume my_cv.pdf --job-file jd.txt --out tailored.md
```

- The JD can come from `--job-file` (paste the posting into a text file —
  most reliable), `--job-url`, or `--job-text "..."`.
- `--category` (asic_design / verification / dft / physical_design / firmware)
  is auto-detected from the JD but can be forced.
- `score` is fully offline: coverage bar, matched/missing skills from a
  curated silicon-skills taxonomy (UVM, ATPG, PrimeTime, Innovus, RTOS, ...),
  JD acronyms you don't mention, and ATS checks (one page, sections, GPA,
  contact info, no tables).
- `tailor` rewrites bullets toward the JD's exact terminology, **never
  invents experience** (anything inferred is marked `[VERIFY]`), and outputs:
  tailored resume + change rationale + real gaps with fastest fixes + a
  30-second pitch.

## Repo layout

```
jobhunt/                  the Python package (run as `python -m jobhunt`)
  config.yaml             companies, search queries, notification defaults
  sources/                one adapter per ATS (workday/greenhouse/lever/eightfold/custom)
  filters.py              category + new-grad + seniority filtering
  resume/                 skills taxonomy, parser, scorer, Claude tailor
jobhunt-data/             generated: JOBS.md (the board) + seen_jobs.json (state)
.github/workflows/job-watch.yml   scheduled live fetch + notify
```

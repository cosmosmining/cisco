#!/usr/bin/env python3
"""Parse curated new-grad job-list repos into a unified, classified CSV."""
import csv, html, os, re, sys

SRC = "/tmp/jobsrc"
OUT = "/tmp/jobs_out"
os.makedirs(OUT, exist_ok=True)

rows = []  # dicts

def strip_tags(s):
    return html.unescape(re.sub(r"<[^>]+>", "", s)).strip()

def clean_url(u):
    u = html.unescape(u.strip())
    u = re.sub(r"[?&](utm_[^&]*|ref|source|gh_src)=[^&]*", lambda m: "", u)
    u = re.sub(r"\?&", "?", u).rstrip("?&")
    return u

EMOJI_FLAGS = {"🛂": "no-sponsorship", "🇺🇸": "us-citizenship"}

def title_flags(t):
    flags = [v for k, v in EMOJI_FLAGS.items() if k in t]
    t = re.sub(r"[🛂🔒🎓🇺🇸]", "", t)
    t = re.sub(r"\s+", " ", t).strip(" -–")
    return t, ";".join(flags)

def add(company, title, location, url, age, source, region, salary="", workmodel=""):
    if not company or not title:
        return
    if "🔒" in title or "🔒" in company:
        return
    title, flags = title_flags(title)
    company = strip_tags(company).strip(" *_")
    title = strip_tags(title)
    location = re.sub(r"\s+", " ", strip_tags(location)).strip()
    if company in ("", "↳") or title == "":
        return
    rows.append(dict(company=company, title=title, location=location,
                     url=clean_url(url), age=age.strip(), source=source,
                     region=region, salary=salary.strip(), workmodel=workmodel.strip(),
                     flags=flags))

# ---------------- SimplifyJobs (HTML tables, sections by ## header) -------------
def parse_simplify(path):
    text = open(path, encoding="utf-8").read()
    sections = re.split(r"^## ", text, flags=re.M)
    for sec in sections:
        header = sec.split("\n", 1)[0]
        if not any(k in header for k in ("Software", "Hardware", "Data Science", "Quantitative")):
            continue
        cat = "simplify:" + ("HW" if "Hardware" in header else
                             "Quant" if "Quant" in header else
                             "AIML" if "Data" in header else "SW")
        last_company = ""
        for tr in re.findall(r"<tr>(.*?)</tr>", sec, re.S):
            tds = re.findall(r"<td[^>]*>(.*?)</td>", tr, re.S)
            if len(tds) < 5:
                continue
            comp_raw = strip_tags(tds[0])
            company = last_company if comp_raw.strip() in ("↳", "") else comp_raw
            if comp_raw.strip() not in ("↳", ""):
                last_company = comp_raw
            role = tds[1]
            loc = tds[2].replace("</br>", "; ").replace("<br>", "; ")
            links = re.findall(r'<a href="([^"]+)"', tds[3])
            apply_url = ""
            for l in links:
                if "simplify.jobs/p/" not in l:
                    apply_url = l; break
            if not apply_url and links:
                apply_url = links[0]
            age = strip_tags(tds[4])
            add(company, role, loc, apply_url, age, cat, "US")

# ---------------- jobright (markdown pipe table, ↳ inherit) ----------------------
def parse_jobright(path, tag):
    last_company = ""
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^\|\s*(.+?)\s*\|\s*\*\*\[(.+?)\]\((.+?)\)\*\*\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*$", line)
        if not m:
            continue
        comp_cell, title, url, loc, model, date = m.groups()
        cm = re.match(r"\*\*\[(.+?)\]", comp_cell)
        if cm:
            company = cm.group(1); last_company = company
        elif "↳" in comp_cell:
            company = last_company
        else:
            company = strip_tags(comp_cell)
            if company: last_company = company
        add(company, title, loc, url, date, tag, "US", workmodel=model)

# ---------------- speedyapply (pipe table w/ HTML cells + salary) ----------------
def parse_speedy(path, region):
    text = open(path, encoding="utf-8").read()
    for sec_name, block in re.findall(r"<!-- TABLE_(\w*?)_?START -->(.*?)<!-- TABLE_\w*?_?END -->", text, re.S):
        tag = "speedy:" + (sec_name if sec_name else "OTHER")
        for line in block.splitlines():
            m = re.match(r'^\|\s*<a href="[^"]*">\s*<strong>(.*?)</strong>\s*</a>\s*\|\s*(.*?)\s*\|\s*(.*?)\s*\|\s*(.*?)\s*\|\s*<a href="([^"]+)".*?\|\s*(.*?)\s*\|\s*$', line)
            if not m:
                continue
            company, pos, loc, sal, url, age = m.groups()
            add(company, pos, loc, url, age, tag, region, salary=sal)

# ---------------- vanshb03 (pipe table, **Company**) -----------------------------
def parse_vansh(path, region):
    last_company = ""
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*$", line)
        if not m:
            continue
        comp_cell, role, loc, app_cell, date = m.groups()
        if comp_cell.strip(": -") in ("Company", "---", ""):
            continue
        comp = strip_tags(comp_cell).strip(" *")
        if comp == "↳" or comp == "":
            comp = last_company
        else:
            last_company = comp
        um = re.search(r'href="([^"]+)"', app_cell)
        url = um.group(1) if um else ""
        add(comp, role, loc, url, date, "vansh", region)

parse_simplify(f"{SRC}/SimplifyJobs_New-Grad-Positions/README.md")
parse_jobright(f"{SRC}/jobright_engineering/README.md", "jobright:ENG")
parse_jobright(f"{SRC}/jobright-ai_2026-Software-Engineer-New-Grad/README.md", "jobright:SW")
parse_speedy(f"{SRC}/speedyapply_2026-SWE-College-Jobs/NEW_GRAD_USA.md", "US")
parse_speedy(f"{SRC}/speedyapply_2026-SWE-College-Jobs/NEW_GRAD_INTL.md", "Intl")
parse_vansh(f"{SRC}/vanshb03_New-Grad-2026/README.md", "US")
parse_vansh(f"{SRC}/vanshb03_New-Grad-2026/Canada.md", "Intl")

hist = f"{SRC}/jobright_history_rows.csv"
if "--include-history" in sys.argv and os.path.exists(hist):
    for h in csv.DictReader(open(hist, encoding="utf-8")):
        add(h["company"], h["title"], h["location"], h["url"], h["posted"],
            h["source"] + "@hist", "US", workmodel=h["workmodel"])
print(f"raw rows parsed: {len(rows)}")

# ================= classification =================
PAT = {
 "DFT": r"\bdft\b|design[\s-]*for[\s-]*test|atpg|mbist|\bscan\b|silicon test|test (?:engineer|development|chip)|product(?:ion)? test",
 "DV":  r"verification|\bverif\w*|\bdv\b|\buvm\b|emulation|formal",
 "RTL": r"\brtl\b|asic|digital design|digital ic|logic design|microarchitect|soc design|silicon design|cpu (?:design|core)|gpu (?:design|asic|hardware)|vlsi|physical design|logic synthesis|physical synthesis|timing analysis|timing closure|\bsta\b|\bic design\b|fpga|chip design|(?:ic|chip|asic|silicon|digital|logic|soc|cpu|gpu|memory|semiconductor) design",
 "EDA": r"\bcad\b|\beda\b|design automation|methodology",
 "FW":  r"firmware|embedded|\brtos\b|bare[\s-]*metal|\bbsp\b|device driver|kernel",
 "VAL": r"validation|post[\s-]*silicon|bring[\s-]*up|silicon insights",
 "HW":  r"hardware|silicon|semiconductor|electrical engineer",
 "SW":  r"software|swe\b|developer|machine learning|\bml\b|\bai\b|data engineer|compiler|systems engineer|infrastructure|quant",
}
ORDER = ["DFT", "DV", "EDA", "RTL", "FW", "VAL", "HW", "SW"]
TRACKNAME = {"DFT": "DFT / Silicon Test", "DV": "Design Verification", "RTL": "RTL / ASIC Design",
             "EDA": "EDA / CAD", "FW": "Firmware / Embedded", "VAL": "Silicon Validation",
             "HW": "Hardware (general)", "SW": "Software"}

SENIOR = re.compile(r"\b(senior|staff|principal|sr\.?|lead|manager|director|architect|distinguished|fellow)\b", re.I)
INTERN = re.compile(r"\bintern(ship)?\b|co-?op\b|\bphd\b.*\bintern", re.I)
# disciplines outside the candidate's five digital tracks
EXCL = re.compile(r"\b(mechanical|civil|structural|manufactur\w*|industrial engineer|environmental|"
                  r"chemical|materials|biomedical|geotech|mining|petroleum|hvac|plumbing|fire protection|"
                  r"transportation|traffic|water|wastewater|landscape|aerospace stress|analog|mixed[- ]signal|"
                  r"\brf\b|antenna|photonic|optic\w*|pcb|board design|power electronic\w*|signal integrity|"
                  r"packag\w*|thermal|reliability|process engineer|equipment engineer|technician|"
                  r"quality engineer|sales|field service|application[s]? engineer|customer|facilities|"
                  r"battery|motor|harness|avionics|scientist|pharma|biolog|clinical|laborator\w+|"
                  r"propulsion|vehicle|spacecraft|launch)\b", re.I)
STRONG_DIGITAL = re.compile(r"\brtl\b|asic|\bsoc\b|digital|silicon|\bdft\b|verification|microarchitect|"
                            r"\bcpu\b|\bgpu\b|vlsi|firmware|embedded|\bfpga\b", re.I)
DFT_STRONG = re.compile(r"\bdft\b|atpg|mbist|scan|design[\s-]*for[\s-]*test|silicon test|ate\b|"
                        r"post[\s-]*silicon|product engineer|system level test|\bslt\b", re.I)
HWISH = re.compile(r"hardware|chip|silicon|\bic\b|semiconductor|asic|soc|wafer", re.I)
# ITAR/defense employers: realistically require US citizenship/person status
DEFENSE = ["northrop","lockheed","rtx","raytheon","collins aerospace","pratt","hii","huntington ingalls",
           "general dynamics","bae","boeing","l3harris","leidos","booz","mitre","draper","apl",
           "sandia","lawrence livermore","los alamos","spacex","blue origin","anduril","sierra nevada",
           "sierra space","rocket lab","kbr","saic","caci","peraton","textron","honeywell aero"]

BIGTECH = ["nvidia","amd","apple","google","alphabet","meta","microsoft","amazon","aws","annapurna","lab126","kuiper",
           "tesla","netflix","oracle","ibm","adobe","salesforce","uber","airbnb","stripe","cisco","intel","twitch","waymo","deepmind","youtube","tiktok","bytedance","linkedin","paypal","block","square","pinterest","snap","roblox","doordash","coinbase","databricks","snowflake","palantir","figma","openai","anthropic","x.ai","xai","spacex","anduril","rivian","lucid","zoox","cruise","qualcomm","broadcom","marvell","micron","texas instruments","analog devices","arm","mediatek","tsmc","samsung","sk hynix","western digital","sandisk","seagate","kioxia","gE aerospace x","nxp","infineon","stmicro","renesas","onsemi","microchip","synopsys","cadence","siemens","keysight","teradyne","advantest","kla","applied materials","lam research","asml","globalfoundries","rambus","astera","credo","sifive","tenstorrent","cerebras","groq","ampere","rivos","d-matrix","garmin","juniper","arista","hpe","hewlett packard","dell","sony","bosch","qorvo","skyworks","marvel"]
QUANT = ["citadel","jane street","hudson river","jump trading","optiver","susquehanna","five rings","tower research","two sigma","akuna","flow traders","virtu","millennium","point72","belvedere","old mission","wolverine","transmarket","dv trading","chicago trading","aquatic","geneva trading","squarepoint"]
QUANT_WB = re.compile(r"\b(sig|imc|drw|ctc|xtx|hrt)\b", re.I)
SEMI_EXTRA = ["ase","amkor","powerchip","umc","vanguard","novatek","realtek","phison","silicon motion","himax","asmedia","parade","andes","faraday","alchip","ememory","m31","global unichip","guc","etron","sunplus","elan","nuvoton","ite tech","weltrend","fitipower","sigmastar","artery"]

def tier_of(company, salary_num):
    c = company.lower()
    if any(q in c for q in QUANT) or QUANT_WB.search(c):
        return "Quant/HFT (top salary)"
    if any(b in c for b in BIGTECH):
        return "Big Tech / Semi leader"
    if any(s in c for s in SEMI_EXTRA):
        return "Semiconductor (TW/global)"
    if salary_num >= 150:
        return "High salary ($150k+)"
    return "Other"

def salary_num_of(s):
    m = re.search(r"\$(\d{2,3})k", s)
    return int(m.group(1)) if m else 0

seen = set()
final = []
for r in rows:
    t = r["title"]
    if not r["url"]:
        continue
    if INTERN.search(t):
        continue
    if SENIOR.search(t):
        continue
    tl = t.lower()
    if EXCL.search(tl) and not STRONG_DIGITAL.search(tl):
        continue
    track = None
    for k in ORDER:
        if re.search(PAT[k], tl, re.I):
            track = k; break
    if track is None:
        continue
    sal = salary_num_of(r["salary"])
    tier = tier_of(r["company"], sal)
    # generic test-engineer rows only count as DFT when clearly silicon-related
    if track == "DFT" and not DFT_STRONG.search(tl) and not HWISH.search(tl):
        continue
    if track == "EDA" and tier == "Other" and not re.search(r"eda|design automation|vlsi|\bic\b|silicon", tl):
        continue
    # software & generic rows only kept when big-tech / quant / high-salary
    if track in ("SW", "HW") and tier == "Other":
        continue
    if track == "HW" and "electrical engineer" in tl and tier == "Other":
        continue
    cl = r["company"].lower()
    if any(d in cl for d in DEFENSE):
        r["flags"] = (r["flags"] + ";" if r["flags"] else "") + "us-person-likely"
    # dedupe: req id from url if available, else company+title+loc
    rid = None
    for pat in (r"(JR\d{6,})", r"[_/](R\d{5,})\b", r"jobs/(\d{6,})", r"/job/(\d{6,})", r"gh_jid=(\d+)", r"jobs/info/([a-f0-9]{12,})"):
        m = re.search(pat, r["url"])
        if m: rid = m.group(1); break
    STOP = {"us","usa","united","states","america","of"}
    loctok = "".join(sorted(set(re.findall(r"[a-z]+", r["location"].lower())) - STOP))
    titlenorm = re.sub(r"new (college )?grad(uate)?|university grad|early career|20\d\d|[^a-z0-9]", "", tl)
    key = rid or re.sub(r"[^a-z0-9]", "", r["company"].lower()) + titlenorm + loctok
    key2 = re.sub(r"[^a-z0-9]", "", r["company"].lower()) + titlenorm + loctok
    if key in seen or key2 in seen:
        continue
    seen.add(key); seen.add(key2)
    final.append(dict(Company=r["company"], Title=t, Track=TRACKNAME[track],
                      Location=r["location"], Region=r["region"], Tier=tier,
                      Salary=r["salary"], Flags=r["flags"], Posted_Age=r["age"],
                      WorkModel=r["workmodel"], Source=r["source"], URL=r["url"]))

# tier sort for readability
TIER_ORDER = {"Big Tech / Semi leader": 0, "Quant/HFT (top salary)": 1,
              "Semiconductor (TW/global)": 2, "High salary ($150k+)": 3, "Other": 4}
TRACK_ORDER = {"RTL / ASIC Design": 0, "Design Verification": 1, "DFT / Silicon Test": 2,
               "Silicon Validation": 3, "EDA / CAD": 4, "Firmware / Embedded": 5,
               "Hardware (general)": 6, "Software": 7}
final.sort(key=lambda r: (TIER_ORDER[r["Tier"]], TRACK_ORDER[r["Track"]], r["Company"].lower()))

cols = ["Company","Title","Track","Location","Region","Tier","Salary","Flags","Posted_Age","WorkModel","Source","URL"]
with open(f"{OUT}/master.csv", "w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=cols); w.writeheader(); w.writerows(final)

from collections import Counter
print(f"final rows: {len(final)}")
print("\nby track:"); [print(f"  {k:24} {v}") for k, v in Counter(r['Track'] for r in final).most_common()]
print("\nby tier:");  [print(f"  {k:28} {v}") for k, v in Counter(r['Tier'] for r in final).most_common()]
print("\nbig-tech hardware (non-SW) count:",
      sum(1 for r in final if r['Tier'] != 'Other' and r['Track'] != 'Software'))
print("hardware total (all tiers):", sum(1 for r in final if r['Track'] != 'Software'))

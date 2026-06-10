"""Offline sanity tests: filtering, store diff, report rendering, resume scoring.

Run:  python -m jobhunt.tests.test_offline
"""
from __future__ import annotations

import json
import os
import tempfile

from ..filters import filter_jobs, categorize, has_new_grad_signal, is_excluded
from ..models import Job
from ..report import write_report
from ..store import Store
from ..resume.analyzer import score


def test_categorize():
    cases = {
        "ASIC Design Engineer - New College Grad": "asic_design",
        "Design Verification Engineer (UVM)": "verification",
        "ASIC Design Verification Engineer": "verification",
        "DFT Engineer, Early Career": "dft",
        "ASIC Physical Design Engineer": "physical_design",
        "Firmware Engineer - SSD Controller": "firmware",
        "Embedded Software Engineer, New Grad": "firmware",
        "CPU RTL Design Engineer": "asic_design",
        "Software Engineer, Frontend": None,
        "Account Executive": None,
        "Mechanical Design Engineer": None,
        "Physical Design Engineer, University Graduate": "physical_design",
        "Silicon Validation Engineer": "verification",
    }
    for title, want in cases.items():
        got = categorize(title)
        assert got == want, f"{title!r}: want {want}, got {got}"


def test_levels():
    assert has_new_grad_signal("ASIC Engineer, New College Grad 2026")
    assert has_new_grad_signal("Engineer", "ideal for recent graduates, 0-2 years experience")
    assert not has_new_grad_signal("ASIC Design Engineer")
    assert is_excluded("Senior DFT Engineer")
    assert is_excluded("ASIC Verification Intern")
    assert is_excluded("Staff Physical Design Engineer")
    assert is_excluded("Principal Engineer, RTL")
    assert is_excluded("DV Engineer", "requires 8+ years of UVM experience")
    assert not is_excluded("DV Engineer", "Bachelor's degree and 0+ years experience; 2+ years is a plus")
    assert not is_excluded("ASIC Design Engineer, New Grad")
    # 'lead' and 'architect' filtered
    assert is_excluded("Lead Verification Engineer")
    assert is_excluded("CPU Architect")


def test_pipeline_and_report():
    raw = [
        Job("NVIDIA", "ASIC Design Engineer - New College Grad", "https://x/1", "Santa Clara, CA"),
        Job("NVIDIA", "ASIC Design Engineer - New College Grad", "https://x/1"),  # dupe
        Job("AMD", "Senior DFT Engineer", "https://x/2"),                          # excluded
        Job("Qualcomm", "DV Engineer, Early Career", "https://x/3", "San Diego, CA"),
        Job("Google", "Chef", "https://x/4"),                                       # no category
        Job("Micron", "DFT Engineer", "https://x/5", "Boise, ID",
            description="Bachelor's and 0-2 years experience. ATPG, MBIST."),
    ]
    matched = filter_jobs(raw)
    assert len(matched) == 3, [j.title for j in matched]
    assert {j.category for j in matched} == {"asic_design", "verification", "dft"}
    ng = [j for j in matched if j.new_grad]
    assert len(ng) == 3  # NCG title, Early Career title, 0-2 years description

    with tempfile.TemporaryDirectory() as d:
        store = Store(os.path.join(d, "seen.json"))
        new = store.diff_and_update(matched)
        assert len(new) == 3
        store.save()
        # second run: nothing new
        store2 = Store(os.path.join(d, "seen.json"))
        assert store2.diff_and_update(matched) == []
        # one fresh job appears
        fresh = filter_jobs([Job("Apple", "Physical Design Engineer (New Grad)", "https://x/9")])
        assert [j.title for j in store2.diff_and_update(fresh)] == ["Physical Design Engineer (New Grad)"]

        report_path = os.path.join(d, "JOBS.md")
        write_report(store2.all_records(), report_path, errors={"BrokenCo": "HTTP 404"})
        text = open(report_path, encoding="utf-8").read()
        assert "ASIC / RTL Design" in text and "https://x/1" in text
        assert "🎓" in text and "BrokenCo" in text
        json.loads(open(os.path.join(d, "seen.json")).read())  # valid JSON


def test_resume_score():
    resume = """
    Jane Doe — jane@example.com — (555) 123-4567 — github.com/janedoe
    EDUCATION: BS Electrical Engineering, GPA 3.8/4.0
    SKILLS: SystemVerilog, UVM, Python, Git, Linux
    EXPERIENCE: Built a UVM testbench with functional coverage and SVA assertions
    for an AXI DMA block; ran regressions with VCS. PROJECTS: RISC-V core on FPGA.
    """
    jd = """
    Design Verification Engineer, New College Grad. Develop UVM testbenches in
    SystemVerilog, write assertions (SVA) and functional coverage, debug with
    Verdi, run formal verification with JasperGold. Scripting in Python or Perl.
    """
    result = score(resume, jd, "verification")
    assert "UVM" in result.matched and "SystemVerilog" in result.matched
    assert "Verdi" in result.missing and "Formal Verification" in result.missing
    assert 0.3 < result.coverage < 1.0
    rendered = result.render()
    assert "Keyword coverage" in rendered and "MISSING" in rendered


if __name__ == "__main__":
    test_categorize()
    test_levels()
    test_pipeline_and_report()
    test_resume_score()
    print("all offline tests passed")

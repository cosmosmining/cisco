"""Per-category hardware/firmware skill taxonomy used for resume <-> JD matching.

Each entry is (canonical name, regex). Regexes are matched case-insensitively
against whole text; \b guards keep "C" from matching everywhere.
"""
from __future__ import annotations

import re

COMMON = [
    ("Verilog", r"\bverilog\b"), ("SystemVerilog", r"system\s*verilog|\bsv\b"),
    ("VHDL", r"\bvhdl\b"), ("Python", r"\bpython\b"), ("C", r"(?<![\w+#])c(?![\w+#])"),
    ("C++", r"c\+\+"), ("Perl", r"\bperl\b"), ("Tcl", r"\btcl\b"),
    ("Bash/Shell", r"\bbash\b|\bshell script"), ("Git", r"\bgit\b"),
    ("Linux", r"\blinux\b|\bunix\b"), ("FPGA", r"\bfpga\b"),
    ("Computer Architecture", r"computer architecture"), ("SoC", r"\bsoc\b"),
    ("AXI", r"\baxi\b"), ("AHB/APB", r"\bahb\b|\bapb\b"), ("AMBA", r"\bamba\b"),
    ("PCIe", r"\bpci[- ]?e(?:xpress)?\b"), ("USB", r"\busb\b"),
    ("DDR/Memory", r"\bddr\d?\b|\blpddr\d?\b|\bhbm\d?\b"),
    ("Ethernet", r"\bethernet\b|\bserdes\b"), ("RISC-V", r"risc[- ]?v"),
    ("Arm Architecture", r"\barm\b|\bcortex"), ("CDC", r"clock domain crossing|\bcdc\b"),
    ("Low Power/UPF", r"low[- ]power|\bupf\b|power gating|clock gating"),
    ("VLSI/CMOS", r"\bvlsi\b|\bcmos\b"), ("Makefile", r"\bmakefile\b|\bmake\b"),
]

CATEGORY_SKILLS: dict[str, list[tuple[str, str]]] = {
    "asic_design": [
        ("RTL Design", r"\brtl\b"), ("Microarchitecture", r"micro[- ]?architect"),
        ("Synthesis", r"\bsynthesi[sz]"), ("Design Compiler", r"design compiler|\bdc\b shell"),
        ("Genus", r"\bgenus\b"), ("SpyGlass/Lint", r"spyglass|\blint\b"),
        ("Datapath", r"\bdatapath\b"), ("Pipelining", r"pipelin"),
        ("Cache/Coherency", r"\bcache\b|coherenc"), ("NoC/Interconnect", r"\bnoc\b|interconnect|fabric"),
        ("FIFO/Arbiter Design", r"\bfifo\b|\barbiter\b"), ("Timing Constraints (SDC)", r"\bsdc\b|timing constraint"),
        ("FPGA Prototyping", r"fpga prototyp|vivado|quartus"),
    ],
    "verification": [
        ("UVM", r"\buvm\b|\bovm\b|\bvmm\b"), ("Testbench Development", r"test\s*bench"),
        ("Functional Coverage", r"functional coverage|covergroup"),
        ("Code Coverage", r"code coverage"), ("Assertions (SVA)", r"\bsva\b|assertion"),
        ("Constrained Random", r"constrained[- ]random"),
        ("Scoreboard/Checker", r"scoreboard|checker"),
        ("VCS", r"\bvcs\b"), ("Questa/ModelSim", r"questa|modelsim"),
        ("Xcelium", r"xcelium|incisive"), ("Verdi", r"\bverdi\b"),
        ("Formal Verification", r"formal (?:verification|property)|jaspergold|\bjasper\b|vc formal"),
        ("Emulation", r"emulation|palladium|\bzebu\b|veloce"),
        ("cocotb", r"\bcocotb\b"), ("Regression/Debug", r"regression"),
        ("Post-Silicon Validation", r"post[- ]silicon|silicon validation|bring[- ]?up"),
    ],
    "dft": [
        ("DFT", r"\bdft\b|design[- ]for[- ]test"), ("Scan Insertion", r"\bscan\b"),
        ("ATPG", r"\batpg\b"), ("MBIST", r"\bmbist\b|memory bist"),
        ("LBIST", r"\blbist\b|logic bist"), ("JTAG (1149.1)", r"\bjtag\b|1149"),
        ("IEEE 1500", r"\b1500\b"), ("IJTAG (1687)", r"\bijtag\b|1687"),
        ("Boundary Scan", r"boundary scan"), ("Tessent", r"tessent"),
        ("Modus", r"\bmodus\b"), ("TetraMAX/TestMAX", r"tetramax|testmax"),
        ("Fault Models", r"stuck[- ]at|transition fault|path delay|bridging"),
        ("Test Coverage", r"fault coverage|test coverage"),
        ("Scan Compression", r"compression|\bedt\b"),
        ("Silicon Debug/Yield", r"yield|silicon debug|diagnosis"),
    ],
    "physical_design": [
        ("Place & Route", r"place (?:and|&) route|\bpnr\b|p&r"),
        ("Innovus", r"innovus|encounter"), ("ICC2/Fusion Compiler", r"\bicc2?\b|fusion compiler"),
        ("PrimeTime", r"primetime|\bpt\b signoff"), ("STA", r"\bsta\b|static timing"),
        ("Timing Closure", r"timing closure|timing eco"),
        ("Floorplanning", r"floor[- ]?plan"), ("CTS", r"\bcts\b|clock tree"),
        ("DRC/LVS", r"\bdrc\b|\blvs\b|\berc\b"), ("Calibre", r"calibre"),
        ("IR Drop / EM", r"ir[- ]drop|\bem\b analysis|redhawk|voltus"),
        ("Extraction (StarRC)", r"starrc|extraction|\brc\b extraction"),
        ("ECO Flows", r"\beco\b"), ("DEF/LEF/GDS", r"\bdef\b|\blef\b|\bgds"),
        ("Power Planning", r"power (?:plan|grid|mesh)"),
        ("Physical Verification", r"physical verification|signoff"),
    ],
    "firmware": [
        ("RTOS", r"\brtos\b|freertos|zephyr|vxworks|threadx"),
        ("Embedded Linux", r"embedded linux|\byocto\b|buildroot|\bkernel\b"),
        ("Device Drivers", r"device driver|\bdriver\b development"),
        ("Bootloader", r"bootloader|u[- ]?boot|\buefi\b|\bbios\b"),
        ("Bare-Metal", r"bare[- ]?metal"), ("Interrupts/DMA", r"interrupt|\bisr\b|\bdma\b"),
        ("I2C/SPI/UART", r"\bi2c\b|\bspi\b|\buart\b|\bcan\b bus|\bgpio\b"),
        ("Memory-Mapped IO", r"memory[- ]map|register map|\bmmio\b"),
        ("Debugging (JTAG/GDB)", r"\bgdb\b|\bjtag\b debug|\bopenocd\b|lauterbach|trace32"),
        ("Lab Equipment", r"oscilloscope|logic analyzer|multimeter"),
        ("Board Bring-Up", r"bring[- ]?up"), ("Power Management FW", r"power management"),
        ("OTA/Secure Boot", r"\bota\b|secure boot|\bhsm\b|crypto"),
        ("Unit Testing/CI", r"unit test|\bci\b|jenkins|pytest"),
    ],
}


def skills_for(category: str) -> list[tuple[str, re.Pattern]]:
    pairs = COMMON + CATEGORY_SKILLS.get(category, [])
    return [(name, re.compile(rx, re.I)) for name, rx in pairs]

# SPDX-License-Identifier: GPL-2.0
"""Exercise pinned CCF function bodies; do not transcribe its no-op/error rules."""
import pathlib
import re

source = pathlib.Path("drivers/clk/clk.c").read_text()
names = [
    "clk_core_get_rate_nolock",
    "clk_recalc",
    "__clk_recalc_rates",
    "clk_core_get_rate_recalc",
    "clk_get_rate",
    "clk_change_rate",
    "clk_core_set_rate_nolock",
    "clk_core_set_parent_nolock",
]
with open("ccf.inc", "w") as output:
    for name in names:
        match = re.search(r"^(?:static )?(?:unsigned long|void|int) " + name + r"\([^;]*?\n\{.*?^\}", source, re.M | re.S)
        if not match:
            raise SystemExit(f"Pinned CCF function not found: {name}")
        output.write(match.group(0) + "\n\n")

# Bind the harness to the production descriptor's flags too.
provider = pathlib.Path("drivers/clk/sophgo/clk-cv1800.c").read_text()
match = re.search(r"static CV1800_FACTIONAL_PLL\(clk_disppll,.*?,\s*([^,]+)\);", provider, re.S)
if not match:
    raise SystemExit("DISPPLL descriptor not found")
with open("ccf.inc", "a") as output:
    output.write("#define TEST_DISPPLL_FLAGS (" + match.group(1).strip() + ")\n")

match = re.search(r"static CV1800_FACTIONAL_PLL\(clk_disppll,.*?REG_PLL_G2_STATUS, (\d+)", provider, re.S)
if not match:
    raise SystemExit("DISPPLL status descriptor not found")
with open("ccf.inc", "a") as output:
    output.write("#define TEST_DISPPLL_STATUS_SHIFT " + match.group(1) + "\n")

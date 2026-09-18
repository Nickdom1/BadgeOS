# SPDX-License-Identifier: GPL-2.0
"""Extract real PLL declarations, keeping the production descriptor macros."""
import pathlib
import re

source = pathlib.Path("drivers/clk/sophgo/clk-cv1800.c").read_text()
blocks = re.findall(
    r"^static (?:const struct cv1800_clk_pll_limit pll_limits\[.*?^};"
    r"|struct cv1800_clk_pll_synthesizer \w+ = \{.*?^};"
    r"|CV1800_(?:INTEGRAL|FACTIONAL)_PLL\(.*?\);)",
    source, re.M | re.S,
)
assert len(blocks) == 15, "Expected limits, six synthesizers and eight PLLs"
with open("descriptors.inc", "w") as output:
    for block in blocks:
        output.write(block + "\n")
        match = re.match(r"static CV1800_\w+_PLL\((\w+),\s*(\w+),", block)
        if match:
            output.write(f'#define {match[1]}_parent "{match[2]}"\n')

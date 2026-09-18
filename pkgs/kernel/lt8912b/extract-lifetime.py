# SPDX-License-Identifier: GPL-2.0-only
"""Exercise driver cleanup together with the pinned DRM core's ownership."""
import pathlib
import re


def function(source, name):
    match = re.search(
        r"^(?:static )?[^\n]*\b" + name + r"\([^;]*?\n\{.*?^\}",
        source, re.M | re.S,
    )
    assert match, name
    return match[0] + "\n"


driver = pathlib.Path("drivers/gpu/drm/bridge/lontium-lt8912b.c").read_text()
core = pathlib.Path("drivers/gpu/drm/drm_bridge.c").read_text()
assert "devm_drm_bridge_alloc" in function(driver, "lt8912_probe")
assert "drm_bridge_get(bridge);" in function(core, "drm_bridge_add")
assert "drm_bridge_put(bridge);" in function(core, "drm_bridge_put_void")
assert "kref_init(&bridge->refcount);" in function(core, "__devm_drm_bridge_alloc")
assert "drm_bridge_get(bridge);" in function(core, "of_drm_find_and_get_bridge")
with open("lifetime-core.inc", "w") as out:
    for name in ("__drm_bridge_free", "drm_bridge_get", "drm_bridge_put"):
        out.write(function(core, name))
with open("lifetime-driver.inc", "w") as out:
    for name in ("lt8912_parse_dt", "lt8912_put_dt"):
        out.write(function(driver, name))

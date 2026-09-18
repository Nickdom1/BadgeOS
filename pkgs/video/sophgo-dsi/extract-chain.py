# SPDX-License-Identifier: GPL-2.0-only
"""Compile pinned DRM ordering code with fake lists and bridge callbacks."""
import pathlib
import re
source = pathlib.Path('drivers/gpu/drm/drm_bridge.c').read_text()
names = ['drm_atomic_bridge_chain_disable', 'drm_atomic_bridge_call_post_disable',
         'drm_atomic_bridge_chain_post_disable', 'drm_atomic_bridge_call_pre_enable',
         'drm_atomic_bridge_chain_pre_enable', 'drm_atomic_bridge_chain_enable']
with open('chain.inc', 'w') as out:
    for name in names:
        match = re.search(r'^(?:static )?void ' + name + r'\(.*?^}', source, re.M | re.S)
        assert match, name
        out.write(match[0] + '\n')
helper = pathlib.Path('drivers/gpu/drm/drm_atomic_helper.c').read_text()
body = re.search(r'void drm_atomic_helper_commit_modeset_enables\(.*?^}', helper, re.M | re.S)[0]
assert body.index('commit_crtc_enable(') < body.index('bridge_pre_enable(') < body.index('bridge_enable(')

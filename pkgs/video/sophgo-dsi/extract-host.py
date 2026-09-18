# SPDX-License-Identifier: GPL-2.0-only
"""Extract production host callbacks; framework operations are supplied by C tests."""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
with open('host.inc', 'w') as out:
    for name in ['sophgo_dsi_attach', 'sophgo_dsi_detach']:
        match = re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\n\{.*?^\}',
                          source, re.M | re.S)
        assert match, name
        out.write(match[0] + '\n')
probe = re.search(r'static int sophgo_dsi_probe\(.*?^}', source, re.M | re.S)[0]
assert 'of_drm_find' not in probe and 'drm_dev_register' not in probe
assert 'mipi_dsi_host_register' in probe
assert not re.search(r'pipeline_ready\s*=\s*(true|1)', source)
create = re.search(r'static int sophgo_kms_create\([^;]*?\n\{.*?^}', source, re.M | re.S)[0]
assert create.index('sophgo_pipeline_attach') < create.index('drm_dev_register')
assert 'DRM_BRIDGE_ATTACH_NO_CONNECTOR' in source
assert 'drmm_mode_config_init' in create  # survives retained DRM files
pathlib.Path('kms.inc').write_text(create + '\n')

with open('lifetime.inc', 'w') as out:
    for name in ['sophgo_kms_destroy', 'sophgo_ioctl', 'sophgo_compat_ioctl',
                 'sophgo_release', 'sophgo_atomic_commit']:
        match = re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\n\{.*?^\}',
                          source, re.M | re.S)
        assert match, name
        out.write(match[0] + '\n')
assert '.unlocked_ioctl = sophgo_ioctl' in source
assert '.compat_ioctl = sophgo_compat_ioctl' in source
assert '.release = sophgo_release' in source
assert '.atomic_commit = sophgo_atomic_commit' in source
assert 'drm_mode_config_cleanup' not in source
for name in ['detect', 'modes', 'fill_modes']:
    body = re.search(r'static [^\n]* sophgo_connector_' + name + r'\(.*?^}',
                     source, re.M | re.S)[0]
    assert 'drm_dev_enter' in body and 'drm_dev_exit' in body

with open('check.inc', 'w') as out:
    for name in ['sophgo_plane_check', 'sophgo_mode_timing', 'sophgo_crtc_check']:
        body = re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\n\{.*?^\}', source, re.M | re.S)
        assert body, name
        out.write(body[0] + '\n')
assert 'new_state->event' in source
assert '.suspend = sophgo_dsi_suspend' in source
assert 'devm_drm_bridge_alloc' in source and 'devm_drm_bridge_add' in source
assert 'sophgo_pipeline_complete(&kms->dsi->pipeline' in source
body = re.search(r'^static void sophgo_pipeline_shutdown\([^;]*?\n\{.*?^\}', source, re.M | re.S)
assert body
pathlib.Path('pipeline-shutdown.inc').write_text(body[0] + '\n')

with open('manual.inc', 'w') as out:
    out.write(re.search(r'static const struct drm_display_mode sophgo_golden_mode = .*?^};', source, re.M | re.S)[0] + '\n')
    for name in ['pinstripe_show', 'pinstripe_store', 'pinstripe_progress_show']:
        out.write(re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\n\{.*?^\}', source, re.M | re.S)[0] + '\n')
assert 'wait_for_completion(&physical_recovery)' in source
assert 'panic(' not in source and 'kernel_restart(' not in source

with open('trace.inc', 'w') as out:
    for name in ['sophgo_pinstripe_trace', 'pinstripe_progress_show']:
        out.write(re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\n\{.*?^\}', source, re.M | re.S)[0] + '\n')

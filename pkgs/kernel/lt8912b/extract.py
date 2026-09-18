# SPDX-License-Identifier: GPL-2.0
"""Compile actual patched lifecycle/register bodies, with only framework fakes."""
import pathlib
import re

source = pathlib.Path('drivers/gpu/drm/bridge/lontium-lt8912b.c').read_text()
names = [
    'lt8912_write_init_config', 'lt8912_write_mipi_basic_config',
    'lt8912_write_dds_config', 'lt8912_write_rxlogicres_config',
    'lt8912_init_i2c', 'bridge_to_lt8912', 'lt8912_hard_power_on', 'lt8912_stop',
    'lt8912_hard_power_off', 'lt8912_video_setup', 'lt8912_soft_power_on',
    'lt8912_video_on', 'lt8912_check_cable_status',
    'lt8912b_pipeline_bind', 'lt8912_bridge_pre_enable', 'lt8912_bridge_enable',
    'lt8912_bridge_disable', 'lt8912_bridge_post_disable',
    'lt8912_bridge_hpd_cb', 'lt8912_bridge_connector_init',
    'lt8912_bridge_attach', 'lt8912_bridge_detach',
]
with open('bridge.inc', 'w') as out:
    out.write(re.search(r'struct lt8912 \{.*?^};', source, re.M | re.S)[0] + '\n')
    out.write(re.search(r'static const struct regmap_config lt8912_regmap_config = \{.*?^};', source, re.M | re.S)[0] + '\n')
    for name in names:
        match = re.search(r'^(?:static )?[^\n]*\b' + name + r'\([^;]*?\n\{.*?^\}', source, re.M | re.S)
        if not match:
            raise SystemExit(f'Missing production function: {name}')
        out.write(match[0] + '\n\n')

# Catch descriptor wiring / untested entry points bypassing the tested lifecycle.
for field, function in [('pre_enable', 'pre_enable'), ('enable', 'enable'),
                        ('disable', 'disable'), ('post_disable', 'post_disable')]:
    assert f'.{field} = lt8912_bridge_{function},' in source
assert 'lt->bridge.pre_enable_prev_first = true;' in source
assert '.pm =' not in source
assert 'devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH)' in source
assert 'devm_i2c_new_dummy_device' in source
assert 'i2c_unregister_device' not in source
remove = re.search(r'static void lt8912_remove\(.*?^}', source, re.M | re.S)[0]
assert remove.index('lt8912_bridge_detach') < remove.index('drm_bridge_remove')
assert source.count('lt8912_hard_power_on(lt)') == 1
assert source.count('lt8912_soft_power_on(lt)') == 1
assert source.count('lt8912_video_on(lt)') == 1
assert 'lt8912_write_lvds_config' not in source

# Compare production tables with the numerical 720p reference fixture.
# It preserves the register sequences used by the Debian reference recipe;
# the executable recipe and its environment-specific paths are not inputs.
import sys
import json
fixture = json.loads(pathlib.Path(sys.argv[1]).read_text())
for name in ("lt8912_write_init_config", "lt8912_write_mipi_basic_config", "lt8912_write_dds_config"):
    function = re.search(r'^static int ' + name + r'\(.*?^}', source, re.M | re.S)[0]
    actual = [[int(r, 16), int(v, 16)] for r, v in re.findall(
        r'\{(0x[0-9a-fA-F]+), (0x[0-9a-fA-F]+)\}', function)]
    assert actual == fixture[name], f'{name} differs from reference fixture'
    print(f'PASS: {name}: {len(actual)} reference register entries')
with open('golden.inc', 'w') as out:
    out.write('static const struct reg_sequence golden_landscape[] = {\n')
    for r, v in fixture["landscape"]:
        out.write(f'{{0x{r:02x}, 0x{v:02x}}},\n')
    out.write('};\n')

# Removal must detach the host while bridge/DT/devres state is still live.
assert remove.index('devm_release_action') < remove.index('lt8912_bridge_detach')
assert 'devm_mipi_dsi_attach' not in source
with open('remove.inc', 'w') as out:
    for name in ['lt8912_detach_dsi', 'lt8912_attach_dsi', 'lt8912_remove']:
        body = re.search(r'^(?:static )?[^\n]*\b' + name + r'\([^;]*?\n\{.*?^\}',
                         source, re.M | re.S)
        assert body, name
        out.write(body[0] + '\n')

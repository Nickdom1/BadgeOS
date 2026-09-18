// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define I2C_MAIN 0
#define I2C_CEC_DSI 1
#define I2C_MAX_IDX 2
#define I2C_ADDR_MAIN 0x48
#define I2C_ADDR_CEC_DSI 0x49
#define I2C_BOARD_INFO(t, a) .type = (t), .addr = (a)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define DISPLAY_FLAGS_HSYNC_HIGH BIT(0)
#define DISPLAY_FLAGS_VSYNC_HIGH BIT(1)
#define DRM_BRIDGE_OP_HPD BIT(0)
#define DRM_CONNECTOR_POLL_HPD 1
#define DRM_CONNECTOR_POLL_CONNECT 2
#define DRM_CONNECTOR_POLL_DISCONNECT 4
#define DRM_MODE_DPMS_OFF 0
static const int lt8912_connector_funcs;
static const int lt8912_connector_helper_funcs;
#define dev_err(dev, ...) do { (void)(dev); errors++; } while (0)
typedef uint8_t u8;
typedef uint32_t u32;
struct device { int unused; };
struct device_node { int unused; };
struct i2c_client { void *adapter; };
struct i2c_board_info { const char *type; unsigned short addr; };
struct mipi_dsi_device { int unused; };
struct gpio_desc { int unused; };
struct mutex { bool held; };
struct regulator_bulk_data { int unused; };
struct regmap { int bank; };
struct regmap_config { unsigned int reg_bits, val_bits, max_register; };
static struct regmap maps[] = { {0}, {1} };
static struct i2c_client dummy;
static bool dummy_owned, fail_dummy;
static unsigned int map_calls;
static int fail_map = -1;
static struct i2c_client *devm_i2c_new_dummy_device(struct device *dev,
		void *adapter, unsigned short addr)
{
	(void)dev; (void)adapter;
	assert(addr == 0x49 && !dummy_owned);
	if (fail_dummy) return ERR_PTR(-ENOMEM);
	dummy_owned = true;
	return &dummy;
}
static struct regmap *devm_regmap_init_i2c(struct i2c_client *c,
		const struct regmap_config *config)
{
	assert(c && !IS_ERR(c) && config->reg_bits == 8 && config->val_bits == 8);
	unsigned int bank = map_calls++;
	assert(bank < 2);
	if ((int)bank == fail_map) return ERR_PTR(-EIO);
	return &maps[bank];
}
struct reg_sequence { unsigned int reg, def; };
struct drm_encoder { int unused; };
struct drm_bridge {
	void *dev;
	struct drm_encoder *encoder;
	struct drm_bridge *next_bridge;
	unsigned int ops;
	int type;
};
struct drm_connector { void *dev; int polled, dpms; };
struct videomode {
	u32 hactive, hfront_porch, hsync_len, hback_porch;
	u32 vactive, vfront_porch, vsync_len, vback_porch;
	u32 flags;
};
enum drm_connector_status {
	connector_status_unknown, connector_status_connected,
	connector_status_disconnected,
};
enum drm_bridge_attach_flags { DRM_BRIDGE_ATTACH_NO_CONNECTOR = 1 };
static unsigned int writes, fail_write, errors, rails;
static int fail_rail, fail_disable, fail_attach, fail_connector, fail_encoder;
static unsigned int enables, disables, hpd, reads;
static bool next_attached, rc_board, fail_receiver_stop;
static struct gpio_desc reset_gpio;
static bool reset = true, host_phy, host_video;
static unsigned int regs[2][256];
static char trace[1024];
static unsigned int events;
static void event(char e) { assert(events < sizeof(trace)); trace[events++] = e; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static void msleep(unsigned int n) { (void)n; }
static void usleep_range(unsigned int a, unsigned int b) { (void)a; (void)b; }
static void gpiod_set_value_cansleep(struct gpio_desc *g, int value)
{
	if (!g) return;
	if (!value) { assert(host_phy); assert(rails == 7); }
	reset = value;
	event(value ? 'R' : 'r');
}
static int regulator_bulk_enable(unsigned int n, struct regulator_bulk_data *s)
{
	(void)s;
	assert(n == 7 && (reset || rc_board) && !rails && host_phy);
	enables++;
	event('P');
	for (unsigned int i = 0; i < n; i++) {
		if ((int)i == fail_rail) { rails = 0; return -EIO; }
		rails++;
	}
	return 0;
}
static int regulator_bulk_disable(unsigned int n, struct regulator_bulk_data *s)
{
	(void)s;
	assert(n == 7 && (reset || rc_board) && rails == 7 && !host_video);
	disables++;
	event('p');
	if (fail_disable) return -EIO;
	rails = 0;
	return 0;
}
static int regmap_write(struct regmap *m, unsigned int r, unsigned int v)
{
	assert(rails == 7 && !reset && host_phy);
	writes++;
	event('I');
	if (writes == fail_write || (fail_receiver_stop && m->bank == I2C_MAIN && r == 0x03 && v == 0x7f)) return -EIO;
	regs[m->bank][r] = v;
	return 0;
}
static int regmap_multi_reg_write(struct regmap *m, const struct reg_sequence *s,
				 unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		int ret = regmap_write(m, s[i].reg, s[i].def);
		if (ret) return ret;
	}
	return 0;
}
static int regmap_update_bits(struct regmap *m, unsigned int r,
			      unsigned int mask, unsigned int v)
{
	return regmap_write(m, r, (regs[m->bank][r] & ~mask) | (v & mask));
}
static int regmap_read(struct regmap *m, unsigned int r, unsigned int *v)
{
	assert(!reset && rails == 7);
	reads++;
	*v = regs[m->bank][r];
	return 0;
}
static int drm_bridge_attach(struct drm_encoder *e, struct drm_bridge *n,
			     struct drm_bridge *b, unsigned int f)
{
	(void)e; (void)n; (void)b;
	assert(f == DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (!fail_attach) next_attached = true;
	return fail_attach;
}
static int drm_connector_init(void *dev, struct drm_connector *c,
			      const void *funcs, int type)
{
	(void)funcs; (void)type;
	if (fail_connector) return fail_connector;
	c->dev = dev;
	return 0;
}
static void drm_connector_helper_add(struct drm_connector *c, const void *f)
{ (void)c; (void)f; }
static void drm_connector_cleanup(struct drm_connector *c) { c->dev = NULL; }
static int drm_connector_attach_encoder(struct drm_connector *c, struct drm_encoder *e)
{ (void)c; (void)e; return fail_encoder; }
static void drm_helper_hpd_irq_event(void *dev) { assert(dev); }
static void drm_bridge_hpd_enable(struct drm_bridge *b,
		void (*cb)(void *, enum drm_connector_status), void *data)
{ (void)b; (void)cb; (void)data; assert(!hpd); hpd++; }
static void drm_bridge_hpd_disable(struct drm_bridge *b)
{ (void)b; assert(hpd == 1); hpd--; }
#include <drm/bridge/lt8912b.h>
#include "bridge.inc"
#include "golden.inc"

static struct drm_bridge next = { .ops = DRM_BRIDGE_OP_HPD };
static struct device device;
static struct lt8912 fresh(void)
{
	assert(!rails && !hpd);
	writes = fail_write = errors = enables = disables = reads = events = 0;
	fail_rail = -1;
	fail_disable = fail_attach = fail_connector = fail_encoder = 0;
	rc_board = fail_receiver_stop = false;
	reset = true; host_phy = host_video = next_attached = false;
	return (struct lt8912) {
		.dev = &device,
		.gp_reset = &reset_gpio,
		.bridge = { .next_bridge = &next, .dev = &device },
		.regmap = { &maps[0], &maps[1] }, .data_lanes = 4,
		.mode = {1280, 110, 40, 220, 720, 5, 5, 20,
			 DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH},
	};
}
static void prepare(struct lt8912 *lt)
{
	host_phy = true; event('H');
	lt8912_bridge_pre_enable(&lt->bridge);
}
static void enable(struct lt8912 *lt)
{
	host_video = true; event('V');
	lt8912_bridge_enable(&lt->bridge);
}
static void shutdown_bridge(struct lt8912 *lt)
{
	lt8912_bridge_disable(&lt->bridge);
	assert(reset && !lt->video_on && !lt->is_power_on);
	host_video = false; event('v');
	lt8912_bridge_post_disable(&lt->bridge);
	host_phy = false; event('h');
	assert(!rails && !lt->supplies_on);
}
int main(void)
{
	struct lt8912 lt = fresh();
	assert(lt8912_check_cable_status(&lt) == connector_status_unknown && !reads);
	lt8912_bridge_enable(&lt.bridge);
	assert(errors && !writes && !rails);
	prepare(&lt);
	assert(lt.is_power_on && lt.supplies_on && !lt.video_on);
	unsigned int init_writes = writes;
	lt8912_bridge_pre_enable(&lt.bridge);
	assert(enables == 1 && writes == init_writes);
	enable(&lt);
	assert(lt.video_on);
	for (unsigned int i = 0; i < ARRAY_SIZE(golden_landscape); i++)
		assert(regs[1][golden_landscape[i].reg] == golden_landscape[i].def);
	unsigned int all_writes = writes;
	lt8912_bridge_enable(&lt.bridge);
	assert(writes == all_writes);
	assert(regs[1][0x4e] == 0xff && regs[1][0x4f] == 0x56 && regs[1][0x50] == 0x69);
	assert(regs[0][0x44] == 0x31 && regs[0][0xab] == 3 && regs[0][0xb2] == 1);
	assert(regs[1][0x34] == 0x72 && regs[1][0x35] == 6);
	assert(regs[1][0x36] == 0xee && regs[1][0x37] == 2);
	shutdown_bridge(&lt);
	assert(trace[0] == 'H' && trace[1] == 'R' && trace[2] == 'P' && trace[3] == 'r');
	assert(trace[events-5] == 'R' && trace[events-4] == 'v' && trace[events-3] == 'R' && trace[events-2] == 'p');
	lt8912_bridge_disable(&lt.bridge);
	lt8912_bridge_post_disable(&lt.bridge);
	assert(disables == 1);
	prepare(&lt); enable(&lt); shutdown_bridge(&lt); /* DRM sleep restore */
	for (int rail = 0; rail < 7; rail++) {
		lt = fresh(); fail_rail = rail; prepare(&lt); enable(&lt);
		assert(reset && !rails && !writes && !lt.is_power_on && !lt.video_on);
		shutdown_bridge(&lt); assert(!disables);
	}
	for (unsigned int i = 1; i <= all_writes; i++) {
		lt = fresh(); fail_write = i; prepare(&lt); enable(&lt);
		assert(writes == i && errors && reset && rails == 7);
		assert(!lt.is_power_on && !lt.video_on && lt.supplies_on);
		assert(!disables); /* Host has not stopped yet. */
		shutdown_bridge(&lt); assert(disables == 1);
	}
	lt = fresh(); prepare(&lt); enable(&lt);
	fail_disable = 1;
	lt8912_bridge_disable(&lt.bridge); host_video = false;
	lt8912_bridge_post_disable(&lt.bridge);
	assert(reset && lt.supplies_on && !lt.is_power_on && !lt.video_on && rails == 7);
	lt8912_bridge_pre_enable(&lt.bridge);
	assert(enables == 1 && reset && !lt.is_power_on);
	fail_disable = 0; lt8912_bridge_post_disable(&lt.bridge);
	assert(!rails && !lt.supplies_on);
	for (int failure = 0; failure < 4; failure++) {
		lt = fresh();
		fail_attach = failure == 1 ? -EIO : 0;
		fail_connector = failure == 2 ? -ENOMEM : 0;
		fail_encoder = failure == 3 ? -EINVAL : 0;
		int ret = lt8912_bridge_attach(&lt.bridge, NULL, 0);
		assert((ret != 0) == (failure != 0));
		assert(next_attached == (failure == 0));
		assert((lt.connector.dev != NULL) == (failure == 0));
		assert(!writes && !enables && reset && hpd == (failure == 0));
		lt8912_bridge_detach(&lt.bridge); lt8912_bridge_detach(&lt.bridge);
		assert(!hpd && !disables);
	}
	lt = fresh();
	assert(!lt8912_bridge_attach(&lt.bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR));
	assert(!hpd && !enables);
	prepare(&lt); enable(&lt);
	host_video = false; /* Master has stopped before unbind. */
	lt8912_bridge_detach(&lt.bridge); /* defensive remove after active state */
	assert(reset && !rails && !lt.is_power_on && !lt.video_on);
	for (int failure = 0; failure < 4; failure++) {
		lt = fresh();
		struct i2c_client client = {0};
		lt.i2c_client[0] = &client;
		map_calls = 0;
		fail_map = failure == 1 ? 0 : failure == 3 ? 1 : -1;
		fail_dummy = failure == 2;
		int ret = lt8912_init_i2c(&lt, &client);
		assert((ret != 0) == (failure != 0));
		assert(dummy_owned == (failure == 0 || failure == 3));
		assert(!rails && !writes && !enables);
		/* Device-core devres release, including a failed second regmap. */
		dummy_owned = false;
	}
	/* RC reset has no GPIO; repeated I2C stop failure retains ownership. */
	lt = fresh();
	lt.rc_reset = rc_board = true;
	lt.gp_reset = NULL;
	reset = false;
	prepare(&lt); enable(&lt);
	assert(lt.video_on && !reset);
	fail_receiver_stop = true;
	for (int attempt = 0; attempt < 3; attempt++) {
		lt8912_bridge_disable(&lt.bridge);
		host_video = false;
		lt8912_bridge_post_disable(&lt.bridge);
		assert(rails == 7 && lt.supplies_on && lt.video_on && !reset);
	}
	fail_receiver_stop = false;
	lt8912_bridge_disable(&lt.bridge);
	assert(regs[I2C_MAIN][0x03] == 0x7f && !lt.video_on && !reset);
	lt8912_bridge_post_disable(&lt.bridge);
	assert(!rails && !lt.supplies_on && !reset);
	host_phy = false;

	printf("PASS: actual LT8912B callbacks; RC-board stop/retry and 7 regulator faults, %u I2C faults, ordering, repeat/sleep, attach/detach, shutdown failure\n", all_writes);
	return 0;
}

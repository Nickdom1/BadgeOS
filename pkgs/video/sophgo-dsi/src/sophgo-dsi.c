// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sophgo SG2000 MIPI DSI host: fixed 1280x720 scanout through an LT8912B.
 *
 * It registers a DSI host for the LT8912B bridge driver and, once the bridge
 * attaches, a DRM device whose fixed-mode CRTC scans an fbdev or dumb buffer
 * out of memory through the display block's DRAM reader.
 *
 * Register sequences follow sophgo/osdrv aa542c41df94f7bc656cb740f6622a5dca7dc403
 * (interdrv/vpss/chip/cv181x/{scaler.c,scaler.h,scaler_reg.h,reg_disp.h,dsi_phy.c},
 * interdrv/vo/chip/cv181x/{vo.c,vo_mipi_tx.c}, interdrv/base/chip/cv181x/reg_vip_sys.h)
 * and the SG2000 TRM v1.0 (sophgo/sophgo-doc 370d81b365ed528dc8f5dc91f63ed445e1142d4d).
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/lockdep.h>
#include <linux/irqflags.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <drm/bridge/lt8912b.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

/* Scaler top (sc_top): scaler_reg.h REG_SCL_TOP_*. */
#define SC_TOP_CFG0		0x00
#define SC_TOP_CFG1		0x04
#define SC_TOP_SHD		0x10
#define SC_TOP_VO_MUX		0x70
/* scaler.c:sclr_ctrl_init: the display fed from DRAM, the scalers off. */
#define SC_TOP_CFG0_INIT	0x80000008	/* force_clk_enable (31), ip_trig_src (3) */
#define SC_TOP_CFG1_INIT	0x00ff1200	/* sc_debug_en (12), qos_en (23:16) */
#define SC_TOP_REG_DONE		BIT(0)	/* sclr_top_reg_done */
#define SC_TOP_SHD_RAW_BANK	BIT(9)	/* reg_shrd_sel: read the working bank */
#define SC_TOP_FORCE_UP		0xff	/* sclr_top_reg_force_up: SHD[7:0] */

/* Display timing generator (disp): reg_disp.h. Counters are 14 bits, end:16 | start. */
#define DISP_CFG		0x00
#define DISP_TOTAL		0x04
#define DISP_VSYNC		0x08
#define DISP_VFDE		0x0c
#define DISP_VMDE		0x10
#define DISP_HSYNC		0x14
#define DISP_HFDE		0x18
#define DISP_HMDE		0x1c
#define DISP_PAT_CFG		0x94
#define DISP_PAT_COLOR0		0x98
#define DISP_PAT_COLOR1		0x9c
#define DISP_AXI_ST		0xb0	/* AXI_IDLE bit 0, AXI_STATE 15:8 */
#define DISP_CACHE		0xc0
#define DISP_CFG_POLARITY	0x60	/* bit 6 hsync high, bit 5 vsync high */
#define DISP_CFG_TGEN		BIT(7)
#define DISP_CFG_COMMIT		BIT(16)	/* shadow commit trigger */
#define DISP_CFG_SHADOW_MASK	BIT(17)
#define DISP_CFG_READ_WORKING	BIT(18)	/* sclr_disp_reg_shadow_sel(false) */
#define DISP_AXI_IDLE		BIT(0)
/* scaler.c:sclr_disp_set_cfg at init: RGB planar (2 << 12) from DRAM, no external sync. */
#define DISP_CFG_INIT_MASK	0xf01f
#define DISP_CFG_INIT		0x2000
#define DISP_CACHE_MODE		BIT(0)
#define DISP_TIMING_MASK	0x3fff3fff
/* scaler.c:sclr_disp_set_pattern: COLOR_BAR (7 << 24), enable (1), full-scale colours. */
#define DISP_PAT_MASK		0x1f000016
#define DISP_PAT_COLORBAR	0x07000002
#define DISP_PAT_OFF_MASK	0x16
#define DISP_PAT_WINDOW_BG	BIT(5)

/* DSI MAC (dsi_mac): scaler_reg.h REG_SCL_DSI_*; TRM mipi_tx_control_registers. */
#define MAC_EN			0x00	/* bits 2:0 read-write-set mode request, 5:4 done */
#define MAC_HS0			0x04	/* lanes 25:24, EoT 26, continuous clock 29, format 31:30 */
#define MAC_HS1			0x08	/* word count 15:0, event delay 26:16 */
#define MAC_MODE_VIDEO		0x4	/* SCLR_DSI_MODE_HS */
#define MAC_HS0_MASK		0xe7000000
#define MAC_HS1_MASK		0x07ffffff

/* D-PHY (dphy): dsi_phy.c offsets. */
#define DPHY_EN			0x00
#define DPHY_HS			0x14
#define DPHY_PLL		0x6c
#define DPHY_UPDATE		0x8c
#define DPHY_SET		0x90
#define DPHY_MAP		0x9c
#define DPHY_PN			0xa0

/* VIP system syscon: reg_vip_sys.h. Shared with capture: masked regmap access only. */
#define VIP_RESET		0x00
#define VIP_CLK_CTRL0		0x18
#define VIP_DISP_SEL_BT_DIV1	BIT(4)		/* dsi_phy.c:_cal_pll_reg pixel clock route */
#define VIP_DISPLAY_RESETS	0x04900a00 /* disp 9, dsi_mac 11, mac_apb 20, phy_apb 23, phy 26 */

/* vo_mipi_tx.c:_fill_disp_timing: sync 1..width, active from 1 + total - sync_start. */
#define DISP_TOTAL_720P		0x067102ed	/* 1649 << 16 | 749 */
#define DISP_HSYNC_720P		0x00280001	/* 40 pixels from 1 */
#define DISP_HACTIVE_720P	0x06040105	/* 261..1540 */
#define DISP_VSYNC_720P		0x00050001	/* 5 lines from 1 */
#define DISP_VACTIVE_720P	0x02e9001a	/* 26..745 */
#define DISP_POLARITY_720P	0x40	/* to the MAC: H high (bit 6), V low (bit 5 clear) */

/* scaler.c:sclr_dsi_config for two lanes; no EoT, as the bridge asks NO_EOT_PACKET. */
#define MAC_HS0_720P		0x21000000	/* lanes / 2 << 24, continuous clock (29) */
#define MAC_HS1_720P		0x00800f00	/* word count 1280 * 3, event delay 1280 / 10 */

/*
 * dsi_phy.c:dphy_dsi_set_lane for this board: pads 0..4 are data0, data1, clock and two
 * unused (DSI_LANE_MAX); 90 degree clock phase, no PN swap, no preamble at 891 Mbit/s.
 */
#define DPHY_MAP_720P		0x04055021
#define DPHY_PN_720P		0
#define DPHY_LANE_EN		0x07	/* clock, data0, data1 */
#define DPHY_PADS_OFF_720P	0x18	/* physical pads 3 and 4, powered down by dphy_init */
/* dsi_phy.c:dphy_set_hs_settle raw counts: trail 5, zero 32, prepare 6. */
#define DPHY_HS_720P		0x05200600
/*
 * dsi_phy.c:_cal_pll_reg, 25 MHz reference: 74250 kHz * 24 bpp = 1782000 kbit/s, 891000
 * per lane: VCO gain 2 (div_out 1), VCO 1782000 kHz, pixel divider 24 (<= 127: VIP div1),
 * loop 1, synthesizer set = (900000 * 8 << 26) / 1782000.
 */
#define DPHY_TXPLL_720P		0x118
#define DPHY_LOOP_720P		(1 << 20)
#define DPHY_SET_720P		0x10295fad
#define SOPHGO_ESC_HZ		20000000	/* clk_dsi_esc from AXI6 */
#define SOPHGO_DSI_SRC_HZ	900000000	/* MIPIMPLL full-rate synthesizer source */

/*
 * DISP DRAM reader: scaler.c sclr_disp_set_mem, sclr_disp_set_addr, sclr_disp_set_cfg and
 * sclr_disp_set_in_csc(SCL_CSC_NONE). It fetches one 1280x720 framebuffer at the origin,
 * unscaled, as SCL_FMT_BGR_PACKED: B,G,R bytes, which is DRM_FORMAT_RGB888. Addresses are
 * 40 bits (SRC_*_BASE_1 is 8 bits). Pitches fit the 16 bits below OS_MAX and, like the
 * addresses, are VIP_ALIGNMENT (64-byte) aligned; requests are SCL_DEFAULT_BURST, 128 bytes.
 * sclr_ctrl_init raises the fetch FIFO threshold to 0x10201020 "to avoid garbage"; it
 * latches with the reader words.
 */
#define SOPHGO_RD_ADDR0_L 0x34
#define SOPHGO_RD_PITCH_Y 0x4c
#define SOPHGO_RD_IN_CSC0 0x78
#define SOPHGO_RD_WORDS 11U /* ADDR0..2 L/H, PITCH_Y/C, OFFSET, SIZE, IN_CSC0 */
#define SOPHGO_RD_FMT_MASK 0xf007U /* fmt_sel [15:12], disp_sel [2:0] = DRAM */
#define SOPHGO_RD_OS_MAX 0x0f000000U
#define SOPHGO_RD_BURST 7U /* PITCH_Y[31:28] */
#define SOPHGO_RD_ALIGN 64ULL
#define SOPHGO_RD_ADDR_END (1ULL << 40)
#define SOPHGO_RD_FIFO_THR_REG 0x30
#define SOPHGO_RD_FIFO_THR 0x10201020U
#define SOPHGO_RD_FIFO (SOPHGO_RD_WORDS + 6U) /* after the six PAT words */
#define SOPHGO_RD_LATCH (SOPHGO_RD_WORDS + 7U) /* reader, PAT and FIFO words */
#define SOPHGO_RD_FMT_BGR 3U /* scaler.h SCL_FMT_BGR_PACKED */
#define SOPHGO_SO_WIDTH 1280U
#define SOPHGO_SO_HEIGHT 720U
#define SOPHGO_SO_PITCH_MAX 0xffffU

#define SOPHGO_FETCH_TRIES 3U
#define SOPHGO_FETCH_WAIT_NS 100000000ULL /* frame-end wait, about six frames */
#define SOPHGO_FETCH_HOLD_NS 2000000ULL /* idle run proving a cut, about 90 lines */
#define SOPHGO_FETCH_SETTLE_NS 25000000ULL /* one frame and margin */

enum sophgo_clock {
	SOPHGO_CLK_SC_TOP,
	SOPHGO_CLK_DISP,
	SOPHGO_CLK_DSI_MAC,
	SOPHGO_CLK_DSI_ESC,
	SOPHGO_CLK_DISPPLL,
	SOPHGO_CLK_DISP_SRC,
	SOPHGO_CLK_DSI_SRC,
	SOPHGO_CLK_CFG_REG,
	SOPHGO_CLK_COUNT,
};

/* Once per bind: detach unplugs, and only a new host bind registers again. */
enum sophgo_kms_state { SOPHGO_KMS_OFF, SOPHGO_KMS_REGISTERED, SOPHGO_KMS_DONE };

struct sophgo_reader_plan {
	unsigned int word[SOPHGO_RD_WORDS];
	struct drm_gem_object *gem; /* the object the address points into */
};

struct sophgo_dsi {
	struct drm_device drm;
	struct drm_encoder encoder;
	struct device *dev;
	struct mipi_dsi_host host;
	struct mipi_dsi_device *peripheral;
	struct drm_bridge *bridge;
	struct device_node *bridge_node;
	void __iomem *top, *disp, *mac, *phy;
	struct regmap *vip;
	struct clk_bulk_data clocks[SOPHGO_CLK_COUNT];
	struct clk *bt, *esc_parent;
	struct mutex lock; /* attach, detach, shutdown and the CRTC */
	struct lt8912b_pipeline status; /* the bridge's report on a start or stop */
	bool pll_held, source_held, esc_held; /* ownership: see sophgo_prepare() */
	bool clocks_on, bt_on, esc_set, div_set, phy_on, mmio_ready;
	struct clk *esc_old_parent;
	unsigned long esc_old_rate, pll_hz;
	unsigned int div_old;
	bool latch_failed;
	struct clk *reader_axi;
	bool reader_axi_on, reader_applied;
	struct work_struct kms_work;
	enum sophgo_kms_state kms_state;
	struct sophgo_reader_plan plan; /* the framebuffer last enabled or flipped to */
	int plan_refused; /* the enable's verdict on plan, returned by the reader start */
	struct drm_gem_object *held[2]; /* objects the reader may still fetch from */
};

/*
 * The connector routes through a placeholder encoder and the bridge stays on the host
 * encoder, because the CRTC enable and disable sequence the bridge themselves: a failed
 * bridge or MAC step must end a start and a failed stop must keep its resources, which
 * the void atomic bridge callbacks cannot express. No vblank is initialised, so the
 * atomic helpers fake every vblank event.
 */
struct sophgo_kms {
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

/* CEA-861 1280x720@60 (VIC 4), the one mode; the register words above derive from it. */
static const struct drm_display_mode sophgo_720p = {
	.type = DRM_MODE_TYPE_DRIVER,
	.clock = 74250,
	.hdisplay = 1280, .hsync_start = 1390, .hsync_end = 1430, .htotal = 1650,
	.vdisplay = 720, .vsync_start = 725, .vsync_end = 730, .vtotal = 750,
	/* The HDMI timing the bridge emits; DISP_POLARITY_720P is the host-to-MAC sync. */
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

/* cfg_reg (CLK_CFG_REG_VIP) gates direct register access; the syscon takes it per access. */
static const char *const sophgo_clock_names[SOPHGO_CLK_COUNT] = {
	"sc_top", "clk_disp", "clk_dsi", "dsi_esc", "disppll", "disp_src", "dsi_src", "cfg_reg",
};

static const u32 sophgo_kms_formats[] = { DRM_FORMAT_RGB888 };

static struct sophgo_dsi *host_to_dsi(struct mipi_dsi_host *host)
{
	return container_of(host, struct sophgo_dsi, host);
}

static struct sophgo_dsi *drm_to_dsi(struct drm_device *drm)
{
	return container_of(drm, struct sophgo_dsi, drm);
}

static void sophgo_rmw(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | (value & mask), reg);
}

/* DISP_CFG bit 16 is a commit command: never replay it from a readback. */
static void sophgo_disp_cfg_rmw(void __iomem *disp, u32 mask, u32 value)
{
	u32 old = readl(disp + DISP_CFG) & ~DISP_CFG_COMMIT;

	writel((old & ~mask) | (value & mask), disp + DISP_CFG);
}

static void sophgo_disp_shadow_begin(void __iomem *disp)
{
	sophgo_disp_cfg_rmw(disp, DISP_CFG_READ_WORKING, DISP_CFG_READ_WORKING);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_SHADOW_MASK, DISP_CFG_SHADOW_MASK);
}

static bool sophgo_rate_near(unsigned long got, unsigned long want)
{
	unsigned long delta = got > want ? got - want : want - got;

	return got && want && delta <= want / 1000000;
}

static bool sophgo_owned(struct sophgo_dsi *dsi)
{
	return dsi->pll_held || dsi->source_held || dsi->esc_held || dsi->clocks_on ||
	       dsi->bt_on || dsi->esc_set || dsi->div_set || dsi->phy_on || dsi->mmio_ready;
}

/* The first error of a start sticks until the next start clears the status. */
static void sophgo_fail(struct sophgo_dsi *dsi, int ret)
{
	if (ret && !dsi->status.error)
		dsi->status.error = ret;
}

/* MAC_EN is read-write-set (scaler.c:sclr_dsi_clr_mode): writing the mode back clears it. */
static int sophgo_mac_quiesce(struct sophgo_dsi *dsi)
{
	u32 val = readl(dsi->mac + MAC_EN);

	if ((val & 0xf) && (val & 0xf) != MAC_MODE_VIDEO)
		return -EBUSY;
	if (val & MAC_MODE_VIDEO)
		writel(val, dsi->mac + MAC_EN);
	return read_poll_timeout(readl, val, !(val & 0xf), 1000, 20000, false, dsi->mac + MAC_EN);
}

static int sophgo_mac_configure(struct sophgo_dsi *dsi)
{
	int ret = sophgo_mac_quiesce(dsi);

	if (ret)
		return ret;
	sophgo_rmw(dsi->mac + MAC_HS0, MAC_HS0_MASK, MAC_HS0_720P);
	sophgo_rmw(dsi->mac + MAC_HS1, MAC_HS1_MASK, MAC_HS1_720P);
	readl(dsi->mac + MAC_HS1);
	return 0;
}

/* vo_mipi_tx.c:mipi_tx_enable does not poll: a missing acknowledgment is only reported. */
static int sophgo_mac_start(struct sophgo_dsi *dsi)
{
	u32 val = readl(dsi->mac + MAC_EN);

	if (val & 0xf)
		return -EBUSY;
	/* 5 ms between the timing generator start and the HS request. */
	usleep_range(5000, 7000);
	writel(val | MAC_MODE_VIDEO, dsi->mac + MAC_EN);
	if (read_poll_timeout(readl, val, (val & 0xf) == MAC_MODE_VIDEO, 1000, 20000, false,
			      dsi->mac + MAC_EN))
		dev_warn(dsi->dev, "MAC did not acknowledge video within 20 ms: MAC_EN=%#x\n", val);
	return 0;
}

/*
 * vo_mipi_tx.c:mipi_tx_set_combo_dev_cfg -> dsi_phy.c: lanes off, map, PN, enable;
 * dphy_dsi_set_pll (after the VIP divider) and dphy_set_hs_settle; then dphy_init's
 * LP-11 writes, ending with dphy_dsi_analog_setting(false) in 0x74. The analogue words
 * are undocumented and reproduce the vendor writes.
 */
static void sophgo_dphy_program(struct sophgo_dsi *dsi)
{
	void __iomem *phy = dsi->phy;
	const u32 pads = DPHY_PADS_OFF_720P;

	sophgo_rmw(phy + DPHY_EN, 0x3f, 0);
	sophgo_rmw(phy + DPHY_MAP, 0xfffff, 0);
	readl(phy + DPHY_EN);
	sophgo_rmw(phy + DPHY_MAP, 0x1f077777, DPHY_MAP_720P);
	sophgo_rmw(phy + DPHY_PN, 0x1f, DPHY_PN_720P);
	sophgo_rmw(phy + DPHY_EN, 0x3f, DPHY_LANE_EN);

	sophgo_rmw(phy + DPHY_PLL, 0x300000, DPHY_LOOP_720P);
	sophgo_rmw(phy + DPHY_PLL, 0x7ff, DPHY_TXPLL_720P);
	writel(DPHY_SET_720P, phy + DPHY_SET);
	sophgo_rmw(phy + DPHY_UPDATE, 1, 0);
	sophgo_rmw(phy + DPHY_UPDATE, 1, 1);
	sophgo_rmw(phy + DPHY_HS, 0xffffff00, DPHY_HS_720P);

	/* dphy_init: the unused pads powered down. */
	writel(pads << 8 | pads, phy + 0x64);
	sophgo_rmw(phy + 0x4c, 0x001f001f, pads << 16 | pads);
	sophgo_rmw(phy + 0x54, 0x1f1f, pads << 8 | pads);
	sophgo_rmw(phy + 0x50, 0x1f1f1f1f, pads * 0x01010101U);
	sophgo_rmw(phy + 0xc4, 0x1f1f, pads << 8 | pads);
	sophgo_rmw(phy + 0xc8, 0x1f1f, pads << 8 | pads);
	writel(0x100, phy + 0x0c);
	writel(0x100, phy + 0x10);
	sophgo_rmw(phy + 0xc0, 0xfffff, 0);
	sophgo_rmw(phy + 0xb4, 1, 0);
	sophgo_rmw(phy + 0x60, 0x10000, 0);
	sophgo_rmw(phy + 0x5c, 0x1f000000, 0);
	sophgo_rmw(phy + 0x88, 0xfffff, 0);
	sophgo_rmw(phy + 0x74, 0x3ff, 0);
	/* scaler.c:sclr_disp_set_intf(MIPI): the parallel VO output is off. */
	sophgo_rmw(dsi->top + SC_TOP_VO_MUX, 0xf, 0);
	readl(dsi->top + SC_TOP_VO_MUX);
}

/* scaler.c:sclr_disp_set_intf(DISABLE) -> dsi_phy.c:dphy_init. */
static void sophgo_dphy_power_down(void __iomem *phy)
{
	sophgo_rmw(phy + DPHY_EN, 0x3f, 0);
	sophgo_rmw(phy + DPHY_MAP, 0xfffff, 0);
	sophgo_rmw(phy + 0x44, 0x1f1f1f1f, 0);
	sophgo_rmw(phy + 0x64, 0x1f1f, 0x1f1f);
	readl(phy + 0x64);
}

/*
 * scaler.c:sclr_ctrl_init order. The colour bars run until the reader's first latch
 * replaces them, before the HS request.
 */
static void sophgo_display_start(struct sophgo_dsi *dsi)
{
	void __iomem *top = dsi->top, *disp = dsi->disp;

	writel(SC_TOP_CFG0_INIT, top + SC_TOP_CFG0);
	writel(SC_TOP_CFG1_INIT, top + SC_TOP_CFG1);
	writel(SC_TOP_SHD_RAW_BANK, top + SC_TOP_SHD);
	sophgo_rmw(top + SC_TOP_CFG0, SC_TOP_REG_DONE, SC_TOP_REG_DONE);
	sophgo_rmw(top + SC_TOP_SHD, SC_TOP_FORCE_UP, SC_TOP_FORCE_UP);
	readl(top + SC_TOP_CFG0);

	sophgo_disp_shadow_begin(disp);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_INIT_MASK, DISP_CFG_INIT);
	sophgo_rmw(disp + DISP_CACHE, DISP_CACHE_MODE, DISP_CACHE_MODE);
	readl(disp + DISP_CFG);

	sophgo_disp_cfg_rmw(disp, DISP_CFG_TGEN, 0);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_POLARITY, DISP_POLARITY_720P);
	sophgo_rmw(disp + DISP_TOTAL, DISP_TIMING_MASK, DISP_TOTAL_720P);
	sophgo_rmw(disp + DISP_VSYNC, DISP_TIMING_MASK, DISP_VSYNC_720P);
	sophgo_rmw(disp + DISP_VFDE, DISP_TIMING_MASK, DISP_VACTIVE_720P);
	sophgo_rmw(disp + DISP_VMDE, DISP_TIMING_MASK, DISP_VACTIVE_720P);
	sophgo_rmw(disp + DISP_HSYNC, DISP_TIMING_MASK, DISP_HSYNC_720P);
	sophgo_rmw(disp + DISP_HFDE, DISP_TIMING_MASK, DISP_HACTIVE_720P);
	sophgo_rmw(disp + DISP_HMDE, DISP_TIMING_MASK, DISP_HACTIVE_720P);

	sophgo_rmw(disp + DISP_PAT_CFG, DISP_PAT_WINDOW_BG, 0);
	writel(0x03ff03ff, disp + DISP_PAT_COLOR0);
	sophgo_rmw(disp + DISP_PAT_COLOR1, 0x3ff, 0x3ff);
	sophgo_rmw(disp + DISP_PAT_CFG, DISP_PAT_MASK, DISP_PAT_COLORBAR);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_TGEN, DISP_CFG_TGEN);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_SHADOW_MASK, 0);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_COMMIT, DISP_CFG_COMMIT);
	readl(disp + DISP_CFG);
}

static void sophgo_display_stop(struct sophgo_dsi *dsi)
{
	void __iomem *disp = dsi->disp;

	sophgo_disp_shadow_begin(disp);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_TGEN, 0);
	sophgo_rmw(disp + DISP_PAT_CFG, DISP_PAT_WINDOW_BG, 0);
	sophgo_rmw(disp + DISP_PAT_CFG, DISP_PAT_OFF_MASK, 0);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_SHADOW_MASK, 0);
	sophgo_disp_cfg_rmw(disp, DISP_CFG_COMMIT, DISP_CFG_COMMIT);
	readl(disp + DISP_CFG);
}

/*
 * Reader updates are DISP shadow transactions (scaler.c:sclr_disp_reg_shadow_sel,
 * sclr_disp_reg_set_shadow_mask): the shadow bank is written under MASK_UP and released
 * without SW_UP, so the running TGEN latches the whole transaction at a frame end, and an
 * update counts only once the working bank reads back every expected word. The start's
 * latch moves the reader onto the framebuffer and turns the pattern generator off
 * together; a flip changes only ADDR0. A GEM object is held from before its address
 * reaches the reader until a latch moves the reader off it or a stop sees the fetch
 * quiesce. A failed latch leaves the reader on either buffer: both stay held, and flips
 * are refused until the next start.
 */
static void sophgo_disp_shadow_end(void __iomem *disp)
{
	sophgo_disp_cfg_rmw(disp, DISP_CFG_SHADOW_MASK, 0);
	readl(disp + DISP_CFG);
}

static void sophgo_pat_read(void __iomem *disp, unsigned int words[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++)
		words[i] = readl(disp + DISP_PAT_CFG + i * 4);
}

static void sophgo_reader_observe(void __iomem *disp, unsigned int *words)
{
	unsigned int i;

	for (i = 0; i < 10; i++)
		words[i] = readl(disp + SOPHGO_RD_ADDR0_L + i * 4);
	words[10] = readl(disp + SOPHGO_RD_IN_CSC0);
	sophgo_pat_read(disp, words + SOPHGO_RD_WORDS);
	words[SOPHGO_RD_FIFO] = readl(disp + SOPHGO_RD_FIFO_THR_REG);
}

static void sophgo_reader_apply(void __iomem *disp, const struct sophgo_reader_plan *p,
				unsigned int expected[SOPHGO_RD_LATCH])
{
	unsigned int i;

	memcpy(expected, p->word, sizeof(p->word));
	expected[6] |= readl(disp + SOPHGO_RD_PITCH_Y) & SOPHGO_RD_OS_MAX;
	sophgo_pat_read(disp, expected + SOPHGO_RD_WORDS);
	expected[SOPHGO_RD_WORDS] &= ~(DISP_PAT_OFF_MASK | DISP_PAT_WINDOW_BG);
	expected[SOPHGO_RD_FIFO] = SOPHGO_RD_FIFO_THR;
	sophgo_disp_shadow_begin(disp);
	sophgo_disp_cfg_rmw(disp, SOPHGO_RD_FMT_MASK, SOPHGO_RD_FMT_BGR << 12);
	for (i = 0; i < 10; i++)
		writel(expected[i], disp + SOPHGO_RD_ADDR0_L + i * 4);
	writel(expected[10], disp + SOPHGO_RD_IN_CSC0);
	writel(expected[SOPHGO_RD_FIFO], disp + SOPHGO_RD_FIFO_THR_REG);
	for (i = 1; i < 6; i++)
		writel(expected[SOPHGO_RD_WORDS + i], disp + DISP_PAT_CFG + i * 4);
	writel(expected[SOPHGO_RD_WORDS], disp + DISP_PAT_CFG);
	sophgo_disp_shadow_end(disp);
}

/* Plans even a refused plane state: the flip compares its address. */
static int sophgo_plan(const struct drm_plane_state *ps, struct sophgo_reader_plan *p)
{
	struct drm_framebuffer *fb = ps->fb;
	struct drm_gem_dma_object *obj = fb ? drm_fb_dma_get_gem_obj(fb, 0) : NULL;
	unsigned int pitch = fb ? fb->pitches[0] : 0;
	unsigned long long dma = 0, bytes = 0, need;

	if (obj && obj->base.size > fb->offsets[0]) {
		dma = obj->dma_addr + fb->offsets[0];
		bytes = obj->base.size - fb->offsets[0];
	}
	*p = (struct sophgo_reader_plan) { .gem = obj ? &obj->base : NULL };
	p->word[0] = (unsigned int)dma;
	p->word[1] = (unsigned int)(dma >> 32);
	p->word[6] = SOPHGO_RD_BURST << 28 | pitch;
	p->word[9] = (SOPHGO_SO_HEIGHT - 1U) << 16 | (SOPHGO_SO_WIDTH - 1U);
	if (!fb || ((ps->src_x | ps->src_y | ps->src_w | ps->src_h) & 0xffff) || ps->src_x ||
	    ps->src_y || ps->src_w != SOPHGO_SO_WIDTH << 16 || ps->src_h != SOPHGO_SO_HEIGHT << 16)
		return -EINVAL;
	if (pitch < SOPHGO_SO_WIDTH * 3U || pitch % SOPHGO_RD_ALIGN || pitch > SOPHGO_SO_PITCH_MAX)
		return -ERANGE;
	need = (unsigned long long)pitch * (SOPHGO_SO_HEIGHT - 1U) + SOPHGO_SO_WIDTH * 3U;
	if (bytes < need)
		return -ENOSPC;
	if (!dma || dma % SOPHGO_RD_ALIGN || dma >= SOPHGO_RD_ADDR_END ||
	    SOPHGO_RD_ADDR_END - dma < need)
		return -EFAULT;
	return 0;
}

static void sophgo_flip_apply(void __iomem *disp, const struct sophgo_reader_plan *p,
			      unsigned int expected[SOPHGO_RD_LATCH])
{
	sophgo_reader_observe(disp, expected);
	expected[0] = p->word[0];
	expected[1] = p->word[1];
	sophgo_disp_shadow_begin(disp);
	writel(expected[0], disp + SOPHGO_RD_ADDR0_L);
	writel(expected[1], disp + SOPHGO_RD_ADDR0_L + 4);
	sophgo_disp_shadow_end(disp);
}

static bool sophgo_reader_latched(void __iomem *disp, const unsigned int *expected)
{
	unsigned int observed[SOPHGO_RD_LATCH];

	sophgo_reader_observe(disp, observed);
	return !memcmp(observed, expected, sizeof(observed));
}

static int sophgo_reader_latch(struct sophgo_dsi *dsi, const unsigned int expected[SOPHGO_RD_LATCH])
{
	bool latched;
	int ret = read_poll_timeout(sophgo_reader_latched, latched, latched, 1000, 100000, true,
				    dsi->disp, expected);

	if (ret)
		dsi->latch_failed = true;
	return ret;
}

static bool sophgo_reader_hold(struct sophgo_dsi *dsi, struct drm_gem_object *gem)
{
	unsigned int i;

	if (!gem)
		return true;
	for (i = 0; i < ARRAY_SIZE(dsi->held); i++)
		if (dsi->held[i] == gem)
			return true;
	for (i = 0; i < ARRAY_SIZE(dsi->held); i++)
		if (!dsi->held[i]) {
			drm_gem_object_get(gem);
			dsi->held[i] = gem;
			return true;
		}
	return false;
}

static void sophgo_reader_drop(struct sophgo_dsi *dsi, struct drm_gem_object *keep)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dsi->held); i++)
		if (dsi->held[i] && dsi->held[i] != keep) {
			drm_gem_object_put(dsi->held[i]);
			dsi->held[i] = NULL;
		}
}

/* The AXI clock is on before any address reaches the reader, until sophgo_reader_release(). */
static int sophgo_reader_start(struct sophgo_dsi *dsi)
{
	unsigned int expected[SOPHGO_RD_LATCH];
	int ret;

	dsi->latch_failed = false;
	dsi->reader_applied = false;
	if (dsi->plan_refused)
		return dsi->plan_refused;
	if (!sophgo_reader_hold(dsi, dsi->plan.gem))
		return -EBUSY;
	ret = clk_prepare_enable(dsi->reader_axi);
	if (ret)
		return ret;
	dsi->reader_axi_on = true;
	/*
	 * vo.c:vo_start_streaming: TOP_CFG1 disp_enable (4) on, disp_from_sc (8) off, through
	 * sclr_top_set_cfg with reg_done and force_up, before the reader's transaction, so the
	 * forced update latches only the pattern generator's words.
	 */
	sophgo_rmw(dsi->top + SC_TOP_CFG1, 0x110, 0x10);
	sophgo_rmw(dsi->top + SC_TOP_CFG0, SC_TOP_REG_DONE, SC_TOP_REG_DONE);
	sophgo_rmw(dsi->top + SC_TOP_SHD, SC_TOP_FORCE_UP, SC_TOP_FORCE_UP);
	readl(dsi->top + SC_TOP_CFG1);
	sophgo_reader_apply(dsi->disp, &dsi->plan, expected);
	ret = sophgo_reader_latch(dsi, expected);
	if (ret)
		return ret;
	dsi->reader_applied = true;
	return 0;
}

static void sophgo_reader_release(struct sophgo_dsi *dsi)
{
	dsi->reader_applied = false;
	if (dsi->reader_axi_on) {
		clk_disable_unprepare(dsi->reader_axi);
		dsi->reader_axi_on = false;
	}
	sophgo_reader_drop(dsi, NULL);
}

/*
 * Fetch stop. The DRAM reader is idle (DISP_AXI_ST bit 0) only between frames while TGEN
 * runs; TGEN stopped mid-fetch leaves it busy until TGEN runs again. vo.c:vo_stop_streaming
 * first turns the scaler-top disp_enable off, without which AXI_IDLE never returns once
 * TGEN stops, then TGEN with sclr_disp_tgen_enable(false), a bare read-modify-write of
 * DISP_CFG bit 7. So does this stop, after the MAC stop and the bridge disable: a cut
 * samples AXI_ST and clears TGEN in the same interrupts-off section as the first idle
 * sample, inside that frame end's blank, then needs TGEN off and the reader idle for 2 ms
 * within 25 ms. A reader left busy gets TGEN back on with the same write, as the vendor
 * does around timing changes, drains to a frame end and is cut again, three cuts at most;
 * every path leaves TGEN off. The quiesce then gates every release: a reader still busy
 * keeps every resource and held buffer, and the stop fails for a retry.
 */
static void sophgo_fetch_cut(void __iomem *disp)
{
	u64 start = ktime_get_ns();
	unsigned long flags;
	bool idle, last;

	for (;;) {
		last = ktime_get_ns() - start >= SOPHGO_FETCH_WAIT_NS;
		local_irq_save(flags);
		idle = readl(disp + DISP_AXI_ST) & DISP_AXI_IDLE;
		if (idle || last) {
			sophgo_disp_cfg_rmw(disp, DISP_CFG_TGEN, 0);
			/* Post the TGEN-off write before interrupts return. */
			readl(disp + DISP_CFG);
		}
		local_irq_restore(flags);
		if (idle || last)
			return;
		usleep_range(10, 20);
	}
}

static bool sophgo_reader_idle(void __iomem *disp)
{
	u32 cfg = readl(disp + DISP_CFG), axi = readl(disp + DISP_AXI_ST);

	return !(cfg & DISP_CFG_TGEN) && (axi & DISP_AXI_IDLE);
}

static bool sophgo_fetch_settled(void __iomem *disp)
{
	u64 start = ktime_get_ns(), since = 0, now;
	bool run = false, idle;

	for (;;) {
		idle = sophgo_reader_idle(disp);
		now = ktime_get_ns();
		if (!idle) {
			run = false;
		} else if (!run) {
			run = true;
			since = now;
		} else if (now - since >= SOPHGO_FETCH_HOLD_NS) {
			return true;
		}
		if (now - start >= SOPHGO_FETCH_SETTLE_NS)
			return false;
		usleep_range(50, 100);
	}
}

static void sophgo_fetch_stop(void __iomem *disp)
{
	unsigned int attempt;

	if (!(readl(disp + DISP_CFG) & DISP_CFG_TGEN) &&
	    (readl(disp + DISP_AXI_ST) & DISP_AXI_IDLE))
		return;
	for (attempt = 0; attempt < SOPHGO_FETCH_TRIES; attempt++) {
		if (!(readl(disp + DISP_CFG) & DISP_CFG_TGEN))
			sophgo_disp_cfg_rmw(disp, DISP_CFG_TGEN, DISP_CFG_TGEN);
		sophgo_fetch_cut(disp);
		if (sophgo_fetch_settled(disp))
			return;
	}
}

static void sophgo_reader_input_off(struct sophgo_dsi *dsi)
{
	if (!dsi->reader_axi_on)
		return;
	sophgo_rmw(dsi->top + SC_TOP_CFG1, 0x10, 0);
	sophgo_rmw(dsi->top + SC_TOP_CFG0, SC_TOP_REG_DONE, SC_TOP_REG_DONE);
	sophgo_rmw(dsi->top + SC_TOP_SHD, SC_TOP_FORCE_UP, SC_TOP_FORCE_UP);
	readl(dsi->top + SC_TOP_CFG1);
	sophgo_fetch_stop(dsi->disp);
}

static int sophgo_reader_quiesce(struct sophgo_dsi *dsi)
{
	bool idle;

	if (!dsi->reader_axi_on)
		return 0;
	return read_poll_timeout(sophgo_reader_idle, idle, idle, 1000, 100000, false, dsi->disp);
}

/* DISPPLL reads zero until it locks, so a nonzero rate is the lock; escape within 1 ppm. */
static int sophgo_clocks_ready(struct sophgo_dsi *dsi)
{
	struct clk *esc = dsi->clocks[SOPHGO_CLK_DSI_ESC].clk;
	unsigned long rate;
	int ret;

	ret = read_poll_timeout(clk_get_rate, rate, sophgo_rate_near(rate, dsi->pll_hz), 100,
				200000, false, dsi->clocks[SOPHGO_CLK_DISPPLL].clk);
	if (ret)
		return ret;
	if (clk_get_rate(dsi->clocks[SOPHGO_CLK_DSI_SRC].clk) != SOPHGO_DSI_SRC_HZ ||
	    !sophgo_rate_near(clk_get_rate(esc), SOPHGO_ESC_HZ) ||
	    !clk_is_match(clk_get_parent(esc), dsi->esc_parent))
		return -ERANGE;
	return 0;
}

/*
 * Start and stop. The start follows vo_mipi_tx.c: display clocks before the DSI ones and
 * no local reset (asserted display resets are refused), then LP-11 pads, TGEN running and
 * the MAC idle before the bridge's pre_enable, and the HS request before its video enable;
 * the bridge's void callbacks report through the shared status. Each acquired resource
 * sets an ownership flag, and mmio_ready qualifies direct MMIO. The stop runs from any
 * partial state and lets the reader's AXI clock go last. A step that cannot complete (a
 * quiesce timeout, a failed restore, rails the bridge still owns) keeps what it holds and
 * fails the stop: the pipeline stays owned, enables return -EBUSY, and detach or shutdown
 * retries it.
 */
static int sophgo_prepare(struct sophgo_dsi *dsi)
{
	struct clk *esc = dsi->clocks[SOPHGO_CLK_DSI_ESC].clk;
	unsigned int val;
	long rounded;
	int ret;

	/* Rate holds keep other consumers off the shared PLL and source rates. */
	ret = clk_rate_exclusive_get(dsi->clocks[SOPHGO_CLK_DISPPLL].clk);
	if (ret)
		return ret;
	dsi->pll_held = true;
	ret = clk_rate_exclusive_get(dsi->clocks[SOPHGO_CLK_DSI_SRC].clk);
	if (ret)
		return ret;
	dsi->source_held = true;
	ret = clk_bulk_prepare_enable(SOPHGO_CLK_COUNT, dsi->clocks);
	if (ret)
		return ret;
	dsi->clocks_on = true;
	ret = clk_prepare_enable(dsi->bt);
	if (ret)
		return ret;
	dsi->bt_on = true;
	ret = read_poll_timeout(clk_get_rate, dsi->pll_hz, dsi->pll_hz, 100, 200000, false,
				dsi->clocks[SOPHGO_CLK_DISPPLL].clk);
	if (ret)
		return ret;
	if (clk_get_rate(dsi->clocks[SOPHGO_CLK_DSI_SRC].clk) != SOPHGO_DSI_SRC_HZ)
		return -ERANGE;
	ret = regmap_read(dsi->vip, VIP_RESET, &val);
	if (ret)
		return ret;
	if (val & VIP_DISPLAY_RESETS)
		return -EBUSY;
	dsi->mmio_ready = true;
	/* A firmware-preselected MAC must be idle before its PHY changes. */
	ret = sophgo_mac_quiesce(dsi);
	if (ret)
		return ret;

	/* Stop a timing generator the firmware left running. */
	sophgo_display_stop(dsi);

	ret = clk_rate_exclusive_get(esc);
	if (ret)
		return ret;
	dsi->esc_held = true;
	dsi->esc_old_parent = clk_get_parent(esc);
	dsi->esc_old_rate = clk_get_rate(esc);
	if (!dsi->esc_old_parent || !dsi->esc_old_rate)
		return -ERANGE;
	dsi->esc_set = true;
	ret = clk_set_parent(esc, dsi->esc_parent);
	if (ret)
		return ret;
	rounded = clk_round_rate(esc, SOPHGO_ESC_HZ);
	if (rounded < 0)
		return rounded;
	if (!sophgo_rate_near(rounded, SOPHGO_ESC_HZ))
		return -ERANGE;
	ret = clk_set_rate(esc, rounded);
	if (ret)
		return ret;
	ret = sophgo_clocks_ready(dsi);
	if (ret)
		return ret;

	/* dsi_phy.c:_cal_pll_reg: disp_sel_bt_div1 only, read back before the first PHY access. */
	ret = regmap_read(dsi->vip, VIP_CLK_CTRL0, &dsi->div_old);
	if (ret)
		return ret;
	dsi->div_old &= VIP_DISP_SEL_BT_DIV1;
	dsi->div_set = true;
	ret = regmap_update_bits(dsi->vip, VIP_CLK_CTRL0, VIP_DISP_SEL_BT_DIV1,
				 VIP_DISP_SEL_BT_DIV1);
	if (ret)
		return ret;
	ret = regmap_read(dsi->vip, VIP_CLK_CTRL0, &val);
	if (ret)
		return ret;
	dsi->phy_on = true;
	sophgo_dphy_program(dsi);
	return sophgo_mac_configure(dsi);
}

static int sophgo_release(struct sophgo_dsi *dsi)
{
	struct clk *esc = dsi->clocks[SOPHGO_CLK_DSI_ESC].clk;
	unsigned int val;
	int ret;

	if (dsi->phy_on) {
		sophgo_dphy_power_down(dsi->phy);
		dsi->phy_on = false;
	}
	if (dsi->esc_set) {
		ret = clk_set_parent(esc, dsi->esc_old_parent);
		if (ret)
			return ret;
		ret = clk_set_rate(esc, dsi->esc_old_rate);
		if (ret)
			return ret;
		if (!clk_is_match(clk_get_parent(esc), dsi->esc_old_parent) ||
		    clk_get_rate(esc) != dsi->esc_old_rate)
			return -ERANGE;
		dsi->esc_set = false;
	}
	if (dsi->esc_held) {
		clk_rate_exclusive_put(esc);
		dsi->esc_held = false;
	}
	if (dsi->div_set) {
		ret = regmap_update_bits(dsi->vip, VIP_CLK_CTRL0, VIP_DISP_SEL_BT_DIV1,
					 dsi->div_old);
		if (ret)
			return ret;
		ret = regmap_read(dsi->vip, VIP_CLK_CTRL0, &val);
		if (ret)
			return ret;
		if ((val & VIP_DISP_SEL_BT_DIV1) != dsi->div_old)
			return -EIO;
		dsi->div_set = false;
	}
	if (dsi->bt_on) {
		clk_disable_unprepare(dsi->bt);
		dsi->bt_on = false;
	}
	if (dsi->clocks_on) {
		clk_bulk_disable_unprepare(SOPHGO_CLK_COUNT, dsi->clocks);
		dsi->clocks_on = false;
	}
	if (dsi->pll_held) {
		clk_rate_exclusive_put(dsi->clocks[SOPHGO_CLK_DISPPLL].clk);
		dsi->pll_held = false;
	}
	if (dsi->source_held) {
		clk_rate_exclusive_put(dsi->clocks[SOPHGO_CLK_DSI_SRC].clk);
		dsi->source_held = false;
	}
	return 0;
}

static int sophgo_stop(struct sophgo_dsi *dsi)
{
	int ret;

	lockdep_assert_held(&dsi->lock);
	drm_atomic_bridge_chain_disable(dsi->bridge, NULL);
	if (dsi->mmio_ready) {
		ret = sophgo_mac_quiesce(dsi);
		sophgo_fail(dsi, ret);
		if (ret)
			return ret;
		sophgo_reader_input_off(dsi);
		sophgo_display_stop(dsi);
		ret = sophgo_reader_quiesce(dsi);
		sophgo_fail(dsi, ret);
		if (ret)
			return ret;
		dsi->mmio_ready = false;
	}
	drm_atomic_bridge_chain_post_disable(dsi->bridge, NULL);
	if (dsi->status.supplies_owned)
		return dsi->status.error ?: -EBUSY;
	ret = sophgo_release(dsi);
	sophgo_fail(dsi, ret);
	if (ret)
		return ret;
	sophgo_reader_release(dsi);
	return 0;
}

static int sophgo_start(struct sophgo_dsi *dsi)
{
	int ret;

	lockdep_assert_held(&dsi->lock);
	if (sophgo_owned(dsi))
		return -EBUSY;
	memset(&dsi->status, 0, sizeof(dsi->status));
	ret = sophgo_prepare(dsi);
	if (ret)
		goto fail;
	sophgo_display_start(dsi);
	ret = sophgo_reader_start(dsi);
	if (ret)
		goto fail;
	drm_bridge_chain_mode_set(dsi->bridge, &sophgo_720p, &sophgo_720p);
	drm_atomic_bridge_chain_pre_enable(dsi->bridge, NULL);
	if (dsi->status.error)
		goto fail;
	ret = sophgo_clocks_ready(dsi);
	if (ret)
		goto fail;
	ret = sophgo_mac_start(dsi);
	if (ret)
		goto fail;
	drm_atomic_bridge_chain_enable(dsi->bridge, NULL);
	if (!dsi->status.error && !dsi->status.started)
		ret = -EIO;
	if (ret || dsi->status.error)
		goto fail;
	dev_info(dsi->dev, "scanout started\n");
	return 0;
fail:
	sophgo_fail(dsi, ret);
	sophgo_stop(dsi);
	return dsi->status.error;
}

static int sophgo_kms_plane_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs;
	struct sophgo_reader_plan plan;
	int ret;

	if (!ps->crtc)
		return 0;
	cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	ret = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	if (ret || !ps->visible)
		return ret;
	return sophgo_plan(ps, &plan);
}

static int sophgo_kms_flip(struct sophgo_dsi *dsi, const struct drm_plane_state *ps)
{
	unsigned int expected[SOPHGO_RD_LATCH];
	struct sophgo_reader_plan plan;
	int ret;

	lockdep_assert_held(&dsi->lock);
	ret = sophgo_plan(ps, &plan);
	if (sophgo_owned(dsi) && plan.word[0] == dsi->plan.word[0] &&
	    plan.word[1] == dsi->plan.word[1])
		return 0;
	if (!dsi->reader_applied || dsi->latch_failed)
		ret = -EBUSY;
	else if (!ret && plan.word[6] != dsi->plan.word[6])
		ret = -EOPNOTSUPP;
	if (!ret && !sophgo_reader_hold(dsi, plan.gem))
		ret = -EBUSY;
	if (!ret) {
		sophgo_flip_apply(dsi->disp, &plan, expected);
		ret = sophgo_reader_latch(dsi, expected);
	}
	if (!ret) {
		dsi->plan = plan;
		sophgo_reader_drop(dsi, plan.gem);
	}
	return ret;
}

/* A modeset latches the buffer through the CRTC enable instead. */
static void sophgo_kms_plane_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct sophgo_dsi *dsi = drm_to_dsi(plane->dev);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs;
	int ret;

	if (!ps->crtc || !ps->fb)
		return;
	cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	if (!cs || !cs->active || drm_atomic_crtc_needs_modeset(cs))
		return;
	mutex_lock(&dsi->lock);
	ret = sophgo_kms_flip(dsi, ps);
	mutex_unlock(&dsi->lock);
	if (ret)
		drm_err(plane->dev, "flip failed: %d\n", ret);
}

static int sophgo_kms_crtc_check(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct sophgo_dsi *dsi = drm_to_dsi(crtc->dev);
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_crtc_state *old = drm_atomic_get_old_crtc_state(state, crtc);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, crtc->primary);
	struct drm_plane_state *ops = drm_atomic_get_old_plane_state(state, crtc->primary);
	bool modeset = drm_atomic_crtc_needs_modeset(cs);
	struct sophgo_reader_plan plan;
	int ret = 0;

	/* A flip changes only the reader address; a pitch change needs a modeset. */
	if (old->active && cs->active && !modeset && ps && ops && ps->fb != ops->fb) {
		ret = sophgo_plan(ps, &plan);
		if (!ret && (!ops->fb || ps->fb->pitches[0] != ops->fb->pitches[0]))
			ret = -EOPNOTSUPP;
	}
	if (ret || !cs->enable)
		return ret;
	if (!drm_mode_equal(&cs->mode, &sophgo_720p) ||
	    !(cs->plane_mask & drm_plane_mask(crtc->primary)))
		return -EINVAL;
	if (!cs->active || !modeset)
		return 0;
	/* Early refusal only: the enable rechecks under the same lock. */
	mutex_lock(&dsi->lock);
	if (!dsi->peripheral)
		ret = -EBUSY;
	mutex_unlock(&dsi->lock);
	return ret;
}

static void sophgo_kms_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct sophgo_dsi *dsi = drm_to_dsi(crtc->dev);
	int ret = -EBUSY;

	mutex_lock(&dsi->lock);
	if (dsi->peripheral && !sophgo_owned(dsi)) {
		/* This commit's primary state; an off-screen one skipped the plane check. */
		dsi->plan_refused = sophgo_plan(crtc->primary->state, &dsi->plan);
		ret = sophgo_start(dsi);
	}
	mutex_unlock(&dsi->lock);
	if (ret)
		drm_err(crtc->dev, "scanout start failed: %d\n", ret);
}

static void sophgo_kms_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct sophgo_dsi *dsi = drm_to_dsi(crtc->dev);
	int ret = 0;

	mutex_lock(&dsi->lock);
	if (sophgo_owned(dsi))
		ret = sophgo_stop(dsi);
	mutex_unlock(&dsi->lock);
	if (ret)
		drm_err(crtc->dev, "scanout stop failed, pipeline held: %d\n", ret);
}

static int sophgo_kms_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &sophgo_720p);
}

static const struct drm_plane_funcs sophgo_kms_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct drm_plane_helper_funcs sophgo_kms_plane_helpers = {
	.atomic_check = sophgo_kms_plane_check,
	.atomic_update = sophgo_kms_plane_update,
};

static const struct drm_crtc_funcs sophgo_kms_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs sophgo_kms_crtc_helpers = {
	.atomic_check = sophgo_kms_crtc_check,
	.atomic_enable = sophgo_kms_crtc_enable,
	.atomic_disable = sophgo_kms_crtc_disable,
};

static const struct drm_connector_funcs sophgo_kms_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs sophgo_kms_connector_helpers = {
	.get_modes = sophgo_kms_get_modes,
};

static const struct drm_mode_config_funcs sophgo_kms_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(sophgo_kms_fops);

static const struct drm_driver sophgo_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &sophgo_kms_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name = "sophgo-dsi",
	.desc = "Sophgo SG2000 MIPI DSI host",
	.major = 1,
};

/* The host encoder gets the CRTC only for validation. */
static int sophgo_kms_init(struct sophgo_dsi *dsi)
{
	struct drm_device *drm = &dsi->drm;
	struct sophgo_kms *kms;
	int ret;

	drm->mode_config.min_width = SOPHGO_SO_WIDTH;
	drm->mode_config.min_height = SOPHGO_SO_HEIGHT;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &sophgo_kms_mode_config_funcs;
	kms = drmm_universal_plane_alloc(drm, struct sophgo_kms, plane, 1, &sophgo_kms_plane_funcs,
					 sophgo_kms_formats, ARRAY_SIZE(sophgo_kms_formats), NULL,
					 DRM_PLANE_TYPE_PRIMARY, NULL);
	if (IS_ERR(kms))
		return PTR_ERR(kms);
	drm_plane_helper_add(&kms->plane, &sophgo_kms_plane_helpers);
	ret = drmm_crtc_init_with_planes(drm, &kms->crtc, &kms->plane, NULL,
					 &sophgo_kms_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&kms->crtc, &sophgo_kms_crtc_helpers);
	ret = drmm_encoder_init(drm, &kms->encoder, NULL, DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	kms->encoder.possible_crtcs = drm_crtc_mask(&kms->crtc);
	dsi->encoder.possible_crtcs = kms->encoder.possible_crtcs;
	ret = drmm_connector_init(drm, &kms->connector, &sophgo_kms_connector_funcs,
				  DRM_MODE_CONNECTOR_HDMIA, NULL);
	if (ret)
		return ret;
	drm_connector_helper_add(&kms->connector, &sophgo_kms_connector_helpers);
	ret = drm_connector_attach_encoder(&kms->connector, &kms->encoder);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	return 0;
}

/*
 * Objects are built under the lifecycle mutex; registration and the fbdev client run
 * outside it, because the client's modeset enters the CRTC enable.
 */
static void sophgo_kms_register_work(struct work_struct *work)
{
	struct sophgo_dsi *dsi = container_of(work, struct sophgo_dsi, kms_work);
	int ret;

	mutex_lock(&dsi->lock);
	/* Objects may be half built: a failed bind never registers. */
	ret = sophgo_kms_init(dsi);
	dsi->kms_state = ret ? SOPHGO_KMS_DONE : SOPHGO_KMS_REGISTERED;
	mutex_unlock(&dsi->lock);
	if (!ret) {
		ret = drm_dev_register(&dsi->drm, 0);
		if (ret) {
			mutex_lock(&dsi->lock);
			dsi->kms_state = SOPHGO_KMS_DONE;
			mutex_unlock(&dsi->lock);
		} else {
			drm_client_setup_with_fourcc(&dsi->drm, DRM_FORMAT_RGB888);
		}
	}
	if (ret)
		dev_warn(dsi->dev, "DRM registration at attach failed: %d\n", ret);
	else
		dev_info(dsi->dev, "DRM device and fbdev registered at attach\n");
}

/*
 * Called without the lifecycle mutex: the shutdown commit's CRTC disable takes it.
 * cancel_work_sync() drops a queued registration and waits out a running one, and nothing
 * queues another: attach queues only as it publishes the peripheral that detach is
 * removing, and host remove runs after the bridge, the device link's consumer, unbinds.
 */
static void sophgo_kms_unplug(struct sophgo_dsi *dsi)
{
	cancel_work_sync(&dsi->kms_work);
	if (dsi->kms_state == SOPHGO_KMS_REGISTERED) {
		drm_dev_unplug(&dsi->drm);
		drm_atomic_helper_shutdown(&dsi->drm);
		mutex_lock(&dsi->lock);
		dsi->kms_state = SOPHGO_KMS_DONE;
		mutex_unlock(&dsi->lock);
	}
}

static const struct drm_encoder_funcs sophgo_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/*
 * The bridge attaches from its own probe, after drm_bridge_add(). lontium-lt8912b.c
 * registers "lt8912" with RGB888, VIDEO | LPM | NO_HFP | NO_EOT_PACKET and its DT
 * data-lanes; only that peripheral, on two lanes, is accepted. It sends no commands.
 */
static int sophgo_dsi_attach(struct mipi_dsi_host *host, struct mipi_dsi_device *device)
{
	struct sophgo_dsi *dsi = host_to_dsi(host);
	struct i2c_client *client;
	struct device_link *link;
	int ret;

	if (strcmp(device->name, "lt8912") || device->format != MIPI_DSI_FMT_RGB888 ||
	    !(device->mode_flags & MIPI_DSI_MODE_VIDEO))
		return -EINVAL;
	if (device->lanes != 2) {
		dev_err(dsi->dev, "LT8912B attached with %u data lanes, only 2 are supported\n",
			device->lanes);
		return -EINVAL;
	}
	mutex_lock(&dsi->lock);
	if (dsi->peripheral) {
		ret = -EBUSY;
		goto unlock;
	}
	dsi->bridge = of_drm_find_and_get_bridge(dsi->bridge_node);
	if (!dsi->bridge) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	/* The bridge must unbind first: its devres owns a DSI device on this host. */
	client = of_find_i2c_device_by_node(dsi->bridge_node);
	if (!client) {
		ret = -EPROBE_DEFER;
		goto put_bridge;
	}
	link = device_link_add(&client->dev, dsi->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&client->dev);
	if (!link) {
		ret = -ENOMEM;
		goto put_bridge;
	}
	ret = drm_encoder_init(&dsi->drm, &dsi->encoder, &sophgo_encoder_funcs,
			       DRM_MODE_ENCODER_DSI, NULL);
	if (ret)
		goto unlink;
	ret = drm_bridge_attach(&dsi->encoder, dsi->bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret) {
		drm_encoder_cleanup(&dsi->encoder);
		goto unlink;
	}
	lt8912b_pipeline_bind(dsi->bridge, &dsi->status);
	dsi->peripheral = device;
	dev_info(dsi->dev, "LT8912B attached with %u data lanes\n", device->lanes);
	if (dsi->kms_state == SOPHGO_KMS_OFF)
		schedule_work(&dsi->kms_work);
	goto unlock;
unlink:
	device_link_del(link);
put_bridge:
	drm_bridge_put(dsi->bridge);
	dsi->bridge = NULL;
unlock:
	mutex_unlock(&dsi->lock);
	return ret;
}

static int sophgo_dsi_detach(struct mipi_dsi_host *host, struct mipi_dsi_device *device)
{
	struct sophgo_dsi *dsi = host_to_dsi(host);

	if (dsi->peripheral == device)
		sophgo_kms_unplug(dsi);
	mutex_lock(&dsi->lock);
	if (dsi->peripheral != device) {
		mutex_unlock(&dsi->lock);
		return -EINVAL;
	}
	/* A failed stop keeps its resources; the departing bridge is let go regardless. */
	if (sophgo_owned(dsi) && sophgo_stop(dsi))
		dev_err(dsi->dev, "display pipeline still owned after a failed stop\n");
	lt8912b_pipeline_bind(dsi->bridge, NULL);
	drm_encoder_cleanup(&dsi->encoder);
	drm_bridge_put(dsi->bridge);
	dsi->bridge = NULL;
	dsi->peripheral = NULL;
	mutex_unlock(&dsi->lock);
	return 0;
}

static const struct mipi_dsi_host_ops sophgo_dsi_host_ops = {
	.attach = sophgo_dsi_attach,
	.detach = sophgo_dsi_detach,
};

static int sophgo_dsi_probe(struct platform_device *pdev)
{
	static const struct {
		const char *name;
		resource_size_t size;
	} windows[] = { { "sc_top", 0x74 }, { "disp", 0xc4 },
			{ "dsi_mac", 0x0c }, { "dphy", 0xcc } };
	struct device *dev = &pdev->dev;
	struct sophgo_dsi *dsi;
	void __iomem **maps[4];
	unsigned int i;
	int ret;

	dsi = devm_drm_dev_alloc(dev, &sophgo_drm_driver, struct sophgo_dsi, drm);
	if (IS_ERR(dsi))
		return PTR_ERR(dsi);
	dsi->dev = dev;
	mutex_init(&dsi->lock);
	platform_set_drvdata(pdev, dsi);
	ret = drmm_mode_config_init(&dsi->drm);
	if (ret)
		return ret;

	maps[0] = &dsi->top;
	maps[1] = &dsi->disp;
	maps[2] = &dsi->mac;
	maps[3] = &dsi->phy;
	for (i = 0; i < ARRAY_SIZE(windows); i++) {
		struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
								   windows[i].name);

		if (!res || resource_size(res) < windows[i].size)
			return dev_err_probe(dev, -EINVAL, "%s window missing or short\n",
					     windows[i].name);
		*maps[i] = devm_ioremap_resource(dev, res);
		if (IS_ERR(*maps[i]))
			return PTR_ERR(*maps[i]);
	}
	dsi->vip = syscon_regmap_lookup_by_phandle(dev->of_node, "sophgo,vip-sys");
	if (IS_ERR(dsi->vip))
		return dev_err_probe(dev, PTR_ERR(dsi->vip), "VIP syscon unavailable\n");
	for (i = 0; i < SOPHGO_CLK_COUNT; i++)
		dsi->clocks[i].id = sophgo_clock_names[i];
	ret = devm_clk_bulk_get(dev, SOPHGO_CLK_COUNT, dsi->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "display clocks unavailable\n");
	dsi->esc_parent = devm_clk_get(dev, "esc_parent");
	if (IS_ERR(dsi->esc_parent))
		return PTR_ERR(dsi->esc_parent);
	/* The vendor mipi_tx node's clk_bt; unheld, clk_disable_unused turns it off. */
	dsi->bt = devm_clk_get(dev, "bt_vip");
	if (IS_ERR(dsi->bt))
		return dev_err_probe(dev, PTR_ERR(dsi->bt), "bt_vip clock unavailable\n");
	INIT_WORK(&dsi->kms_work, sophgo_kms_register_work);
	/* It feeds the DISP DRAM master; the vendor relies on its vi driver holding it. */
	dsi->reader_axi = devm_clk_get(dev, "axi_vip");
	if (IS_ERR(dsi->reader_axi))
		return dev_err_probe(dev, PTR_ERR(dsi->reader_axi), "reader AXI clock\n");
	dsi->bridge_node = of_graph_get_remote_node(dev->of_node, 0, -1);
	if (!dsi->bridge_node)
		return dev_err_probe(dev, -ENODEV, "no bridge on port 0\n");
	dsi->host.dev = dev;
	dsi->host.ops = &sophgo_dsi_host_ops;
	ret = mipi_dsi_host_register(&dsi->host);
	if (ret)
		of_node_put(dsi->bridge_node);
	return ret;
}

static void sophgo_dsi_remove(struct platform_device *pdev)
{
	struct sophgo_dsi *dsi = platform_get_drvdata(pdev);

	sophgo_kms_unplug(dsi);
	mipi_dsi_host_unregister(&dsi->host);
	of_node_put(dsi->bridge_node);
}

static void sophgo_dsi_shutdown(struct platform_device *pdev)
{
	struct sophgo_dsi *dsi = platform_get_drvdata(pdev);

	mutex_lock(&dsi->lock);
	if (dsi->peripheral && sophgo_owned(dsi) && sophgo_stop(dsi))
		dev_err(dsi->dev, "display pipeline still owned after a failed stop\n");
	mutex_unlock(&dsi->lock);
}

/* Refuse system sleep: nothing saves or restores the display pipeline across a suspend. */
static int sophgo_dsi_suspend(struct device *dev)
{
	return -EOPNOTSUPP;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sophgo_dsi_pm, sophgo_dsi_suspend, NULL);

static const struct of_device_id sophgo_dsi_of_match[] = {
	{ .compatible = "sophgo,sg2000-dsi" },
	{}
};
MODULE_DEVICE_TABLE(of, sophgo_dsi_of_match);

static struct platform_driver sophgo_dsi_platform_driver = {
	.probe = sophgo_dsi_probe,
	.remove = sophgo_dsi_remove,
	.shutdown = sophgo_dsi_shutdown,
	.driver = {
		.name = "sophgo-dsi",
		.pm = &sophgo_dsi_pm,
		.of_match_table = sophgo_dsi_of_match,
	},
};
module_platform_driver(sophgo_dsi_platform_driver);

MODULE_DESCRIPTION("Sophgo SG2000 DSI host: fixed 720p HDMI scanout through the LT8912B");
MODULE_AUTHOR("BadgeOS contributors");
MODULE_LICENSE("GPL");

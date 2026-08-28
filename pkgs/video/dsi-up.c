/* dsi-up — stand up the SG2000 DSI link for the Nix Badge 2.0 HDMI path.
 *
 * Minimal arm64 replacement for the vendor `sample_dsi` (riscv-only middleware
 * binary) on the hardware-verified golden path (issue #4). Talks straight to
 * the soph/cv181x mipi_tx driver:
 *
 *   /dev/cvi-mipi-tx: SET_DEV_CFG -> SET_HS_SETTLE -> ENABLE
 *
 * Config provenance:
 *  - timing/preset = dev_cfg_lt9611_1280x720_60Hz from the vendor SDK
 *    (milkv-duo/duo-buildroot-sdk-v2 @ 6f8962c,
 *    cvi_mpi/component/panel/cv181x/dsi_lt9611.h) — CEA-861 720p60;
 *    hs-settle {6,32,5} = hs_timing_cfg_lt9611 from the same header.
 *  - lane_id = the badge's wiring {D0,D1,CLK,D2,D3}: `--laneid=1,2,0,3,4`,
 *    proven by the exhaustive 24-permutation sweep (docs/hdmi-bringup.md).
 *
 * Struct/ioctl definitions come from the same pinned sophgo/osdrv tree the
 * kernel modules are built from (interdrv/include/common/uapi/linux/
 * cvi_comm_mipi_tx.h), so userspace and driver cannot drift.
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <linux/cvi_comm_mipi_tx.h>

#define DEV "/dev/cvi-mipi-tx"

int main(void)
{
	struct combo_dev_cfg_s cfg = {
		.devno = 0,
		/* badge lane map: pos0=D0 pos1=D1 pos2=CLK pos3=D2 pos4=D3 */
		.lane_id = {
			MIPI_TX_LANE_0,
			MIPI_TX_LANE_1,
			MIPI_TX_LANE_CLK,
			MIPI_TX_LANE_2,
			MIPI_TX_LANE_3,
		},
		.lane_pn_swap = { false, false, false, false, false },
		.output_mode = OUTPUT_MODE_DSI_VIDEO,
		.video_mode = BURST_MODE,
		.output_format = OUT_FORMAT_RGB_24_BIT,
		.sync_info = {
			.vid_hsa_pixels = 40,
			.vid_hbp_pixels = 220,
			.vid_hfp_pixels = 110,
			.vid_hline_pixels = 1280,
			.vid_vsa_lines = 5,
			.vid_vbp_lines = 20,
			.vid_vfp_lines = 5,
			.vid_active_lines = 720,
			.vid_vsa_pos_polarity = true,
			.vid_hsa_pos_polarity = true,
		},
		.pixel_clk = 74250,
	};
	struct hs_settle_s settle = { .prepare = 6, .zero = 32, .trail = 5 };
	int fd;

	fd = open(DEV, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "dsi-up: open %s: %s\n", DEV, strerror(errno));
		return 1;
	}
	if (ioctl(fd, CVI_VIP_MIPI_TX_SET_DEV_CFG, &cfg) < 0) {
		fprintf(stderr, "dsi-up: SET_DEV_CFG: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(fd, CVI_VIP_MIPI_TX_SET_HS_SETTLE, &settle) < 0) {
		fprintf(stderr, "dsi-up: SET_HS_SETTLE: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(fd, CVI_VIP_MIPI_TX_ENABLE) < 0) {
		fprintf(stderr, "dsi-up: ENABLE: %s\n", strerror(errno));
		return 1;
	}
	close(fd);
	printf("dsi-up: 1280x720p60 RGB888, 4-lane, lane map 1,2,0,3,4 — link up\n");
	return 0;
}

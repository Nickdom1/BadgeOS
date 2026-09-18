/* SPDX-License-Identifier: GPL-2.0-only */
/* Native execution of the register-snapshot header: ownership gate, fenced
 * page copies, per-block trace marks, ring order, partial captures and the
 * fixed version-3 blob layout the host reader depends on. No hardware model is
 * implied. The sixth block (dphy) is a separately supervised addition; here it is
 * just another fenced MMIO page.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define __iomem
struct regmap {
	unsigned int regs[1024];
	int fail;
	unsigned int reads, count;
};
static unsigned int mmio_reads;
static unsigned int readl(const void *addr)
{
	mmio_reads++;
	return *(const unsigned int *)addr;
}
static int regmap_bulk_read(struct regmap *m, unsigned int reg, void *val, unsigned int count)
{
	assert(reg == 0 && count <= 1024);
	m->reads++;
	m->count = count;
	if (m->fail)
		return m->fail;
	memcpy(val, m->regs, count * sizeof(unsigned int));
	return 0;
}
#include "sophgo-dump.h"

static struct sophgo_dump dump;
static unsigned int pages[5][1024];
static struct regmap map;
static const unsigned int fences[SOPHGO_DUMP_BLOCKS] = {64, 256, 16, 64, 704, 51};
#define FENCED_MMIO (64 + 256 + 16 + 704 + 51)

/* Trace sink: record every mark in order. */
static char marks[64][32];
static bool returned[64];
static unsigned int nmarks;
static void emit(void *ctx, const char *label, bool ret)
{
	assert(ctx == &dump && nmarks < 64);
	strcpy(marks[nmarks], label);
	returned[nmarks++] = ret;
}
static struct sophgo_trace trace = {.emit = emit, .ctx = &dump};

static void check_marks(unsigned int valid, unsigned int vip_attempted)
{
	static const char *const names[] = {"snapshot scaler_top", "snapshot disp", "snapshot dsi_mac",
					    "snapshot vip_sys", "snapshot clk_ctrl", "snapshot dphy"};
	unsigned int b, i = 0;

	for (b = 0; b < SOPHGO_DUMP_BLOCKS; b++) {
		bool attempted = (valid & (1U << b)) || (b == SOPHGO_DUMP_VIP_SYS && vip_attempted);

		if (!attempted)
			continue;
		assert(i + 2 <= nmarks);
		assert(!strcmp(marks[i], names[b]) && !returned[i]);
		assert(!strcmp(marks[i + 1], names[b]) && returned[i + 1]);
		i += 2;
	}
	assert(i == nmarks);
	nmarks = 0;
}

static void check_slot(unsigned int index, const char *label, unsigned long long ns, int error,
		       unsigned int valid)
{
	const struct sophgo_dump_slot *slot = &dump.slot[index];
	unsigned int b, w;

	assert(!strcmp(slot->label, label) && slot->ns == ns && slot->error == error);
	assert(slot->valid == valid);
	for (b = 0; b < SOPHGO_DUMP_BLOCKS; b++) {
		const unsigned int *expected = b == SOPHGO_DUMP_VIP_SYS   ? map.regs :
					       b == SOPHGO_DUMP_CLK_CTRL ? pages[3] :
					       b == SOPHGO_DUMP_DPHY	 ? pages[4] : pages[b];

		if (valid & (1U << b))
			assert(!memcmp(dump.word[index][b], expected, fences[b] * 4));
		else
			for (w = 0; w < fences[b]; w++)
				assert(!dump.word[index][b][w]);
		/* Beyond the fence nothing is ever read: always zero. */
		for (w = fences[b]; w < SOPHGO_DUMP_WORDS; w++)
			assert(!dump.word[index][b][w]);
	}
}

int main(void)
{
	const unsigned int phys[SOPHGO_DUMP_BLOCKS] = {0x0a080000, 0x0a088000, 0x0a08a000, 0x0a0c8000,
						       0x03002000, 0x0a0d1000};
	struct sophgo_dump_sources s = {.page = {pages[0], pages[1], pages[2], NULL, pages[3], pages[4]},
					.vip = &map};
	unsigned int b, w;

	/* Blob ABI v3: header 272 bytes, then slots x blocks x 4 KiB pages. */
	assert(sizeof(dump) == 74000);
	assert(offsetof(struct sophgo_dump, phys) == 24 && offsetof(struct sophgo_dump, name) == 48);
	assert(offsetof(struct sophgo_dump, fence) == 120 && offsetof(struct sophgo_dump, reserved) == 144);
	assert(offsetof(struct sophgo_dump, slot) == 152 && offsetof(struct sophgo_dump, word) == 272);
	assert(offsetof(struct sophgo_dump_slot, ns) == 24 && offsetof(struct sophgo_dump_slot, error) == 32);
	assert(offsetof(struct sophgo_dump_slot, valid) == 36);
	/* Fences are the documented block extents, and clk_ctrl stops below the
	 * bus-unreadable 0xb00 (golden clk_ctrl.missing). The dphy fence covers
	 * the full mapped D-PHY window (0x00-0xc8, SOPHGO_DPHY_SIZE 0xcc).
	 */
	assert(SOPHGO_DUMP_FENCE_CLK_CTRL * 4 == 0xb00 && SOPHGO_DUMP_FENCE_DISP * 4 > 0x314);
	assert(SOPHGO_DUMP_FENCE_SC_TOP * 4 > 0x94 && SOPHGO_DUMP_FENCE_VIP_SYS * 4 > 0xe0);
	assert(SOPHGO_DUMP_FENCE_DSI_MAC * 4 > 0x24);
	assert(SOPHGO_DUMP_FENCE_DPHY * 4 == 0xcc && SOPHGO_DUMP_FENCE_DPHY * 4 > 0xa0);

	for (b = 0; b < 5; b++)
		for (w = 0; w < 1024; w++)
			pages[b][w] = 0x01000000U * (b + 1) + w * 0x1001U;
	for (w = 0; w < 1024; w++)
		map.regs[w] = 0xa5000000U ^ w;

	memset(&dump, 0x5a, sizeof(dump));
	sophgo_dump_init(&dump, phys);
	assert(dump.magic == 0x50444753U && dump.version == 3 && dump.page == 0x1000);
	assert(dump.blocks == 6 && dump.slots == 3 && dump.next == 0);
	assert(!memcmp(dump.phys, phys, sizeof(phys)));
	assert(!memcmp(dump.fence, fences, sizeof(fences)));
	assert(!dump.reserved[0] && !dump.reserved[1]);
	assert(!strcmp(dump.name[0], "scaler_top") && !strcmp(dump.name[1], "disp"));
	assert(!strcmp(dump.name[2], "dsi_mac") && !strcmp(dump.name[3], "vip_sys"));
	assert(!strcmp(dump.name[4], "clk_ctrl") && !strcmp(dump.name[5], "dphy"));
	assert(!dump.slot[0].valid && !dump.word[2][5][1023]);

	/* Without clock ownership nothing is read and nothing is traced, ever. */
	assert(sophgo_dump_capture(&dump, &s, "unowned", false, 1, &trace) == -EACCES);
	assert(!mmio_reads && !map.reads && !nmarks && dump.next == 0 && !dump.slot[0].label[0]);
	assert(sophgo_dump_capture(NULL, &s, "x", true, 1, &trace) == -EINVAL);
	assert(sophgo_dump_capture(&dump, NULL, "x", true, 1, &trace) == -EINVAL);
	assert(sophgo_dump_capture(&dump, &s, NULL, true, 1, &trace) == -EINVAL);
	dump.magic ^= 1;
	assert(sophgo_dump_capture(&dump, &s, "x", true, 1, &trace) == -EINVAL);
	dump.magic ^= 1;
	/* A corrupted fence refuses the whole capture before any access. */
	dump.fence[SOPHGO_DUMP_CLK_CTRL] = SOPHGO_DUMP_WORDS + 1;
	assert(sophgo_dump_capture(&dump, &s, "x", true, 1, &trace) == -EINVAL);
	dump.fence[SOPHGO_DUMP_CLK_CTRL] = SOPHGO_DUMP_FENCE_CLK_CTRL;
	assert(!mmio_reads && !map.reads && !nmarks && dump.next == 0);

	assert(!sophgo_dump_capture(&dump, &s, "prepared", true, 123456789ULL, &trace));
	assert(mmio_reads == FENCED_MMIO && map.reads == 1 && map.count == 64 && dump.next == 1);
	check_slot(0, "prepared", 123456789ULL, 0, 0x3f);
	check_marks(0x3f, 1);

	/* A syscon failure is retained; the MMIO pages are still captured. */
	map.fail = -EIO;
	pages[1][7] ^= 0xffffffffU;
	assert(sophgo_dump_capture(&dump, &s, "started", true, 2, &trace) == -EIO);
	assert(mmio_reads == 2 * FENCED_MMIO && map.reads == 2 && dump.next == 2);
	check_slot(1, "started", 2, -EIO, 0x37);
	check_marks(0x37, 1);
	map.fail = 0;

	/* Unmapped clock page: skipped without error or marks; long labels are
	 * truncated; a NULL trace is accepted.
	 */
	s.page[SOPHGO_DUMP_CLK_CTRL] = NULL;
	assert(!sophgo_dump_capture(&dump, &s, "0123456789abcdef0123456789", true, 3, NULL));
	assert(mmio_reads == 2 * FENCED_MMIO + (FENCED_MMIO - 704) && dump.next == 3 && !nmarks);
	check_slot(2, "0123456789abcdef0123456", 3, 0, 0x2f);
	s.page[SOPHGO_DUMP_CLK_CTRL] = pages[3];

	/* Ring: the fourth capture replaces the oldest slot and clears stale words. */
	pages[0][0] = 0xdeadbeef;
	assert(!sophgo_dump_capture(&dump, &s, "complete", true, 4, &trace));
	assert(dump.next == 4);
	check_slot(0, "complete", 4, 0, 0x3f);
	check_marks(0x3f, 1);
	pages[0][0] = 0x01000000U; /* slot 1 was captured before that write */
	check_slot(1, "started", 2, -EIO, 0x37);

	/* with the dphy source unmapped (pinstripe_dphy off), the block is
	 * skipped with no D-PHY access and valid clears its bit.
	 */
	s.page[SOPHGO_DUMP_DPHY] = NULL;
	mmio_reads = 0;
	assert(!sophgo_dump_capture(&dump, &s, "no-dphy", true, 5, &trace));
	assert(mmio_reads == FENCED_MMIO - 51 && dump.next == 5);
	check_slot(1, "no-dphy", 5, 0, 0x1f); /* next was 4: 4 % 3 slots == slot 1 */
	check_marks(0x1f, 1);
	s.page[SOPHGO_DUMP_DPHY] = pages[4];

	puts("PASS: snapshot blob v3 layout, documented fences (clk_ctrl below 0xb00, dphy 0x00-0xc8), "
	     "ownership and fence refusal before any read, fenced six-page copies with zero tails, "
	     "per-block trace marks, retained syscon error, skipped unmapped clk/dphy pages, label "
	     "truncation and ring replacement");
	return 0;
}

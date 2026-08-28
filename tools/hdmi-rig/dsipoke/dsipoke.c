// dsipoke — one-shot masked register write for the SG2000/CV181x DSI blocks.
//
// The safe write path for the B4/B3 dialect experiments: userspace /dev/mem
// WRITES wedge the SoC, but kernel-space masked RMW on these blocks is the
// vendor driver's own routine mechanism (osdrv interdrv/vpss/chip/cv181x/
// scaler.c uses _reg_write_mask on REG_SCL_DSI_HS_0; our read-only dispdump.ko
// flips the DISP_CFG bank-mux bit the same way on every /proc read).
//
// Target finding (session 19): DSI MAC HS_0 (0x0A08A004) reads 0x26000000
// while streaming. sclr_dsi_config() only writes mask 0xC3000000 (fmt<<30 |
// lanes<<24); bits 26 and 29 are SET by hardware reset default and written by
// no vendor code path (kernel scaler.c, U-Boot cvitek/scaler.c, freertos
// cv1835 HAL all checked). They are the prime sync-mode/EoTp knob candidates.
//
// One-shot: does pre-read -> masked write -> post-read in init, logs all three
// to dmesg, then returns -ECANCELED so it never stays loaded ("Operation
// canceled" from insmod is EXPECTED — the result is in dmesg).
//
//   insmod dsipoke.ko off=0x04 mask=0x04000000 val=0x00000000   # clear bit26
//   dmesg | tail -2
//
// Params: base (phys, default DSI MAC 0x0A08A000; PHY wrap = 0x0A0D1000),
//         off (< 0x100), mask, val (only mask bits of val are applied).

#include <linux/module.h>
#include <linux/io.h>

static ulong base = 0x0A08A000UL;
static uint off;
static uint mask;
static uint val;
module_param(base, ulong, 0);
module_param(off, uint, 0);
module_param(mask, uint, 0);
module_param(val, uint, 0);
MODULE_PARM_DESC(base, "block phys base (default DSI MAC 0x0A08A000)");
MODULE_PARM_DESC(off, "register offset within block, < 0x100");
MODULE_PARM_DESC(mask, "bits to modify (0 = read-only probe)");
MODULE_PARM_DESC(val, "new value for masked bits");

static int __init dsipoke_init(void)
{
	void __iomem *p;
	u32 pre, post;

	if (off >= 0x100 || (off & 3))
		return -EINVAL;

	p = ioremap(base, 0x100);
	if (!p)
		return -ENOMEM;

	pre = readl(p + off);
	if (mask) {
		writel((pre & ~mask) | (val & mask), p + off);
		post = readl(p + off);
	} else {
		post = pre;
	}
	iounmap(p);

	pr_info("dsipoke: %08lx+%02x mask=%08x val=%08x : pre=%08x post=%08x%s\n",
		base, off, mask, val, pre, post,
		(mask && ((post & mask) != (val & mask))) ? " (BITS DID NOT STICK)" : "");

	return -ECANCELED; /* one-shot: never stay resident */
}
module_init(dsipoke_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("one-shot masked MMIO poke for SG2000 DSI MAC/PHY experiments");

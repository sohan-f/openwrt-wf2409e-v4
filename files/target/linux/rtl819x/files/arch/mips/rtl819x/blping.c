// SPDX-License-Identifier: GPL-2.0
/*
 * blping - LED-trace milestone instrumentation for the WF2409E v4
 * no-UART bring-up (build.sh TRACE builds only; harmless if unused).
 *
 * blping_code(n) draws a one-shot busy-wait blink train of n dark pulses
 * on the activity (gpio33, active low) and power (gpio35, active high)
 * front lamps, then leaves both lit.  Busy-wait instead of timers so a
 * train still completes when it is called with IRQs preempted off; the
 * caller's milestone then advances or the board hangs holding the last
 * completed code.
 *
 * PEFGH bank pin registers (donor rtl8197f_eth.c GPIO conventions):
 *   ports E-H: CNR 0x1c, DIR 0x24, DAT 0x28 in the block at 0x18003500,
 *   bit = (port-4)*8 + pin; CNR 0 = GPIO function, DIR 1 = output.
 */
#include <linux/export.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/init.h>

#define BLP_PEFH_CNR	0xb800351cUL
#define BLP_PEFH_DIR	0xb8003524UL
#define BLP_PEFH_DAT	0xb8003528UL

#define BLP_BIT(act32)	(1u << ((act32) - 32))

void blping_code(int code);

static void blping_pick(void)
{
	writel(readl((void __iomem *)BLP_PEFH_CNR) &
	       ~(BLP_BIT(33) | BLP_BIT(35)), (void __iomem *)BLP_PEFH_CNR);
	writel(readl((void __iomem *)BLP_PEFH_DIR) |
	       (BLP_BIT(33) | BLP_BIT(35)), (void __iomem *)BLP_PEFH_DIR);
}

static void blping_set(int on)
{
	u32 v = readl((void __iomem *)BLP_PEFH_DAT);

	if (on) {
		/* activity is active low, power active high */
		v &= ~BLP_BIT(33);
		v |= BLP_BIT(35);
	} else {
		v |= BLP_BIT(33);
		v &= ~BLP_BIT(35);
	}
	writel(v, (void __iomem *)BLP_PEFH_DAT);
}

void blping_code(int code)
{
	int i;

	if (code < 1)
		return;

	blping_pick();
	for (i = 0; i < code; i++) {
		blping_set(0);
		mdelay(140);
		blping_set(1);
		mdelay(160);
	}
	blping_set(1);
	mdelay(1000);
}
EXPORT_SYMBOL(blping_code);

static int __init blping_init(void)
{
	blping_pick();
	blping_set(1);
	blping_code(1);
	return 0;
}
core_initcall(blping_init);

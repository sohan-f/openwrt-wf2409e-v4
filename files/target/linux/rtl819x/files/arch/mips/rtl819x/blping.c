// SPDX-License-Identifier: GPL-2.0
/*
 * blping - LED-trace milestone instrumentation for the WF2409E v4
 * no-UART bring-up (build.sh TRACE builds only; harmless if unused).
 *
 * blping_code(n) draws a one-shot busy-wait blink train of n dark pulses
 * followed by one long dark tail on the activity (gpio33, active low)
 * and power (gpio35, active high) front lamps.  Decode rule: the pulse
 * COUNT is the milestone; the trailing dark tail marks the end of a
 * train so a 1-pulse code 1 is still visible against ambient lamp noise.
 *
 * Busy-wait instead of timers so a train still completes when it is
 * called with IRQs preempted off; the caller's milestone then advances
 * or the board hangs holding the last completed code.
 *
 * From late_initcall on, a kernel thread re-renders the LAST completed
 * code (count + tail) once per 3 s cycle.  Heartbeat alive = the
 * scheduler and timers run and the kernel reached the end of the
 * initcall ladder; a driver freeze does not stop it.  No heartbeat at
 * all = the kernel died before late initcalls finished.
 *
 * PEFGH bank pin registers (donor rtl8197f_eth.c GPIO conventions):
 *   ports E-H: CNR 0x1c, DIR 0x24, DAT 0x28 in the block at 0x18003500,
 *   bit = (port-4)*8 + pin; CNR 0 = GPIO function, DIR 1 = output.
 */
#include <linux/export.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/kthread.h>

#define BLP_PEFH_CNR	0xb800351cUL
#define BLP_PEFH_DIR	0xb8003524UL
#define BLP_PEFH_DAT	0xb8003528UL

#define BLP_BIT(act32)	(1u << ((act32) - 32))

#define BLP_DARK_MS	250	/* short dark pulse width */
#define BLP_GAP_MS	250	/* lit gap between pulses */
#define BLP_TAIL_MS	700	/* end-of-train dark tail */
#define BLP_HEART_MS	3000	/* heartbeat idle gap */

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

static void blping_train(int code)
{
	int i;

	if (code < 1)
		return;

	blping_pick();
	for (i = 0; i < code; i++) {
		blping_set(0);
		mdelay(BLP_DARK_MS);
		blping_set(1);
		mdelay(BLP_GAP_MS);
	}
	/* the tail distinguishes "one pulse" from ambient lamp noise */
	blping_set(0);
	mdelay(BLP_TAIL_MS);
	blping_set(1);
}

static atomic_t blping_last = ATOMIC_INIT(0);
static struct task_struct *blping_thread;

void blping_code(int code)
{
	blping_train(code);
	atomic_set(&blping_last, code);
	mdelay(1000);
}
EXPORT_SYMBOL(blping_code);

static int blping_beat(void *unused)
{
	int code;

	for (;;) {
		if (kthread_should_stop())
			break;
		code = atomic_read(&blping_last);
		blping_train(code);
		msleep(BLP_HEART_MS);
	}
	return 0;
}

static int __init blping_init(void)
{
	blping_pick();
	blping_set(1);
	blping_code(1);
	return 0;
}
core_initcall(blping_init);

static int __init blping_late_init(void)
{
	/* after every initcall level ran, try to start the heartbeat */
	blping_thread = kthread_run(blping_beat, NULL, "blping-alive");
	return IS_ERR(blping_thread) ? PTR_ERR(blping_thread) : 0;
}
late_initcall(blping_late_init);

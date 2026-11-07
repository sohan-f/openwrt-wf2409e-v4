// SPDX-License-Identifier: GPL-2.0-only
/*
 * wf2409e_wlan.h - Netis WF2409E v4 (RTL8197F) clean-room WLAN driver.
 *
 * Every constant here carries its provenance grade:
 *   [S] verified against the friend-unit stock kernel binary
 *       (Linux 3.10.90, .local/mtd0-re/carve/), [D] donor-vendor source,
 *   [P] patch RE (patches/0014).
 */

#ifndef WF2409E_WLAN_H
#define WF2409E_WLAN_H

#include <linux/types.h>

/*
 * [S] WMAC register space, from wlan_device[0] at VA 0x805bd67c in the
 * stock kernel (words: base 0xb8640000, irq 6). Same base/irq the donor
 * build compiled for TYPE_EMBEDDED.
 *
 * [D] Spatial layout per the donor rtl8192cd accessors: MAC regs are
 * byte/word/dword at +0x0.. (the MAC image reaches 0x1448); the BB/PHY
 * block is dword-only at +0x800..0xfff; RF access uses two ports inside
 * that block plus a readback window far beyond it, hence the size:
 *   RF LRF write ports      A 0x840, B 0x844   ((addr<<20)|data) & 0x0fffffff
 *   RF readback shadow win  A 0x2800, B 0x2c00 + (reg<<2), data & 0xfffff
 */
#define WF_WLAN_PHYS		0x18640000	/* KSEG1 0xb8640000 */
#define WF_WLAN_SIZE		0x3000
#define WF_WLAN_IRQ		6	/* unused until M3 rings/ISR */

/*
 * [S] SoC system space (KSEG1 0xb8000000). Chip identity at +0x00 must read
 * 0x8197F000 (low 12 bits = silicon revision); +0x64 gates the WMAC clocks:
 * stock rtl8192cd_init_one RMWs 0x1F into it once the chip tag is confirmed
 * (donor 8192cd_osdep.c:16272). Evidence per constant: docs/wlan-re.md.
 */
#define WF_SOC_KSEG1		0xb8000000
#define WF_SOC_CHIP_ID		0x00
#define WF_SOC_CHIP_ID_TAG	0x8197f000
#define WF_SOC_CHIP_ID_MASK	0xfffff000
#define WF_SOC_WMAC_CLK_EN	0x64
#define WF_SOC_WMAC_CLK_BITS	0x1f

/*
 * [D] PWR sequence command types, donor WlanHAL/RTL88XX/Hal88XXPwrSeqCmd.h.
 */
enum wf_pwr_cmd {
	PWR_CMD_READ		= 0x00,
	PWR_CMD_WRITE		= 0x01,
	PWR_CMD_POLLING		= 0x02,
	PWR_CMD_DELAY		= 0x03,
	PWR_CMD_END		= 0xff,
};

#define PWR_CUT_ALL_MSK		0xff
#define PWR_FAB_ALL_MSK		0xff
#define PWR_INTF_ALL_MSK	0xfff
#define PWR_BASEADDR_MAC	1

#define WF_DRV_NAME		"wf2409e-wlan"

#define WF_PWR_POLL_MAX		5000	/* [D] maxPollingCnt, same name */

/*
 * [D] PWR command row, from the vendor WLAN_PWR_CFG layout
 *     {offset, cut_msk, fab_msk, intf_msk, base|cmd, msk, value}.
 * Our M1 sheet keeps only the fields the engine consumes: every row is
 * PWR_*_ALL_MSK and PWR_BASEADDR_MAC (wf-pwr-gen.py asserts both), so the
 * mask fields carry no information. Rows are generated into
 * wf2409e_wlan.c; regenerate with tools/wf-pwr-gen.py, never hand-edit.
 */
struct wf_pwr_cmd_entry {
	u8 offset;
	u8 base;
	u16 cmd;
	u8 msk;
	u8 value;
};

/*
 * [D] Image row shapes, donor halhwimg8197f_*.c walkers: MAC rows apply
 * plain byte writes (odm_write_1byte), BB/AGC rows apply odm_set_bb_reg
 * with the walker's full dword mask (plain writel), RF rows go through
 * the LRF write protocol above. The generator resolves condition
 * records and delay pseudo-addrs at build time; RF tables may still
 * carry 0xfe/0xffe rows (50 ms pause, no write).
 */
struct wf_mac_op {
	u16 addr;
	u8 val;
};

struct wf_bb_op {
	u16 addr;
	u32 val;
};

struct wf_rf_op {
	u16 addr;
	u32 val;
};

/* [D] RF access protocol, phydm_hal_api8197f.c (config_phydm_*_rf_reg):
 * writes pack reg[27:20]|data[19:0] into the LRF port, reads hit the
 * shadow window far beyond the BB block. */
#define WF_RF_LRF_A		0x840
#define WF_RF_LRF_B		0x844
#define WF_RF_READ_A		0x2800
#define WF_RF_READ_B		0x2c00
#define WF_RF_MASK		0xfffff		/* RFREGOFFSETMASK */
#define WF_RF_INVALID		0xffffffff	/* INVALID_RF_DATA */

/* [D] BB setup rows, phydm_regdefine11n.h + 8192cd_hw.c 97F blocks */
#define WF_BB_BB_CTRL		0x800	/* ODM_REG_BB_CTRL_11N */
#define WF_BB_TX_ANT_CTRL	0x80c
#define WF_BB_TX_PATH		0x90c
#define WF_BB_RX_PATH		0xc04
#define WF_BB_CCA_C3C		0xc3c
#define WF_BB_CCK_2R		0xa2c
#define WF_BB_CCK_DIV		0xa74
#define WF_BB_CCK_ANT_SEL	0xa04
#define WF_BB_CCA_AAA		0xaa8
#define WF_BB_RX_ANT		0xd04

/* [D] RF channel/bandwidth (RF18) */
#define WF_RF_CHNLBW		0x18

/* [D] MAC-side extras, 8192cd_hw.c:18258-18261 */
#define WF_MAC_7D4		0x7d4

#endif /* WF2409E_WLAN_H */

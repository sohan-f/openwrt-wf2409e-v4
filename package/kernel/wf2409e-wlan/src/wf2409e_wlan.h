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

/*
 * [S] WMAC register space, from wlan_device[0] at VA 0x805bd67c in the
 * stock kernel (words: base 0xb8640000, irq 6). Same base/irq the donor
 * build compiled for TYPE_EMBEDDED.
 */
#define WF_WLAN_PHYS		0x18640000	/* KSEG1 0xb8640000 */
#define WF_WLAN_SIZE		0x400
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

#endif /* WF2409E_WLAN_H */

// SPDX-License-Identifier: GPL-2.0-only
/*
 * wf2409e_wlan.c - Netis WF2409E v4 (RTL8197F) clean-room WLAN driver, M2.
 *
 * Milestone scope: M1 card enable + PWR engine, plus the vendor init
 * chain rtl8192cd_init_hw_PCI runs for the 97F: MAC/BB/AGC/RF register
 * images (GENERATED tables, tools/wf-macbb-gen.py), trx 2T2R mode, and
 * an explicit channel switch. Still no descriptor rings or real TX/RX
 * (M3). Per-constant evidence: docs/wlan-re.md.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <net/mac80211.h>

#include "wf2409e_wlan.h"
#include "wf2409e_tables.h"

struct wf_priv {
	void __iomem *wmac;
};

/* WMAC space is byte-addressed; the PWR tables rely on readb/writeb RMW
 * semantics exactly like the vendor HAL_RTL_R8/W8 engine does. */

static inline u8 wf_r8(struct wf_priv *priv, u8 off)
{
	return readb(priv->wmac + off);
}

static inline void wf_rmw8(struct wf_priv *priv, u8 off, u8 msk, u8 set)
{
	u8 v;

	v = readb(priv->wmac + off);
	v = (v & (u8)~msk) | (set & msk);
	writeb(v, priv->wmac + off);
}

/*
 * PWR engine, our rendering of the vendor command loop
 * (Hal88XXPwrSeqCmd.c). The END check must come before the base filter:
 * the vendor's END row itself has base == 0, and stopping on cmd is what
 * keeps the walker inside a finite table. Deviations from the vendor
 * walker: 10us backoff between poll reads, and a poll timeout aborts
 * start() instead of soldiering on.
 */
static int wf_pwr_run(struct wf_priv *priv, struct device *dev,
		      const struct wf_pwr_cmd_entry *tbl)
{
	int i;

	for (i = 0; ; i++) {
		const struct wf_pwr_cmd_entry *e = &tbl[i];
		u8 v;
		int n;

		if (e->cmd == PWR_CMD_END)
			return 0;

		/* Our sheets are all-MAC by construction (wf-pwr-gen.py
		 * asserts it); other bases would need PHY-space accessors
		 * (M2), so skipping them keeps future sheets safe. */
		if (e->base != PWR_BASEADDR_MAC)
			continue;

		switch (e->cmd) {
		case PWR_CMD_WRITE:
			wf_rmw8(priv, e->offset, e->msk, e->value);
			break;

		case PWR_CMD_POLLING:
			n = 0;
			do {
				v = wf_r8(priv, e->offset);
				if ((v & e->msk) == e->value)
					break;
				udelay(10);
			} while (++n < WF_PWR_POLL_MAX);
			if (n >= WF_PWR_POLL_MAX) {
				dev_warn(dev,
					"PWR polling timeout @0x%02x (val=0x%02x, want msk 0x%02x == 0x%02x)\n",
					e->offset, v, e->msk, e->value);
				return -ETIMEDOUT;
			}
			break;

		default:
			break;
		}
	}
}

/*
 * Card-enable sequence = RTL8197F_TRANS_96FSOC_POWER_ON (11 rows, donor
 * Hal8197FPwrSeqCmd.h) + the ten radio power-on writes restored from the
 * stock kernel table (patches/0014), then END. GENERATED - do not
 * hand-edit; rebuild the rows with tools/wf-pwr-gen.py.
 */
static const struct wf_pwr_cmd_entry wf_card_enable_flow[] = {
	{ .offset = 0xa3, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x07, .value = 0x00 },
	{ .offset = 0xa0, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_POLLING, .msk = 0xf0, .value = 0x20 },
	{ .offset = 0xa1, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_POLLING, .msk = 0x03, .value = 0x02 },
	{ .offset = 0x05, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x01, .value = 0x01 },
	{ .offset = 0x06, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x01, .value = 0x01 },
	{ .offset = 0x05, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_POLLING, .msk = 0x01, .value = 0x00 },
	{ .offset = 0xa3, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x07, .value = 0x04 },
	{ .offset = 0xa0, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_POLLING, .msk = 0xff, .value = 0x00 },
	{ .offset = 0xa1, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_POLLING, .msk = 0x03, .value = 0x01 },
	{ .offset = 0x02, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0xff, .value = 0xff },
	{ .offset = 0x03, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0xff, .value = 0xff },
	{ .offset = 0x1f, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x07, .value = 0x00 },
	{ .offset = 0x7b, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x07, .value = 0x00 },
	{ .offset = 0x1f, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x07, .value = 0x07 },
	{ .offset = 0x7b, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x07, .value = 0x07 },
	{ .offset = 0x20, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x08, .value = 0x08 },
	{ .offset = 0x21, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x04, .value = 0x04 },
	{ .offset = 0x25, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x02, .value = 0x00 },
	{ .offset = 0x1f, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x80, .value = 0x00 },
	{ .offset = 0x7b, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x80, .value = 0x00 },
	{ .offset = 0x78, .base = PWR_BASEADDR_MAC, .cmd = PWR_CMD_WRITE, .msk = 0x40, .value = 0x00 },
	{ .cmd = PWR_CMD_END },
};

/* SoC identity + WMAC clock gate. Both touching WMAC registers before
 * the gate opens is what stock init_one does, so validate the tag first
 * (see wf2409e_wlan.h for the evidence). Direct KSEG1 access matches the
 * donor REG32() usage on this platform; a DT binding comes later. */
static inline u32 wf_soc_rmw32(u8 off, u32 msk, u32 set)
{
	volatile void __iomem *p = (volatile void __iomem *)(WF_SOC_KSEG1 + off);
	u32 v;

	v = readl(p);
	v = (v & ~msk) | (set & msk);
	writel(v, p);
	return v;
}

static bool wf_chip_ok(struct device *dev)
{
	u32 tag;

	tag = readl((volatile void __iomem *)(WF_SOC_KSEG1 + WF_SOC_CHIP_ID));
	tag &= WF_SOC_CHIP_ID_MASK;

	if (tag != WF_SOC_CHIP_ID_TAG) {
		dev_warn(dev, "unexpected SoC tag 0x%08x (want 0x%08x)\n",
			 tag, WF_SOC_CHIP_ID_TAG);
		return false;
	}

	wf_soc_rmw32(WF_SOC_WMAC_CLK_EN, WF_SOC_WMAC_CLK_BITS,
		     WF_SOC_WMAC_CLK_BITS);
	return true;
}

/* BB block helper set. phy_set_bb_reg RMWs except with the full dword
 * mask, where it is a plain write; generated image rows carry the full
 * mask shape by construction, trx writes reuse the RMW helper with the
 * vendor masks. */

static inline u32 wf_bb_r32(struct wf_priv *priv, u16 off)
{
	return readl(priv->wmac + off);
}

static inline void wf_bb_w32(struct wf_priv *priv, u16 off, u32 v)
{
	writel(v, priv->wmac + off);
}

static inline void wf_bb_rmw32(struct wf_priv *priv, u16 off, u32 msk, u32 set)
{
	u32 v;

	v = readl(priv->wmac + off);
	v = (v & ~msk) | (set & msk);
	writel(v, priv->wmac + off);
}

/* RF access per config_phydm_write/read_rf_reg_8197f. Every M2 write
 * (images, trx, RF18) carries a full 20-bit payload, so vendor's
 * read-merge branch for partial masks never runs. Donor paces writes
 * 1us (odm_config_rf_reg_8197f). */
static u32 wf_rf_read(struct wf_priv *priv, u8 path, u8 reg)
{
	u16 base = path ? WF_RF_READ_B : WF_RF_READ_A;

	return readl(priv->wmac + base + (reg << 2)) & WF_RF_MASK;
}

static void wf_rf_write(struct wf_priv *priv, u8 path, u8 reg, u32 data)
{
	void __iomem *port = priv->wmac + (path ? WF_RF_LRF_B : WF_RF_LRF_A);

	writel(((reg << 20) | (data & WF_RF_MASK)) & 0x0fffffff, port);
	udelay(1);
}

static void wf_mac_img_apply(struct wf_priv *priv,
			     const struct wf_mac_op *tbl, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		writeb(tbl[i].val, priv->wmac + tbl[i].addr);
}

static void wf_bb_img_apply(struct wf_priv *priv,
			    const struct wf_bb_op *tbl, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		writel(tbl[i].val, priv->wmac + tbl[i].addr);
}

static void wf_rf_img_apply(struct wf_priv *priv, u8 path,
			    const struct wf_rf_op *tbl, size_t n)
{
	size_t i;
	u16 addr;

	for (i = 0; i < n; i++) {
		addr = tbl[i].addr;
		if (addr == 0xfe || addr == 0xffe)
			mdelay(50);
		else
			wf_rf_write(priv, path, addr, tbl[i].val);
	}
}

/*
 * trx 2T2R = donor config_phydm_trx_mode_8197f(BB_PATH_AB, BB_PATH_AB,
 * tx2path) + phydm_config_cck_rx_path(BB_PATH_A) - vendor keeps CCK CCA
 * 1R despite OFDM 2R ("FA issue" workaround, phydm_hal_api:1111) - plus
 * phydm_cca_par_by_bw_8197f for rfe1/pkg1, cut!=A, BW20, rx AB.
 */
static void wf_trx_2t2r(struct wf_priv *priv)
{
	u32 v;

	/* OFDM TX path AB; mask is bits 27:0, vendor keeps 0x90c[31:28] */
	wf_bb_rmw32(priv, WF_BB_TX_PATH, 0x0fffffff, 0x01121313);
	wf_bb_rmw32(priv, WF_BB_RX_PATH, 0xff, 0x33);
	wf_bb_rmw32(priv, WF_BB_RX_ANT, 0xf, 0x3);
	/* CCK TX path AB first (is_tx2path upgrades H4 0x8 -> 0xc below) */
	wf_bb_rmw32(priv, WF_BB_CCK_ANT_SEL, 0xf0000000, 0x8 << 28);
	/* CCK RX: both input selects to path A, no diversity, no 2R */
	wf_bb_rmw32(priv, WF_BB_CCK_ANT_SEL, BIT(27) | BIT(26), 0);
	wf_bb_rmw32(priv, WF_BB_CCK_ANT_SEL, BIT(25) | BIT(24), 0);
	wf_bb_rmw32(priv, WF_BB_CCK_DIV, BIT(8), 0);
	wf_bb_rmw32(priv, WF_BB_CCK_2R, BIT(18) | BIT(17), 0);
	wf_bb_rmw32(priv, WF_BB_CCK_2R, BIT(22) | BIT(21), 0);
	/* 2-path TX upgrade */
	wf_bb_rmw32(priv, WF_BB_TX_PATH, 0x0fffffff, 0x03321333);
	wf_bb_rmw32(priv, WF_BB_TX_ANT_CTRL, BIT(31), 0);
	wf_bb_rmw32(priv, WF_BB_CCK_ANT_SEL, 0xf0000000, 0xc << 28);
	/* 2T RF table: standby then RX mode per path */
	wf_rf_write(priv, 0, 0x30, 0x08000);
	wf_rf_write(priv, 0, 0x31, 0x0005f);
	wf_rf_write(priv, 0, 0x32, 0x01042);
	wf_rf_write(priv, 0, 0x30, 0x18000);
	wf_rf_write(priv, 0, 0x31, 0x0004f);
	wf_rf_write(priv, 0, 0x32, 0x71fc2);
	wf_rf_write(priv, 1, 0x30, 0x08000);
	wf_rf_write(priv, 1, 0x31, 0x00050);
	wf_rf_write(priv, 1, 0x32, 0x01042);
	wf_rf_write(priv, 1, 0x30, 0x18000);
	wf_rf_write(priv, 1, 0x31, 0x00040);
	wf_rf_write(priv, 1, 0x32, 0x71fc2);
	/* CCA params for rfe1/pkg1/cut!=A/BW20: C3C[8:3], AAA[20:16] */
	v = wf_bb_r32(priv, WF_BB_CCA_C3C);
	v &= ~0x1f8;
	v |= (0x2 << 3) | (0x6 << 6);
	wf_bb_w32(priv, WF_BB_CCA_C3C, v);
	wf_bb_rmw32(priv, WF_BB_CCA_AAA, 0x1f0000, 0x12 << 16);
}

/*
 * POST extras, donor 8192cd_hw.c:18257-18261. The generated image
 * programs RF 0xb4 = 0x141f0, so the vendor gate (== 0x140a0) never
 * fires on this table; the comparison stays live regardless.
 *
 * The readbase comment: the donor 97F read protocol reads RF state at
 * WMAC+0x2800/0x2c00+(reg<<2) (phydm_hal_api8197f.c:317), while the
 * stock rtl8192cd binds its RF read to WMAC+0x8a0/0x8a4 result regs
 * (phy_RFSerialRead -> phy_InitBBRFRegisterDefinition table, no
 * config_phydm_*_8197f symbols in the stock kernel). Both addresses are
 * inside our 0x3000 aperture, so neither read faults; log both until
 * hardware data says which window actually tracks RF state.
 */
static void wf_post_extras(struct wf_priv *priv, struct device *dev)
{
	u32 b4;
	u32 res;

	b4 = wf_rf_read(priv, 0, 0xb4);
	res = readl(priv->wmac + 0x8a0);
	dev_info(dev, "POST: RF 0xb4 readback: win2800 0x%x res8a0 0x%x (vendor gate 0x140a0)\n",
		 b4, res);
	if (b4 == 0x140a0) {
		mdelay(1);
		writeb(0, priv->wmac + 0x7a);
	}
	writeb(0x90, priv->wmac + WF_MAC_7D4);
}

/*
 * Chassis base for the RF18 RMW, = the value the generated RF image
 * itself programs into RF18 (wf2409e_tables.h, donor radioa.txt row).
 * The vendor switch (config_phydm_switch_channel_8197f, hal_api:827)
 * sources this from a readback, but our read window is only [D]-grade
 * evidence (donor 0x2800 window vs the stock kernel's 0x8a0 result-reg
 * path), so the write must not eat a wrong-window read: a garbage base
 * here would program random band bits into RF18. 0x0c07 & ~(0x60000|0xff)
 * | 6 = 0x000c06 for channel 6.
 */
#define WF_RF_CHNBW_BASE	0x000c07

/* Channel switch, config_phydm_switch_channel_8197f: clear band bits
 * (18:17) and channel byte, program both paths. Vendor guards RF_PATH_B
 * on rf_type; ours is 2T2R unconditionally. Vendor's 0xc70 AGC-table
 * read is DIG bookkeeping, no hardware effect without our DIG, so
 * skipped. The old pre-read no longer feeds the write; both candidate
 * read windows are logged instead so hardware data can settle which
 * one is real (see the 0x8a0 note on wf_post_extras). */
static void wf_switch_channel(struct wf_priv *priv, struct device *dev,
			      u8 ch)
{
	u32 v18;
	u32 pre;
	u32 ra;
	u32 rb;

	pre = wf_rf_read(priv, 0, WF_RF_CHNLBW);
	v18 = (WF_RF_CHNBW_BASE & ~(BIT(18) | BIT(17) | 0xff)) | ch;
	wf_rf_write(priv, 0, WF_RF_CHNLBW, v18);
	wf_rf_write(priv, 1, WF_RF_CHNLBW, v18);
	ra = wf_rf_read(priv, 0, WF_RF_CHNLBW);
	rb = wf_rf_read(priv, 1, WF_RF_CHNLBW);
	dev_info(dev, "ch%u: RF18 pre-read 0x%x, post-read A 0x%x B 0x%x (sent 0x%x)\n",
		 ch, pre, ra, rb, v18);
}

static void wf_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *ctx,
		  struct sk_buff *skb)
{
	ieee80211_free_txskb(hw, skb); /* no TX path until M3 */
}

static int wf_start(struct ieee80211_hw *hw)
{
	struct wf_priv *priv = hw->priv;
	struct device *dev = &hw->wiphy->dev;
	int err;

	err = wf_pwr_run(priv, dev, wf_card_enable_flow);
	if (err)
		return err;

	/* 97F init chain order, donor rtl8192cd_init_hw_PCI: PRE gate ->
	 * MAC img -> BB phy_reg -> AGC tab -> RF imgs A/B -> POST gate ->
	 * POST extras -> trx -> our pick of channel 6 */
	wf_bb_rmw32(priv, WF_BB_BB_CTRL, BIT(25) | BIT(24), 0);
	wf_mac_img_apply(priv, wf_mac_img, WF_MAC_IMG_N);
	wf_bb_img_apply(priv, wf_bb_img, WF_BB_IMG_N);
	wf_bb_img_apply(priv, wf_agc_img, WF_AGC_IMG_N);
	wf_rf_img_apply(priv, 0, wf_rf_a_img, WF_RF_A_IMG_N);
	wf_rf_img_apply(priv, 1, wf_rf_b_img, WF_RF_B_IMG_N);
	wf_bb_rmw32(priv, WF_BB_BB_CTRL, BIT(25) | BIT(24),
		    BIT(25) | BIT(24));
	wf_post_extras(priv, dev);
	wf_trx_2t2r(priv);
	wf_switch_channel(priv, dev, 6);

	dev_info(dev, "init chain complete: mac %d, bb %d, agc %d, rfA %d, rfB %d rows\n",
		 WF_MAC_IMG_N, WF_BB_IMG_N, WF_AGC_IMG_N,
		 WF_RF_A_IMG_N, WF_RF_B_IMG_N);
	return 0;
}

static void wf_stop(struct ieee80211_hw *hw, bool suspend)
{
	/* radio shutdown: M2 */
}

static int wf_add_interface(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif)
{
	/* M1 admits any vif; the driver state is inert (no TX/RX). */
	return 0;
}

static void wf_remove_interface(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
}

static void wf_configure_filter(struct ieee80211_hw *hw,
				unsigned int changed_flags,
				unsigned int *total_flags, u64 multicast)
{
	*total_flags = 0; /* RX path appears in M3 */
}

static void wf_bss_info_changed(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_bss_conf *info, u64 changed)
{
}

static const struct ieee80211_ops wf_ops = {
	.tx			= wf_tx,
	.start			= wf_start,
	.stop			= wf_stop,
	.add_interface		= wf_add_interface,
	.remove_interface	= wf_remove_interface,
	.configure_filter	= wf_configure_filter,
	.bss_info_changed	= wf_bss_info_changed,
};

/* 2.4 GHz-only SoC. HT20 + single RX chain until M2/M3 prove more. */
static struct ieee80211_channel wf_chan_2g[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13 },
};

static struct ieee80211_rate wf_rate_2g[] = {
	{ .hw_value = 10,  .bitrate = 10 },
	{ .hw_value = 20,  .bitrate = 20 },
	{ .hw_value = 55,  .bitrate = 55 },
	{ .hw_value = 110, .bitrate = 110 },
	{ .hw_value = 60,  .bitrate = 60 },
	{ .hw_value = 90,  .bitrate = 90 },
	{ .hw_value = 120, .bitrate = 120 },
	{ .hw_value = 180, .bitrate = 180 },
	{ .hw_value = 240, .bitrate = 240 },
	{ .hw_value = 360, .bitrate = 360 },
	{ .hw_value = 480, .bitrate = 480 },
	{ .hw_value = 540, .bitrate = 540 },
};

static struct ieee80211_supported_band wf_band_2g = {
	.band		= NL80211_BAND_2GHZ,
	.channels	= wf_chan_2g,
	.n_channels	= ARRAY_SIZE(wf_chan_2g),
	.bitrates	= wf_rate_2g,
	.n_bitrates	= ARRAY_SIZE(wf_rate_2g),
	.ht_cap		= {
		.ht_supported = true,
		.cap = IEEE80211_HT_CAP_SGI_20,
		.mcs = {
			.rx_mask = { 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
			.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
			.rx_highest = cpu_to_le16(72),
		},
	},
};

/* Self-registered platform device: bring-up without a DT binding; base
 * and IRQ are taken from the stock wlan_device[0] (wf2409e_wlan.h). */
static const struct resource wf_wlan_resources[] = {
	{
		.start	= WF_WLAN_PHYS,
		.end	= WF_WLAN_PHYS + WF_WLAN_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device *wf_pdev;

static int wf_probe(struct platform_device *pdev)
{
	struct ieee80211_hw *hw;
	struct wf_priv *priv;
	int err;

	if (!wf_chip_ok(&pdev->dev))
		return -ENODEV;

	hw = ieee80211_alloc_hw(sizeof(*priv), &wf_ops);
	if (!hw) {
		dev_err(&pdev->dev, "ieee80211_alloc_hw failed\n");
		return -ENOMEM;
	}
	priv = hw->priv;

	priv->wmac = ioremap(WF_WLAN_PHYS, WF_WLAN_SIZE);
	if (!priv->wmac) {
		dev_err(&pdev->dev, "ioremap of WMAC space failed\n");
		ieee80211_free_hw(hw);
		return -ENOMEM;
	}

	hw->queues = 4;
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_AP) |
				     BIT(NL80211_IFTYPE_STATION);
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &wf_band_2g;

	err = ieee80211_register_hw(hw);
	if (err) {
		dev_err(&pdev->dev, "ieee80211_register_hw: %d\n", err);
		iounmap(priv->wmac);
		ieee80211_free_hw(hw);
		return err;
	}

	platform_set_drvdata(pdev, hw);
	dev_info(&pdev->dev, "RTL8197F WLAN registered\n");
	return 0;
}

static void wf_remove(struct platform_device *pdev)
{
	struct ieee80211_hw *hw = platform_get_drvdata(pdev);
	struct wf_priv *priv;

	if (!hw)
		return;
	priv = hw->priv;

	ieee80211_unregister_hw(hw); /* drv_stop -> our stop() */
	iounmap(priv->wmac);
	ieee80211_free_hw(hw);
}

static struct platform_driver wf_driver = {
	.probe	= wf_probe,
	.remove	= wf_remove,
	.driver	= {
		.name = WF_DRV_NAME,
	},
};

static int __init wf_mod_init(void)
{
	int err;

	err = platform_driver_register(&wf_driver);
	if (err)
		return err;

	wf_pdev = platform_device_register_simple(WF_DRV_NAME, -1,
						  wf_wlan_resources,
						  ARRAY_SIZE(wf_wlan_resources));
	if (IS_ERR(wf_pdev)) {
		platform_driver_unregister(&wf_driver);
		return PTR_ERR(wf_pdev);
	}
	return 0;
}

static void __exit wf_mod_exit(void)
{
	platform_device_unregister(wf_pdev);
	platform_driver_unregister(&wf_driver);
}

module_init(wf_mod_init);
module_exit(wf_mod_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Netis WF2409E v4 RTL8197F WLAN clean-room driver (M2)");

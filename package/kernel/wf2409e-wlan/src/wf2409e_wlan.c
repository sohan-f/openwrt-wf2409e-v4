// SPDX-License-Identifier: GPL-2.0-only
/*
 * wf2409e_wlan.c - Netis WF2409E v4 (RTL8197F) clean-room WLAN driver, M1.
 *
 * Milestone scope: identify the silicon, run the stock 8197F power-on
 * sequence through our own PWR engine, and register a non-communicating
 * mac80211 device. M2 adds MAC/BB/RF init sheets, M3 descriptor rings
 * and real TX/RX. Per-constant evidence: docs/wlan-re.md.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <net/mac80211.h>

#include "wf2409e_wlan.h"

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

static void wf_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *ctx,
		  struct sk_buff *skb)
{
	ieee80211_free_txskb(hw, skb); /* no TX path until M3 */
}

static int wf_start(struct ieee80211_hw *hw)
{
	struct wf_priv *priv = hw->priv;
	int err;

	err = wf_pwr_run(priv, &hw->wiphy->dev, wf_card_enable_flow);
	if (err)
		return err;

	dev_info(&hw->wiphy->dev, "card-enable sequence complete\n");
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
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 10,  .bitrate = 10 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 20,  .bitrate = 20 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 55,  .bitrate = 55 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 110, .bitrate = 110 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 60,  .bitrate = 60 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 90,  .bitrate = 90 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 120, .bitrate = 120 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 180, .bitrate = 180 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 240, .bitrate = 240 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 360, .bitrate = 360 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 480, .bitrate = 480 },
	{ .flags = IEEE80211_RATE_SUPPORTED, .hw_value = 540, .bitrate = 540 },
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
MODULE_DESCRIPTION("Netis WF2409E v4 RTL8197F WLAN clean-room driver (M1)");

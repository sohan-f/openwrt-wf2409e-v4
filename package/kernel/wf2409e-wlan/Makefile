include $(TOPDIR)/rules.mk

PKG_NAME:=wf2409e-wlan
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=src/wf2409e_wlan.c

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/wf2409e-wlan
  SUBMENU:=Wireless Drivers
  TITLE:=Netis WF2409E v4 RTL8197F WLAN (clean-room)
  DEPENDS:=@TARGET_rtl819x_rtl8197f +kmod-cfg80211 +kmod-mac80211
  FILES:=$(PKG_BUILD_DIR)/wf2409e_wlan.ko
  AUTOLOAD:=$(call AutoLoad,60,wf2409e_wlan)
endef

define KernelPackage/wf2409e-wlan/description
  Clean-room mac80211 driver for the RTL8197F integrated 2.4 GHz radio
  (Netis WF2409E v4). Hardware constants verified against the stock
  Linux-3.10.90 kernel binary; the vendor rtl8192cd tree stays in-tree
  as reference. Evidence: docs/wlan-re.md.
endef

# mac80211 ships as a backport package; its staged headers are the only
# place the <net/mac80211.h> API for the driver build lives.
STAMP_CONFIGURED_DEPENDS := \
	$(STAGING_DIR)/usr/include/mac80211-backport/backport/autoconf.h

NOSTDINC_FLAGS := \
	$(KERNEL_NOSTDINC_FLAGS) \
	-I$(STAGING_DIR)/usr/include/mac80211-backport/uapi \
	-I$(STAGING_DIR)/usr/include/mac80211-backport \
	-I$(STAGING_DIR)/usr/include/mac80211/uapi \
	-I$(STAGING_DIR)/usr/include/mac80211 \
	-include backport/backport.h

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" \
		NOSTDINC_FLAGS="$(NOSTDINC_FLAGS)" \
		modules
endef

$(eval $(call KernelPackage,wf2409e-wlan))

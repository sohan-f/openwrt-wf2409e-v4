#!/bin/sh
# Build a minimal wired-only OpenWrt image for the Netis WF2409E v4.
#
# Base:   ggbruno/openwrt @ 8a0ccb93 (branch Realtek, kernel 4.14, mipsel)
# Overlay: ADCDS/openwrt-dlink-dir842-r1 @ 229536d (RTL8197F target + driver)
# Then:   this directory's files/ + patches/
#
# Use a Debian 11 (bullseye) host or container. The 2020 tree does not build
# with current host tools.
set -eu

SELF=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD_DIR:-$SELF/build}
OWRT=$BUILD/openwrt
ADCDS=$BUILD/adcds

if [ -e "$OWRT" ]; then
	echo "ERROR: $OWRT already exists; remove it first." >&2
	exit 1
fi
mkdir -p "$BUILD"

git clone https://github.com/ggbruno/openwrt "$OWRT"
git -C "$OWRT" checkout 8a0ccb93f3431bcf8f5c5d03d4acc2c8e442de67

git clone https://github.com/ADCDS/openwrt-dlink-dir842-r1 "$ADCDS"
git -C "$ADCDS" checkout 229536d89c25a676417717304fcef386a53a7875

# Keep downloads outside $OWRT so a CI cache of $BUILD/dl never makes the
# "OWRT already exists" check fire before the clone.
mkdir -p "$BUILD/dl"
rm -rf "$OWRT/dl"
ln -s "$BUILD/dl" "$OWRT/dl"

cp -a "$ADCDS/files/." "$OWRT/"

# The ADCDS overlay is a DIR-842 R1 port. These files program the external
# RTL8367S, its 5 GHz radio, or DIR-842-specific first-boot state; none of
# them may run on the internal-switch-only, wired-only WF2409E.
for f in \
	etc/config/dir842 \
	etc/sysctl.d/30-hwnat.conf \
	etc/sysctl.d/31-dir842-lowmem.conf \
	etc/uci-defaults/99-dir842-m5 \
	etc/init.d/asic-wifi-settle \
	etc/init.d/dir842-asic \
	etc/init.d/dir842-l2flush \
	etc/uci-defaults/09_wireless-dualband-dir842 \
	etc/uci-defaults/10_wireless-5g-ifname-dir842 \
	etc/uci-defaults/11_dir842-htmode-ht20 \
	etc/uci-defaults/97-dir842-l2flush-enable \
	etc/uci-defaults/98-asic-wifi-settle-enable \
	lib/netifd/wireless/rtl8192cd.sh
do
	rm -f "$OWRT/target/linux/realtek/base-files/$f"
done

cp -a "$SELF/files/." "$OWRT/"

for p in "$SELF"/patches/*.patch; do
	echo ">>> applying $(basename "$p")"
	patch -d "$OWRT" -p1 < "$p"
done

cp "$SELF/seed.config" "$OWRT/.config"
make -C "$OWRT" defconfig

awk '/^CONFIG_TARGET_realtek_rtl8197f_DEVICE_WF2409EV4=y$/{f=1} END{exit !f}' \
	"$OWRT/.config" || {
	echo "ERROR: WF2409EV4 profile was not selected." >&2
	exit 1
}

make -C "$OWRT" -j"${JOBS:-$(nproc)}"

echo
echo "Images: $OWRT/bin/targets/realtek/rtl8197f/"
echo "Flash factory.bin at NOR offset 0x30000. Do not touch 0x0..0x30000,"
echo "0x7bc000..0x7f8000, or 0x7f8000..0x800000."

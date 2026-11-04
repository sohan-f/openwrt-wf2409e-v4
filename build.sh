#!/usr/bin/env bash
# Assemble and build the OpenWrt firmware for the Netis WF2409E v4.
#
# The tree is assembled fresh on every run (from pinned commits), so the
# local exhibit never accumulates stale state:
#   1. clone OpenWrt master at $OPENWRT_COMMIT
#   2. clone the donor RTL8197F port at $DONOR_COMMIT
#   3. apply the donor's consolidated patch + copy its rtl8192cd package
#   4. install the packages feed (sqm-scripts, tc)
#   5. apply our WF2409E-specific patches, copy our files/ overlay
#   6. seed .config, make defconfig, build
set -e
set -o pipefail

OPENWRT_COMMIT="6e9fd1c3ba6bf486a044ed9d640a77dd50b6cbc2"
DONOR_REPO="https://github.com/Putpocket/iptime-a2004mu-openwrt.git"
DONOR_COMMIT="cd3d082"

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
JOBS="$(nproc)"

echo "== workspace: $BUILD_DIR (jobs=$JOBS)"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -d openwrt/.git ]; then
	git clone --filter=blob:none https://github.com/openwrt/openwrt.git openwrt
fi
cd "$BUILD_DIR/openwrt"
if [ "$OPENWRT_COMMIT" != "$(git rev-parse HEAD)" ]; then
	echo "== openwrt checkout $OPENWRT_COMMIT"
	git checkout -f "$OPENWRT_COMMIT"
fi
# Drop leftovers that would break the pinned rebuild (dl stays for the cache).
make distclean >/dev/null 2>&1 || git clean -qfdx -e dl -e .config

cd "$BUILD_DIR"
if [ ! -d donor/.git ]; then
	git clone --filter=blob:none "$DONOR_REPO" donor
fi
cd "$BUILD_DIR/donor"
if [ "$DONOR_COMMIT" != "$(git rev-parse HEAD)" ]; then
	echo "== donor checkout $DONOR_COMMIT"
	git checkout -f "$DONOR_COMMIT"
fi

cd "$BUILD_DIR/openwrt"
echo "== apply donor patch"
git apply --check "$BUILD_DIR/donor/patches/openwrt/0001-iptime-a2004mu-rtl8197f-support.patch"
git apply "$BUILD_DIR/donor/patches/openwrt/0001-iptime-a2004mu-rtl8197f-support.patch"
cp -a "$BUILD_DIR/donor/package/kernel/rtl8192cd" package/kernel/rtl8192cd

echo "== update feeds (packages feed provides sqm-scripts)"
./scripts/feeds update -a
./scripts/feeds install -a -p packages sqm-scripts

echo "== apply WF2409E patches"
for patch in "$REPO_ROOT"/patches/*.patch; do
	echo "-- $(basename "$patch")"
	git apply --check "$patch"
	git apply "$patch"
done

echo "== copy files/ overlays"
# files/target/* lands in the target tree (board.d, uci-defaults, upgrade).
# files/etc is the rootfs overlay (sqm config template) minimally disconnected
# from the sqm-scripts default at install time.
cp -a "$REPO_ROOT/files/target/." target/
mkdir -p files/etc/config
cp -a "$REPO_ROOT/files/etc/." files/

echo "== seed config"
cp "$REPO_ROOT/seed.config" .config
make defconfig

echo "== build"
make -j"$JOBS"

echo "== collect artifacts"
OUT="$BUILD_DIR/artifacts"
mkdir -p "$OUT"
cp -a bin/targets/rtl819x/rtl8197f/*factory.bin bin/targets/rtl819x/rtl8197f/*sysupgrade.bin \
	bin/targets/rtl819x/rtl8197f/*initramfs*.bin "$OUT/" 2>/dev/null || true
(cd "$OUT" && sha256sum ./*.bin > sha256.txt)

echo "== validate the factory image against the stock loader math"
python3 "$REPO_ROOT/tools/bootloader-check.py" check "$OUT"/wf2409e*factory.bin

echo "== done"
ls -la "$OUT"

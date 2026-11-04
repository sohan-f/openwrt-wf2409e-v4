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
#	but: dl/ and staging_dir/ may be restored into build/openwrt beforehand
#	by the workflow's cache steps; those must survive the re-assembly.
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
	# A workflow cache step may have dropped dl/, staging_dir/, and the
	# host/toolchain build_dir subtrees into
	# build/openwrt before the tree existed. Carry them aside so the clone
	# into the same path succeeds, then put them back into the fresh tree.
	mkdir -p openwrt-carry
	for cached in dl staging_dir build_dir; do
		[ -d "openwrt/$cached" ] && mv "openwrt/$cached" "openwrt-carry/"
	done
	rm -rf openwrt
	git clone --filter=blob:none https://github.com/openwrt/openwrt.git openwrt
	mv openwrt-carry/* openwrt/ 2>/dev/null || true
	rmdir openwrt-carry 2>/dev/null || true
fi
cd "$BUILD_DIR/openwrt"
if [ "$OPENWRT_COMMIT" != "$(git rev-parse HEAD)" ]; then
	echo "== openwrt checkout $OPENWRT_COMMIT"
	git checkout -f "$OPENWRT_COMMIT"
fi
# distclean / git clean deliberately not run here: on CI this tree is a
# fresh clone whose only pre-existing content is the restored dl/ and
# staging_dir/ caches, and distclean deletes exactly that (staging_dir
# IS the cached toolchain; dl is the tarball cache).

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
printf '%s\n' \
	'CONFIG_DEVEL=y' \
	'CONFIG_CCACHE=y' \
	"CONFIG_CCACHE_DIR=\"$BUILD_DIR/ccache\"" >> .config
make defconfig

# Build the toolchain as a distinct phase so CI can preserve staging_dir
# before the much larger kernel and package build begins.
echo "== build host tools and cross-toolchain"
make -j"$JOBS" toolchain/install
touch staging_dir/.wf2409e-toolchain-ready

echo "== build"
if ! make -j"$JOBS"; then
	# Parallel make swallows per-package compiler output behind one summary
	# line ("ERROR: package/x failed to build"); re-run linearly and verbose
	# to surface the real error. Incremental: make only (re)builds what the
	# parallel run left incomplete, starting at the failing package.
	echo "== build failed; retrying single-threaded and verbose to capture the error"
	make -j1 V=s
fi

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

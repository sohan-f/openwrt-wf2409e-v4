#!/usr/bin/env bash
# Drive build.sh inside a GitHub Codespace.
#
# Space layout: the codespace's persisted volume is 32 GiB and is shared with
# the container root filesystem, so only ~11 GiB is usable for caches. The
# 118 GiB /tmp disk is wiped whenever the codespace stops, so the tree cannot
# live there across sessions but is the only place big enough during one.
#
# Therefore: assemble the tree on /tmp, and sync only the expensive, reusable
# pieces back to build/cache/ on the persisted volume --
#   dl/                     source tarballs
#   staging_dir/            installed host tools + cross toolchain
#   build_dir/host/         host tool build stamps/intermediates
#   build_dir/toolchain-*/  toolchain build stamps/intermediates
#   ccache/                 content-addressed compiler cache
# A restart then costs one local rsync instead of a toolchain rebuild plus a
# cold kernel/package compile.
#
# Usage: tools/codespace-build.sh [--no-cache] [--setup-only]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-/tmp/wf2409e-build}"
CACHE_DIR="${CACHE_DIR:-$REPO_ROOT/build/cache}"
TREE="$BUILD_DIR/openwrt"
CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"

PREREQS=(
	autoconf automake bc bison build-essential ca-certificates ccache cpio
	curl diffutils file flex gawk gettext git libelf-dev liblzma-dev
	libncurses-dev libsnappy-dev libssl-dev libtool patch perl pkg-config
	python3 python3-setuptools python3-yaml ripgrep rsync subversion swig time
	unzip wget xz-utils zlib1g-dev zstd
)

# The universal image ships ~5 GB of language runtimes this build never
# touches. Reclaimed on every container rebuild, hence part of setup.
SLIM_PATHS=(
	/opt/conda /usr/share/dotnet /opt/dotnet /usr/local/sdkman
	/usr/local/rubies /usr/local/php /usr/local/go /go /usr/local/hugo
	/usr/local/py-utils /usr/local/buildscriptgen /usr/local/python/3.13.8
)

install_prereqs() {
	local p missing=()
	for p in "${PREREQS[@]}"; do
		[ "$(dpkg-query -W -f='${Status}' "$p" 2>/dev/null)" = "install ok installed" ] ||
			missing+=("$p")
	done
	if [ "${#missing[@]}" -ne 0 ]; then
		echo "== installing build prerequisites: ${missing[*]}"
		sudo apt-get update -qq
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing[@]}"
	fi
	sudo rm -rf "${SLIM_PATHS[@]}"
}

restore_cache() {
	mkdir -p "$TREE" "$BUILD_DIR/ccache"
	if [ -d "$CACHE_DIR/staging_dir" ]; then
		echo "== restore toolchain"
		rsync -a "$CACHE_DIR/staging_dir/" "$TREE/staging_dir/"
	fi
	if [ -d "$CACHE_DIR/build_dir" ]; then
		echo "== restore host/toolchain build stamps"
		rsync -a "$CACHE_DIR/build_dir/" "$TREE/build_dir/"
	fi
	if [ -d "$CACHE_DIR/dl" ]; then
		echo "== restore downloads"
		rsync -a "$CACHE_DIR/dl/" "$TREE/dl/"
	fi
	if [ -d "$CACHE_DIR/ccache" ]; then
		echo "== restore ccache"
		rsync -a "$CACHE_DIR/ccache/" "$BUILD_DIR/ccache/"
	fi
}

save_cache() {
	local rc=$?
	set +e
	echo "== save caches to $CACHE_DIR"
	mkdir -p "$CACHE_DIR"
	if [ -d "$TREE/staging_dir" ]; then
		rsync -a --delete "$TREE/staging_dir/" "$CACHE_DIR/staging_dir/"
	fi
	if [ -d "$TREE/build_dir/host" ]; then
		rsync -a --delete "$TREE/build_dir/host/" "$CACHE_DIR/build_dir/host/"
	fi
	local d
	for d in "$TREE"/build_dir/toolchain-*; do
		[ -d "$d" ] || continue
		rsync -a --delete "$d/" "$CACHE_DIR/build_dir/$(basename "$d")/"
	done
	if [ -d "$TREE/dl" ]; then
		rsync -a "$TREE/dl/" "$CACHE_DIR/dl/"
	fi
	if [ -d "$BUILD_DIR/ccache" ]; then
		rsync -a "$BUILD_DIR/ccache/" "$CACHE_DIR/ccache/"
	fi
	if [ -d "$BUILD_DIR/artifacts" ]; then
		rsync -a "$BUILD_DIR/artifacts/" "$CACHE_DIR/artifacts/"
	fi
	du -sh "$CACHE_DIR" 2>/dev/null || true
	df -h "$CACHE_DIR" | tail -1
	exit "$rc"
}

case "${1:-}" in
--setup-only)
	install_prereqs
	exit 0
	;;
--no-cache)
	NO_CACHE=1
	;;
esac

install_prereqs

if [ -z "${NO_CACHE:-}" ]; then
	restore_cache
	# Cache saving is the point of this script: run it even when the build
	# fails, otherwise one bad patch costs the whole toolchain again.
	trap save_cache EXIT
fi

echo "== workspace: tree=$BUILD_DIR cache=$CACHE_DIR"
mkdir -p "$BUILD_DIR"
FORCE_UNSAFE_CONFIGURE=1 CCACHE_MAXSIZE="$CCACHE_MAXSIZE" \
	BUILD_DIR="$BUILD_DIR" "$REPO_ROOT/build.sh"

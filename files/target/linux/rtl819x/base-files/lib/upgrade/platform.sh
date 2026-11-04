#!/bin/sh
# Netis WF2409E v4 sysupgrade.
#
# There is no single firmware mtd on this board: the kernel lives at flash
# 0x30000 (mtd "kernel", 0x2d0000 bytes) and the rootfs at 0x300000 (mtd
# "rootfs", 0x4f8000 bytes). The upgrade split is done explicitly with
# strictly partition-bounded mtd writes; bootloader 0x0..0x20000, config
# 0x20000..0x30000, netis 0x7f8000..0x7fc000 and reserved 0x7fc000..end
# are never written (the per-unit MAC lives at netis offset 0x3d).

. /lib/functions.sh
. /lib/functions/system.sh

PART_NAME=firmware
KERNEL_BYTES=2949120			# 0x2d0000: kernel slot size

platform_check_image() {
	local board=$(board_name)
	local magic

	case "$board" in
	wf2409e-v4)
		magic=$(get_magic_long "$1")
		if [ "$magic" != "63733663" ]; then
			echo "Invalid image: expected a cs6c kernel header, got 0x$magic"
			return 1
		fi
		return 0
		;;
	esac
	return 0
}

platform_do_upgrade() {
	local board=$(board_name)
	local kernel_mtd rootfs_mtd

	case "$board" in
	wf2409e-v4)
		[ "$(get_magic_long "$1")" = "63733663" ] || {
			echo "platform_do_upgrade: cs6c magic missing"
			return 1
		}

		kernel_mtd=$(find_mtd_index kernel)
		rootfs_mtd=$(find_mtd_index rootfs)
		[ -n "$kernel_mtd" ] && [ -n "$rootfs_mtd" ] || {
			echo "platform_do_upgrade: kernel/rootfs mtd missing"
			return 1
		}

	# Kernel first so an interrupted run leaves the OLD (bootable) squashfs
	# paired with a partially-flashed kernel slot; the loader checksum is
	# host-visible (bootloader-check.py) but the old kernel keeps the box
	# reachable one boot longer than the reverse order does.
		dd if="$1" of=/tmp/kernel.mtd bs=65536 count=45 2>/dev/null
		dd if="$1" of=/tmp/rootfs.mtd bs=65536 skip=45 2>/dev/null
		sync

		mtd -f write /tmp/kernel.mtd "/dev/mtd$kernel_mtd"
		sync
		mtd -f write /tmp/rootfs.mtd "/dev/mtd$rootfs_mtd"
		sync
		rm -f /tmp/kernel.mtd /tmp/rootfs.mtd
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}

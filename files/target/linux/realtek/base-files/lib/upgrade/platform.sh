#
# Copyright (C) 2011 OpenWrt.org
#

. /lib/functions.sh
. /lib/functions/system.sh
. /lib/realtek.sh

PART_NAME=firmware

platform_check_image() {
	local board=$(board_name)

	case "$board" in
	gwr1200ac-v1)
		# DIR-842 R1: the on-flash firmware is a Realtek "cr6b" image
		# (16-byte header the v3.4.11B loader validates + boots). Reject
		# anything without that signature so a wrong-target image can't be
		# written. The header is at offset 0 even when sysupgrade metadata
		# is appended (metadata rides at the tail), so offset-0 is robust.
		local magic
		magic=$(get_magic_long "$1")
		if [ "$magic" != "63723662" ]; then
			echo "Invalid image: expected cr6b firmware header, got 0x$magic"
			return 1
		fi
		return 0
		;;
	wf2409e-v4)
		# The WF2409E image is a raw cs6c kernel at 0x30000 plus a raw
		# squashfs at 0x300000. There is no single "firmware" mtd, so the
		# generic default_do_upgrade() would write to the wrong place.
		echo "WF2409E v4: sysupgrade is not supported yet; write factory.bin at 0x30000 from the bootloader."
		return 1
		;;
	esac

	return 0
}

platform_do_upgrade() {
	default_do_upgrade "$1"
}

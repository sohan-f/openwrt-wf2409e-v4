#!/usr/bin/env python3
"""Re-implement the WF2409E v4 stock bootloader's image checks.

Stage3 of the stock loader (see REQUIREMENTS.md, "Stock bootloader
validation") validates a system image and, for `cr6c` only, a vendor
rootfs trailer. Run this on a factory.bin before flashing; a pass here is
the same arithmetic the bootloader performs, not an approximation.

usage: bootloader-check.py <factory.bin> [--rootfs-offset N]
"""

import argparse
import struct
import sys

SYSTEM_SIGS = {b"cs6c": 1, b"cr6c": 2}
ROOTFS_MAGIC = (b"hsqs", b"sqsh")
HEADER_LEN = 0x10
# check_rootfs_image adds this to the u32 at rootfs+8 before summing:
# 0x280 bytes of trailer plus the 2-byte checksum itself.
ROOTFS_TRAILER = 0x282


def be16_sum(data):
    if len(data) & 1:
        data += b"\0"
    total = 0
    for (word,) in struct.iter_unpack(">H", data):
        total = (total + word) & 0xFFFF
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--rootfs-offset", type=lambda s: int(s, 0), default=0x2D0000)
    args = ap.parse_args()

    blob = open(args.image, "rb").read()
    ok = True

    if len(blob) < HEADER_LEN:
        sys.exit("fail: image shorter than a cvimg header")

    sig, start, burn, length = struct.unpack(">4sIII", blob[:HEADER_LEN])
    print(f"header      sig={sig!r} start={start:#010x} burn={burn:#010x} len={length:#x}")

    kind = SYSTEM_SIGS.get(sig)
    if kind is None:
        print("fail: signature is neither cs6c nor cr6c; the loader prints "
              "'no sys signature at %X!' and reboots")
        ok = False
    elif len(blob) < HEADER_LEN + length:
        print(f"fail: payload truncated ({len(blob) - HEADER_LEN:#x} of {length:#x} bytes)")
        ok = False
    else:
        total = be16_sum(blob[HEADER_LEN:HEADER_LEN + length])
        print(f"payload     be16 sum over {length:#x} bytes = {total:#06x}"
              f"  {'ok' if total == 0 else 'FAIL'}")
        ok &= total == 0

    rootfs = blob[args.rootfs_offset:args.rootfs_offset + 4]
    if rootfs not in ROOTFS_MAGIC:
        print(f"fail: no squashfs magic at {args.rootfs_offset:#x} (found {rootfs!r})")
        ok = False

    if kind == 2:
        # cr6c: check_rootfs_image() byte-swaps the u32 at rootfs+8, so it is
        # big-endian. A plain squashfs has mkfs_time there, so this sums tens
        # of MB of flash and always fails. OpenWrt cannot satisfy it without
        # reserving a vendor trailer inside the writable overlay.
        declared = struct.unpack(">I", blob[args.rootfs_offset + 8:args.rootfs_offset + 12])[0]
        region = declared + ROOTFS_TRAILER
        if args.rootfs_offset + region > len(blob):
            total = -1
        else:
            total = be16_sum(blob[args.rootfs_offset:args.rootfs_offset + region])
        print(f"rootfs      cr6c -> loader checks {region:#x} bytes, sum={total}")
        if total != 0:
            print("fail: cr6c makes the loader validate a vendor rootfs trailer; "
                  "this image has none, so the loader refuses to boot")
            ok = False
    elif kind == 1:
        print("rootfs      cs6c -> loader skips the vendor rootfs check")

    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

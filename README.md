# Netis WF2409E v4 — first OpenWrt port

Wired-only, SSH-only first port for the RTL8197F Netis WF2409E v4. WiFi is
deliberately deferred. Requirements and the evidence behind every value are in
`REQUIREMENTS.md`.

There is no official OpenWrt support for this board. This tree targets the
stock bootloader and flash layout; it does not replace the bootloader.

## Provenance

The reference dumps are from a **friend's** working WF2409E v4. The user's own
MTD blocks are not reliable. Board/model values come from the reference; the MAC
must come from the router's own `netis` block at runtime. Never bake the
reference MAC into the image.

## Base tree and overlay

- Base: `ggbruno/openwrt` @ `8a0ccb93f3431bcf8f5c5d03d4acc2c8e442de67`
  (kernel 4.14, `mipsel_24kc_musl`).
- Overlay: `ADCDS/openwrt-dlink-dir842-r1` @
  `229536d89c25a676417717304fcef386a53a7875` (`files/`).
- `files/` and `patches/` at the repo root are applied on top.

`build.sh` does the whole sequence, including pruning the DIR-842-specific
base-files that must not run on this board.

## Flash layout

8 MiB Winbond W25Q64JV. The OpenWrt image starts at `0x30000` and ends at
`0x7bc000`; everything outside that window is preserved.

| label | offset | size | notes |
|---|---|---|---|
| `boot` | `0x0` | `0x20000` | stock bootloader, read-only |
| `config` | `0x20000` | `0x10000` | COMPHS/COMPDS/COMPCS, read-only |
| `kernel` | `0x30000` | `0x2d0000` | `cs6c` loader image |
| `rootfs` | `0x300000` | `0x4bc000` | squashfs + `rootfs_data` |
| `factory` | `0x7bc000` | `0x3c000` | possible per-unit data, read-only |
| `netis` | `0x7f8000` | `0x4000` | MAC/COMPHS, read-only |
| `reserved` | `0x7fc000` | `0x4000` | untouched |

`rootfs` is `mtdblock3`. `mtdsplit_squashfs` splits it into `rootfs` +
`rootfs_data`, so later partitions shift by one. Use `find_mtd_part netis`,
never a fixed index.

## Ethernet

The WF2409E uses the SoC's internal RTL865x switch; there is no external
RTL8367S. All five jacks are switch ports and the CPU is port 8.

| VLAN | members | egress |
|---|---|---|
| 1 (WAN) | jack 4 + CPU 8 | jack untagged, CPU tagged |
| 2 (LAN) | jacks 0–3 + CPU 8 | jacks untagged, CPU tagged |

Linux sees `eth0.1` (WAN) and `eth0.2` (LAN). The CPU port stays tagged so the
RX path keeps the VLAN identity; the physical jacks must be untagged.

WAN = switch port 4 is taken from the vendor SDK (`RTL_WANPORT_MASK` is `0x10`
in the default branch and `0x110` when the CPU port is included). This has not
been confirmed with a link on the physical jack.

## Image

The recipe lives in `patches/0001-image-makefile.patch` (inserted inside the
`rtl8197f` subtarget). It produces one `factory.bin`:

- `0x00000` relative: `cs6c` kernel/loader image, padded to `0x2d0000`
- `0x2d0000` relative (`0x300000` absolute): raw squashfs rootfs
- total capped at `7728k` (`0x78c000`), so `factory` and `netis` are untouched

The loader is linked at `0x81000000`, relocates itself from `0x80a00000`
(matching the stock header's start address), decompresses to `0x80000000`,
and jumps.

## Patches

- `0001-image-makefile.patch` — `Build/cvimg-kernel-cs6c` and
  `Device/WF2409EV4`.
- `0002-disable-wifi.patch` — disables `CONFIG_RTL8197F_WMAC` and
  `CONFIG_RTL8192CD` for the wired-only build.
- `0003-internal-switch-only.patch` — adds the DT property
  `realtek,no-external-switch`; `rtl819x-eth.c` then skips the DIR-842
  external RTL8367S RGMII-trunk bring-up and selects the internal-switch
  egress-untag masks. The internal VLAN/PVID/swNic path is left intact.

`CONFIG_RTL8366_SMI=y` and `CONFIG_RTL8367B_PHY=y` must stay enabled: the
driver still links against `rtl8367s_cpu_tag_enable()` and
`rtl8367s_trunk_pause_set()`.

## Build

```sh
./build.sh
```

Use Debian 11 (bullseye) or its container. The 2020 tree does not build with
current host tools (its prerequisite check only accepts GCC 4.8–10). The GitHub
Actions workflow `.github/workflows/build-wf2409e.yml` runs the same script and
uploads the image.

Output:

```
build/openwrt/bin/targets/realtek/rtl8197f/*-WF2409EV4-squashfs-factory.bin
```

## Seed config

`seed.config` selects the target, the `WF2409EV4` profile, squashfs, dropbear,
firewall, dnsmasq, and the GPIO button/LED modules. LuCI, uhttpd, and `swconfig`
are not included.

## First flash

Run `tools/bootloader-check.py` on the image first; it re-implements the two
checks the stock loader performs (see `REQUIREMENTS.md`, "Stock bootloader
validation").

The vendor web UI (`netis.cgi`) accepts only a vendor image containing `cr6c`,
`COMPCS`, and `COMPDS` tags and writes it to `/dev/mtdblock0` from offset
`0x10`; it is not a path for this raw image.

A first flash therefore needs either:

- manual `mtd` writes from the vendor shell, writing this image at `0x30000`
  and leaving `0x0..0x30000` and `0x7bc000..0x800000` alone, or
- an external SPI programmer.

Back up all 8 MiB of NOR before any write. `platform.sh` rejects sysupgrade on
this board, so later updates need the same manual path.

Stage3 of the stock loader is not minimal: it carries a command interpreter
(`HELP`, `DB`, `DW`, `EB`, `EW`, `CMP`, `MEMCPY`, `IPCONFIG`, `AUTOBURN`,
`LOADADDR`, `J`, `FLI`, `FLR`, `FLW`, `ERASESECTOR`, `ERASECHIP`, `TI`,
`XMOD`) at `0x80011680`. `0x8000e2d0` reaches it when the byte `m` is pending
in the UART. That would allow a serial-only recovery/update path, but the exact
key timing has not been verified on hardware.

## Unverified before first flash

- **First-flash path** as above. The stock loader does have a command mode, but
  the way into it is unverified (see above).
- **Serial console** is the only boot diagnostic: 38400 8N1 on the board's
  UART pads. Without it a failed boot is indistinguishable from a bad write.
- **Driver patch 0003.** Skipping the external RGMII trunk is a hypothesis. If
  LAN/WAN is dead on first boot, check whether the internal switch needs part of
  the skipped block.
- **WAN jack mapping.** Port 4 comes from the vendor SDK masks, not from a live
  link test.
- **MAC assignment.** WAN = `netis` offset 61, LAN = WAN + 1 is the ADCDS
  convention. The sticker is WAN + 2. Confirm against the unit's label.
- **WiFi and sysupgrade** are not provided.

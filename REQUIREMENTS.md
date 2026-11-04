# WF2409E v4 OpenWrt port — requirements and extracted values

Status: first-port draft, 2026-11-03. This file is the durable record.

## Evidence rules

- The only working firmware dumps are the **friend's** unit:
  `dumps/friend-wf2409e-v4-20260424/`.
- The user's own MTD blocks are **not reliable** (user statement, 2026-11-03).
  `flash-original.bin` is not byte-identical to the friend's dumps and must
  not be used as a per-unit source.
- Board/model values may come from the friend's working firmware. Per-unit
  values (MAC, 2.4 GHz RF calibration) **must be read from the router's own
  `netis` block at runtime** and must never be baked into the image.
- No live writes during collection. Live read-only queries are the only way
  to confirm the user's own unit.

## Base tree and overlay

- Base: `ggbruno/openwrt` @ `8a0ccb93f3431bcf8f5c5d03d4acc2c8e442de67`
  (branch `Realtek`, kernel 4.14, gcc 8.4 / musl).
- Overlay: `ADCDS/openwrt-dlink-dir842-r1` @
  `229536d89c25a676417717304fcef386a53a7875` (`files/`), which supplies the
  RTL8197F target, `rtl819x-eth`, `cvimg`/`mtdsplit_cvimg`, and the vendor
  2.4 GHz driver.
- Upstream `realtek` target is RTL838x/930x switches only; do not use it.

## Endianness / architecture (resolved)

- Vendor bootloader and vendor kernel are **little-endian MIPS**:
  capstone decodes 5,856 LE instructions vs 2 BE at kernel offset 0x1000.
- Base tree `target/linux/realtek/rtl8197f/target.mk` sets
  `ARCH:=mipsel`, `CPU_TYPE:=24kc`, overriding the top-level `ARCH:=mips`.
  Toolchain is `mipsel_24kc_musl`.

## DTS requirements (`WF2409EV4.dts`)

- `compatible = "netis,wf2409e-v4", "realtek,rtl8197f-soc"`;
  `model = "Netis WF2409E v4"`.
- Memory: 64 MiB, `reg = <0x0 0x4000000>`.
- Console: `console=ttyS0,38400` (UART at `0x18147000`).
- Ethernet: inherit `ethernet@18010000` from `RTL8197F.dtsi`
  (`realtek,rtl8197f-eth`, CPU interrupt 4). Do **not** add an external
  RTL8367 node; the WF2409E uses the internal RTL865x switch.
- `&ethernet { realtek,no-external-switch; };` is required by patch 0003.
- Internal switch model:
  - LAN = VLAN 2, member mask `0x10F` (jacks 0-3 + CPU port 8),
    egress untag mask `0x00F`
  - WAN = VLAN 1, member mask `0x110` (jack 4 + CPU port 8),
    egress untag mask `0x010`
  - Physical jacks egress untagged; the CPU port (8) stays tagged so RX keeps
    the VLAN identity.
  - Linux interfaces: `eth0.2` (LAN), `eth0.1` (WAN).
- WAN = jack 4 is from the vendor SDK `rtl865x_netif.h`:
  `RTL_WANPORT_MASK` is `0x10` in the default branch and `0x110` when the CPU
  port is included (`CONFIG_RTL_PUBLIC_SSID` / `CONFIG_RTK_INBAND_HOST_HACK`).
  The vendor `rtl_set_wanport_portmapping()` also clears `1 << RTL_CPU_PORT`
  from `untagSet`, which confirms tagged CPU egress is a supported mode.
- PCIe: disable `&pcie0` (no 5 GHz radio).
- WMAC: keep the DT node disabled. `rtl8192cd` accesses the block at its fixed
  address and lifts the `SR+0x64` gate itself; the `rtl8197f-wmac` bring-up
  driver is not used.

### GPIO pins (confirmed from the vendor kernel)

Disassembled `rtl819x_gpio_peripheral_init` at `0x805f17a4` in the vendor
kernel (base `0x80000000`):

| device | vendor symbol | pin | GPIO controller / offset |
|---|---|---|---|
| reset button | `rtl_pdata_btn1` | 19 (`C3`) | `gpio0` 19 |
| WPS button | `rtl_pdata_btn2` | 17 (`C1`) | `gpio0` 17 |
| reset / sys LED | `rtl_pdata_led1` | 15 (`B7`) | `gpio0` 15 |
| WPS LED | `rtl_pdata_led2` | 35 (`E3`) | `gpio1` 3 |

All four are active-low. The generic SDK defaults are `C3/C1/B7/B7`; the
WF2409E build overrides the WPS LED to pin 35, so do not use `B7` for WPS.

## Flash map (stock layout, with factory area preserved)

8 MiB Winbond W25Q64JV on `spi0` (`realtek,sheipa-spi`).

| label | offset | size | flags |
|---|---|---|---|
| `boot` | `0x0` | `0x20000` | read-only |
| `config` | `0x20000` | `0x10000` | read-only |
| `kernel` | `0x30000` | `0x2d0000` | writable |
| `rootfs` | `0x300000` | `0x4bc000` | writable |
| `factory` | `0x7bc000` | `0x3c000` | read-only |
| `netis` | `0x7f8000` | `0x4000` | read-only |
| `reserved` | `0x7fc000` | `0x4000` | read-only |

- `rootfs` is `mtdblock3`. `mtdsplit_squashfs` (`CONFIG_MTD_SPLIT_SQUASHFS_ROOT`,
  already `=y` in `target/linux/generic/config-4.14`) splits it into
  `rootfs` + `rootfs_data`; later partitions shift by one.
- The stock vendor rootfs region is `0x300000..0x7f8000`, but the last
  `0x3c000` is carved out as `factory` because the user's unit may hold
  COMPHS-style factory calibration at `0x7bc000..0x7f8000`. Do not format or
  erase it.
- The friend's mtd1 squashfs `bytes_used` is `0x4573b0`; a minimal OpenWrt
  rootfs must fit in `0x4bc000`.
- Stock kernel is a `cr6c` image at `0x30000`, start `0x80a00000`,
  burn `0x30000`, len `0x209002`. `cvimg -s cr6c` reproduces the format;
  checksum is 16-bit BE `~sum+1` over the payload.
- `boot` (0x0..0x20000) contains the stage2 bootloader; the gzip payload
  starts at `0x9a60`, so do not shrink it to 64 KiB.

## Stock bootloader validation (reverse-engineered)

Everything below was read out of the loader, not inferred from a log. It is
recorded here so the signature choice is never re-litigated from scratch.
`tools/bootloader-check.py` re-implements both checks; run it on any
`factory.bin` before flashing.

Unpacking stage3 (the only copy of the loader we have is the friend's `mtd0`):

- stage1 `0x0..0x7000`; stage2 at `0x8000`; gzip payload `0x9a60..0x138f4`.
- Decompressed stage3 is 103,056 bytes, loads at `0x80000000`,
  `v3.4.14b`, build `2023.12.07-10:26+0800`.

| addr | name | behaviour |
|---|---|---|
| `0x8000db2c` | `check_system_image(flash, buf)` | reads 16 B header; byte-swaps fields 4/8/0xc; `cs6c`→1, `cr6c`→2, anything else→0; then requires a zero 16-bit BE sum over `len` payload bytes |
| `0x8000de14` | `check_rootfs_image(flash)` | accepts `hsqs`/`sqsh`, reads BE u32 at +8 as a length, adds `0x282`, requires a zero 16-bit BE sum over that region |
| `0x8000e050` | `find_image` | tries `0xb0010000`, `0xb0020000`, `0xb0030000`, then scans `0xb0010000..0xb0080000` in `0x1000` steps |
| `0x8000e30c` | boot | prints `Loade addr : %X`, copies the payload to `start \| 0x20000000`, jumps to `start` |
| `0x8000e54c` | `boot_image` | normal cold-boot entry; `find_image` then `0x8000e30c` |

Why `cs6c`: `find_image` returns the `check_system_image` code. Code 2
(`cr6c`) additionally runs the rootfs validation above, and code 0 makes
`boot_image` write the watchdog and reset. OpenWrt's squashfs has `mkfs_time`
at +8 and no vendor trailer, so the region it sums is ~1.6 GB and the check
always fails — that is the "flashed, does not boot" failure. Code 1 (`cs6c`)
returns before the rootfs checks. The jump path itself never looks at the
signature: the only two references to the `cs6c`/`cr6c` strings in the whole
loader are inside `check_system_image`.

The payload checksum is still enforced for `cs6c`, and `cvimg` already emits
it, so nothing else has to change.

## Image recipe

`patches/0001-image-makefile.patch` inserts into
`target/linux/realtek/image/Makefile`:

```make
# cs6c skips the bootloader's vendor rootfs-checksum check, whose trailer
# would be overwritten by OpenWrt's writable overlay.
define Build/cvimg-kernel-cs6c
	$(call Build/cvimg, \
		-e $(CVIMG_LOADER_START_ADDR) \
		-b $(CVIMG_KERNEL_BURN_ADDR) \
		-s cs6c \
		$(1))
endef

define Device/WF2409EV4
  $(Device/RealtekDTS)
  BLOCKSIZE := 64k
  DEVICE_DTS := WF2409EV4
  DEVICE_TITLE := Netis WF2409E v4
  SUPPORTED_DEVICES := wf2409e-v4
  IMAGE_SIZE := 7728k
  DEV_PROFILE := WF2409EV4
  DEVICE_PACKAGES := kmod-leds-gpio kmod-input-gpio-keys-polled \
	-kmod-gpio-button-hotplug -swconfig
  CMDLINE := console=ttyS0,38400 root=/dev/mtdblock3 rootfstype=squashfs rootwait
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x30000
  CVIMG_LOADER_START_ADDR := 0x81000000
  IMAGES := factory.bin
  IMAGE/factory.bin = append-kernel | cvimg-kernel-cs6c | check-size 2949120 | pad-to 2949120 | append-rootfs | pad-rootfs | check-size
endef
TARGET_DEVICES += WF2409EV4
```

- `CVIMG_LOADER_START_ADDR=0x81000000` is the loader's link address. Loading
  it there skips the self-relocation path; loading it at the stock `0x80a00000`
  would also work, but the in-place path is what has been booted on hardware.
- The loader decompresses the kernel to `LOADADDR=0x80000000`, then jumps.
- `pad-to 2949120` (`0x2d0000`) puts the raw squashfs at absolute `0x300000`.
- `IMAGE_SIZE=7728k` (`0x78c000`) ends at `0x7bc000`, leaving `factory` alone.
- `check-size 2949120` fails the build if the kernel outgrows its slot;
  the final `check-size` enforces the total.

## Base-files

- `02_network`: `ucidef_set_interfaces_lan_wan "eth0.2" "eth0.1"`.
  - MAC source: `netis` offset 61 (`0x3d`), six binary bytes.
  - WAN = base MAC, LAN = base+1 (ADCDS convention); sticker is base+2.
  - A blank, all-zero, or all-FF value leaves the MAC unset; there is no
    fallback to the `config` block.
- `01_leds`: `wf2409e:green:sys` (gpio0 15), `wf2409e:green:wps` (gpio1 3).
- Buttons use `gpio-keys-polled` because the GPIO controllers have no
  interrupt support; `kmod-input-gpio-keys-polled` is selected instead of the
  default `kmod-gpio-button-hotplug`.
- `realtek.sh`: `*"WF2409E v4"*` -> `wf2409e-v4`.
- SSH only: `dropbear`; no LuCI, no uhttpd. `swconfig` is removed via
  `DEVICE_PACKAGES` because the switch is internal.
- The shipped `01_leds` and `02_network` are the ADCDS versions plus only the
  WF2409E case. Do not replace them with the older base versions: that
  silently deletes ADCDS's DIR-842 MAC/LED fixes.

## Kernel config

`patches/0002-disable-wmac-bringup.patch` disables `CONFIG_RTL8197F_WMAC`, the
bring-up driver that cannot beacon; the DT node is disabled anyway.
`CONFIG_RTL8192CD=m` is the working 2.4 GHz driver and stays enabled.

Keep `CONFIG_RTL8366_SMI=y` and `CONFIG_RTL8367B_PHY=y`: `rtl819x-eth.c`
still links against `rtl8367s_cpu_tag_enable()` / `rtl8367s_trunk_pause_set()`,
even though patch 0003 skips the calls at runtime. Without those symbols the
link fails.

Keep `# CONFIG_CRASHLOG is not set` (the crashlog handler recurses on this
SoC and masks the real oops).

## Critical driver caveat (unverified)

`rtl819x-eth.c` in ADCDS is **DIR-842-specific**: it unconditionally forces
port 0 into RGMII mode, enables CPU tags, and programs the RGMII trunk to an
external RTL8367S. The WF2409E has only the internal RTL865x switch, so
`patches/0003-internal-switch-only.patch` adds a DT property
(`realtek,no-external-switch`) that skips the whole external-trunk block.

The internal-switch part (VLAN 2/1 membership, PVIDs, CPU port 8, swNic DMA,
VLAN tag insert/strip) is left intact. **This patch has not been tested on
hardware.** If LAN/WAN is dead on first boot, the first thing to check is
whether the skipped block removed something the internal switch also needs.

## Live vendor observations (read-only)

Collected from the user's running unit, without writes:

- `/proc/cpuinfo`: `system type: RTL8197F`, `machine: Unknown`, MIPS 24Kc.
- `/proc/mtd` matches the layout above (`mtd0` `0x300000`, `mtd1` `0x4f8000`,
  `mtd2` `0x4000`).
- Switch state: one VID 9 with members ports `0 1 2 3 4 8`, all untagged;
  PVID 9 on ports 0–4 and 8, PVID 1 on ports 5–7. `br0` contains `eth0` and
  `wlan0`. CPU port 8 link is up at 1G; all jacks were down during collection.
- The unit was in AP/repeater mode, so the wired WAN path was inactive and the
  jack-to-port mapping could not be confirmed by a link test.
- Vendor `flash all` reports `hwNatLanVlanId=9`, `wanVlanId (NAT)=8`,
  `wanPortMode=0`, `lan1..3PortMode=0`. LAN/WAN separation in the vendor stack
  is done by ASIC L3 routing, not by distinct VLANs, which is why the live
  switch shows a single VID.
- Vendor rootfs has no `/sbin`, `/usr/bin`, or `/usr/sbin`; `/bin/script` is a
  directory of symlinks to the `cscripts` binary. Busybox `tar` lacks `-c`,
  `head` does not accept `-N`, and `sort`/`find` are absent.

## Build (GitHub Actions, later)

`build.sh`:

1. Clones the base at `8a0ccb9`.
2. Clones ADCDS at `229536d` and copies `files/`.
3. Copies this directory's `files/`.
4. Applies `patches/*.patch`.
5. Copies `seed.config` and runs `make defconfig`, then `make`.

Use Debian 11 (bullseye) or a container: the 2020 tree does not build with
current host tools.

Build tree and output live under `build/`:

`build/openwrt/bin/targets/realtek/rtl8197f/*-WF2409EV4-squashfs-factory.bin`.

## Open items before first flash

- **First-flash path unproven.** The stock bootloader is minimal: strings
  contain only `Booting...` and `SPI image move done ->Jump to DRAM`, with no
  TFTP/recovery client. The vendor web UI `upload_fw` accepts only an image
  containing `cr6c`, `COMPCS`, and `COMPDS` tags and writes the boot part to
  `/dev/mtdblock0` from offset `0x10`; it is not a path for this raw image.
  A first flash needs manual `mtd` writes from the vendor shell or an external
  SPI programmer. Back up all 8 MiB of NOR first.
- **Driver patch unverified** (see above).
- **MAC assignment**: WAN=base, LAN=base+1 is the ADCDS convention; confirm
  against the vendor `HW_NIC0_ADDR` usage if the user's unit labels WAN
  differently.
- **WiFi on this board**: the vendor `rtl8192cd` driver is included and proven
  on the DIR-842, but the WF2409E RF path and per-unit calibration have not been
  tested.
- **sysupgrade**: not provided yet. The separate kernel/rootfs layout needs a
  custom `platform.sh`; the first flash is a raw write at `0x30000`.

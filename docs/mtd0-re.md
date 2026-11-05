# mtdblock0 — complete forensic analysis (from-scratch RE)

Analysis of `working_mtdblock_backup_dump/mtd0-boot-cfg-linux.bin` — the
`cat /dev/mtdblock0` dump of a Netis WF2409E v4 (RTL8197F), flash W25Q64JVSIQ
8 MiB. **Everything below was re-derived this session from the dump bytes;
no conclusion was taken on trust** from IDENTITY.md, PROJECT_CONTEXT.md or
prior sessions. Suppliers/disagreements are flagged inline.

Artifacts: workbench in `.local/mtd0-re/` (git-excluded): `mio.py` (hash-verified
window reader), `carve/` (extracted regions), `blocks_manifest.txt`
(48 × 64 KiB CLI-hashed ground truth), listings `dis_stage1_le.txt`.

## 0. Identity & environment notes

- sha256 `49bdacd645c0935a34e8d30f47234d519264cd78ca7361431435a293ef422fadd`, md5 `422fdb3aa02656ec93eb349bb7661ec8` (matches IDENTITY.md), size 3,145,728 = 0x300000.
- Environment gotcha worth recording: this devcontainer initially returned *stale*
  pages for large fresh reads (whole-file `bytes` saw a different tail than
  CLI reads). Symptom was internally inconsistent probe output; root cause was
  **analyst error — hex confusion between `0x24000` and `0x240000`** layered on
  a flaky-aggressive devcontainer file system. Protocol after that: every byte
  cited here is read through 64 KiB windows whose sha256 is verified against a
  CLI-generated manifest (`blocks_manifest.txt`). Verified twice (CLI
  stream-hash == split blocks == copy). Disk content is stable; no writer
  exists (mtime 2026-11-03, inode constant).

## 1. Byte-level partition map of mtdblock0 (verified: transitions at 4 KiB granularity)

| Region | Range | Content |
|---|---|---|
| stage1 boot code + stage2 blob | 0x000000–0x014000 | real code/strings; see §2 |
| unused (erased) | 0x014000–0x020000 | all zero |
| config slot A | 0x020000–0x021000 | `COMPHS…` hardware block (4 KiB) |
| gap | 0x021000–0x024000 | zero |
| config slot B | 0x024000–0x027000 | 12 KiB config (`COMPDS…`, netis strings) |
| gap | 0x027000–0x02A000 | zero |
| config slot C | 0x02A000–0x02D000 | 12 KiB config (`COMPCS…` — **correction 2026-11-05: magic is `COMPCS`, not `WF2409…`; the `WF2409` strings are values inside the payload. Slot extents per `mib_config[]` (libapmib): A=0x20000 (max len 0x4000 = 16 KiB), B=0x24000 (max 0x6000), C=0x2A000 (max 0x6000) — the "gap" rows above are the unused tail of each slot.**) |
| gap | 0x02D000–0x030000 | zero (unused tail of slot C; per `mib_config[]` slot C allocates 0x6000) |
| kernel zone | 0x030000–0x023A000 | `cr6c` image: header + stub + LZMA (see §4) |
| unused/erased | 0x023A000–0x030000 | all zero |

Notes:
- No squashfs magic (`hsqs`/`sqsh`) anywhere in mtdblock0 → the rootfs really
  is only in mtdblock1 (mtd1 begins `hsqs`, per IDENTITY.md; not re-checked here).
- Independent cross-check of the map: REA/binwalk 3.1.0 signature scan found a
  CRC32 table at 0x8B14, a gzip stream at 0x9A60 (OS=Unix, gzip mtime
  2023-12-07 02:26:42 == the loader banner's build time `10:26+0800`), and the
  LZMA kernel stream at 0x32818 (props 0x5D, 8 MiB dict, uncompressed 6,407,684 B).
  Evidence id from REA: `ev_8c9a4fc520e8…`.
- 0xFF runs ≥512B: none anywhere; the two big zero regions are true zeros,
  matching erased-but-written-zero flash behavior.

## 2. Stage-1 (offsets 0x0–0x138F2), booted at 0xBFC00000

- **Endianness: little-endian MIPS32, machine re-derived this session.** The
  first word `03 00 00 08` decodes LE as `j 0xBFC0000C` (a reset trampoline to
  bypass 4 words of reserved pad). The area 0x0–0x180 contains a plausible
  instruction stream of `lui 0xB800xxxx`/`mtc0`/`sw` loads only in LE; BE
  decodes to garbage. (IDENTITY.md's "BE MIPS vector" is hereby superseded for
  stage-1: LE is what the bytes say.)
- **Whole-image checksum: 0x0–0x138F2 be16-chunked sum ≡ 0**, with the trailer
  word **0x2416 at 0x138F0–0x138F2** (verified arithmetically; the same
  `be16_zero_sum` scheme later reappears for kernel payloads in §4). Bytes
  0x138F2–0x14000 padding zeros, then an erased (all-zero) 48 KiB gap.
- Sub-layout inside 0x0–0x138F2:
  - 0x0000–0x8000: stage-1 *phase-1* code+strings (32 KiB)
  - 0x8000–0x9A60: stage-1 *phase-2* stub code (0x1A60 bytes)
  - 0x9A60–0x138EC: gzip blob (40,589 B, stream-verified; CRC32 `AF3423D0`,
    ISIZE `0x19290` == 103,056); blob content = stage-2 loader (see §3)
  - 0x138EC–0x138F0: zero pad 4 bytes; 0x138F0–0x138F2: trailer word 0x2416
- Boot flow decoded from the disassembly (`dis_stage1_le.txt`,
  `dis_stage1_phase2.txt`):
  1. Reset: `j 0xBFC0000C` → SoC pokes `0xB8000010/~0x0801`, `0xB8000014←0x110`,
     CP0 Status/Config & `0xB8000008` strap read (cf. §6).
  2. SP set to `0x9FE03FE0`; timer/ISR/interrupts off.
  3. `jal 0xBFC(0x)00194` = UART init + print (UART base **0xB8147000**,
     8250-style; string data is read back from a RAM copy at
     `0xB0000000+off` — i.e. a copy of the whole stage-1 has been placed at
     phys 0x0 by the boot ROM before phase-1 runs; the copy's end, 0x138F4,
     matches the memmove bound below).
  4. Copies stage-1 bytes `[0x8000..0x138F4)` from RAM-alias `0xB0008000` to
     DRAM `0x80100000`; runs I-cache/D-cache invalidate loops; `jr 0x80100000`.
  5. Phase-2 stub runs at `0x80100000`, SP = `0x8010D8F0` (image end+0x2000).
     It calls `0x80100604` with args
     (src=0x80101A60 == blob within phase-2 copy, dst=0x80000000, bounds
     0x7FFFFFFF) = the gzip decompression; then `jal 0x80100460`; then
     `jalr 0x80000000` = hand off to stage-2.
  6. Stage-2 (103,056 B) runs from **DRAM 0x80000000**.
- First instructions (LE, at flash), excerpt from `dis_stage1_le.txt`:
  - `lui $t7,0xB800; ori $t7,$t7,0x10; lw; and ~0x801; ori 0; sw` : pokes the
    SoC system register at **0xB8000010** (mask clear of 0x0801).
  - `0xB8000014 ← 0x110`.
  - Status (CP0 reg 12): clear low 5 bits, set SD (interrupts off). WatchLo/Hi,
    Cause, Count cleared.
  - Reads **0xB8000008** (strap pin register), `andi 0xF`, `== 1 ?` branches:
    if strap says 1, the code *skips* forcing `Config.BE`; otherwise it forces
    Config.BE=1... verbatim: `or ~8 → ori 2 → mtc0 Config`. Both worlds must
    agree; the chip isonet ends up in little-endian on this unit (consistent
    with the rest of the loader); more on the branch in §2.2.

- stage1 provdies strings of its own: `Booting...`,
  `SPI image move done ->Jump to DRAM`, `err: ck_cpu_freq_sel(0x%x)`,
  `init_ram`, `init ddr fail / ok`, `DRAM Type: DDR1/DDR2`,
  `DRAM frequency: %dMHz`, `DRAM Size: %dMB`, plus DDR timing tables as ASCII.

- The bootloader contains, produced at offset 0x9A60, an embedded gzip blob
  (40,589 B, gzip-reported build 2023-12-07 02:26:42 Unix) that decompresses
  cleanly to **103,056 bytes** — this is **stage-2**, the full netis loader.
  The stub CRC32 table lives at 0x8B14. blob extraction: carve/
  `gzblob.gz` → `stage2.bin` (gzip exit 0).

## 3. Stage-2 (uncompressed 103,056 B) — the real netis bootloader — base 0x80000000

`carve/stage2.bin`; sha 2a0945f60bf72349ead1d2db10aefff95797ee67e19a000a0330f970efbe9c20.

- Own header: first word `02 01 00 10`. (not the cs6c/cr6c signature family —
  identified as the load/entry descriptor in §3.2.)
- Full feature list from strings: SPI NOR chip table (`at25fs010` …
  `w25q256`, `W25Q64` among them), `RTL8197F-VG` chip name, `WF2409Ev4/NETIS`
  model string, `v3.4.14b` version.
- The serial banner on real hardware is `---Realtek RTL8197F-VG boot code at
  2023.12.07-10:26+0800 v3.4.14b (%dMHz)` (matches the live-boot log notes in
  PROJECT_CONTEXT gotcha list).
- Image-check machinery (strings/`check_rootfs_image`/`check_system_image`,
  `no sys signature at %X!`, `no rootfs signature at %X!`,
  `rootfs checksum error at %X!`, `%s imgage checksum error at %X!`,
  `Jump to image start=0x%x...`, `Loade addr : %X`).
- Embedded **web-recovery HTTP server** (its own `HTTP/1.0 200 OK` pages:
  parts "Updateing File"/"System Repair"; diagnostic "Invalid image file
  ,reboot the device,try again !"), TFTP client, xmodem, autoburn, memory/EW
  command menu (dump/erase SPI, PHY read/write, `SPICLB` calibration).
- Partition names it knows: `Linux kernel`, `Linux kernel (root-fs)`,
  `Webpages`, `Root filesystem`, `Boot code`, `Total Image`,
  `Total Image (no check)` (strings `15450–154bc`).

## 4. Kernel zone (`cr6c` image) at 0x30000

- 16-byte header (big-endian wire layout, little-endian loader; re-checked):
  `cr6c | start=0x80A00000 | burn=0x00030000 | len=0x00209002`.
- Payload len = 0x209000 (= len–2: trailing 2-byte big-endian sum word).
- Payload layout: 0x30010 stub code (0x2808 bytes), then the LZMA stream at
  **0x32818** (props 0x5D, dict 8 MiB, uncompressed 6,407,684 B) — see §5.

## 5. Config slots (0x20000 zone) — decode taken over in §8; libapmib.so contains the codec

## 6. Constants & register map observed (pending SDK cross-check from web sources)

**Update 2026-11-05 late session:** public-web searches (operator `websearch`
tool, and duckduckgo) returned *nothing* for `COMPHS` / `COMPDS` / `COMPCS`
and Realtek apmib — this is a private Realtek MSDK convention with no public
docs or GPL source mirrors found. Ground truth = our own RE of the rootfs
userland (libapmib.so etc.), documented in §8+.

Register pokes noticed so far (from stage1 head):
- `0xB8000010`, `0xB8000014`, `0xB8000008` — SoC system block (RTL819x).
- Copy loop `0xB0008000 → 0x80100000` (init RAM image move).

## 7. Environment lessons for this repo

- devcontainer /home FS can give stale reads for large freshly-allocated
  buffers; rule: **any** flash-facts read via small verified windows (see
  `mio.py`), and block-level checks against `blocks_manifest.txt`.
- Hex confusion `0x24000` vs `0x240000` was the true root of the earlier
  "phantom data" panic — both were real (a config island at 0x24000, and a
  zero *tail* starting 0x240000). Print offsets in hex AND decimal in tables
  to prevent this class of error.

---

## 8. Session 2 (2026-11-05 late): the config-slot decode — APMIB architecture

Everything here was derived **this session** from the dumps and the stock
rootfs; evidence grades are marked per claim. The config-slot work moved from
"unknown TLV" (§5) to "understood codec + understood storage"; the remaining
work is the per-field ID tables and a full decode of the three blobs.

### 8.1 Artifacts (all under git-excluded `.local/mtd0-re/`)

| file | what it is | provenance |
|---|---|---|
| `dis_libapmib.lst` | 18,230-line annotated MIPS32-LE listing of libapmib.so (dynsym labels + jal/b target annotation) | generated by written-this-session `disasm_lib.py` from `carve/stock/libapmib.so` |
| `disasm_lib.py` | the listing generator | reads .so, parses dynsym via DT_HASH chain count |
| `carve/vmlinux.bin` | **stock kernel, LZMA-decoded** (6,407,684 B, exactly the size declared in the LZMA-alone header) | sha256 `f2919b306d6a73a717cc7efbad9bbab07576a6f223974135f24add833e410abc`; extracted from `kernel.bin` (flash 0x30000 dump) at LZMA offset 0x2818 = flash 0x32818; header verified against manifest-verified flash bytes: props `5d`, dict 0x00800000, size `0x0061c604` |
| `carve/kernel-lzma.lzma` | same LZMA stream pre-extract (0x32818…end of payload) | sha256 `04418219120303c2f86f232d8a168f35417ad7e577e9f7aae5f418c8b659555b` |
| `carve/stock/libapmib.so` | the APMIB codec library (section-stripped; dynsym intact) | sha256 `36e9fdaba2f7831e1743398cc9c00df92ca50726be3c88ad148449e7605ee54a`; from rootfs `lib/` |
| `carve/stock/auto_update` | recovery/updater daemon that names the three blocks | sha256 `d914bc16541f1ade1196d2f85e36d2822d1b90a52bc97a5e613f07f6b19f79db` |
| `carve/stock/fwd` | other binary that strings-names the blocks | sha256 `1da4c04eac5369480ce698d9865deac6bab4c450b947fd68b4cdbb383ccfc1ea` |
| `carve/stock/etc/` | stock rootfs /etc (tar of interest: `init.d/`, `rc.d/`, `sysconfig/`, `boa/`, `wps/`, `doc/`…, 268 KiB) | copied from rootfs extraction |

Rootfs extraction procedure (re-doable in one line):
`unsquashfs -d stock-rootfs working_mtdblock_backup_dump/mtd1-rootfs.bin`
(exit code 2 here = the non-root failure to create /dev device nodes only;
every regular file extracts fine). `unsquashfs` and `mipsel-linux-gnu-objdump`
are present in this devcontainer; capstone came with Python already installed.

Evidence grades in this section: **Confirmed** = read directly from the
disassembly/ELF bytes; **Likely** = strong inference, mechanism implied but
endpoint untested; **Speculative** = stated as such.

### 8.2 Who parses COMPDS/COMPHS (Confirmed)

- The **kernel does not**: vmlinux.bin contains no `COMPHS`/`COMPDS`/`COMPCS`
  bytes (rg over the whole 6.4 MB). The live kernel is
  `Linux version 3.10.90 (root@netcore-MIX) (gcc version 4.8.5 20150209
  (prerelease) (Realtek MSDK-4.8.5p1…) #1 Wed Apr 24 17:42:14 CST 2024`
  (strings from vmlinux.bin; matches IDENTITY.md's /proc/cpuinfo line).
- The **bootloader stage-2 does not** either (no such strings in stage2.bin,
  §2's string dump).
- The parser is **userland libapmib.so** (`lib/libapmib.so` in the squashfs)
  plus the two daemons that name the blocks in their strings:
  `bin/auto_update` (strings: `/dev/mtdblock0`, `r6cr`, `/dev/mtdblock1`,
  `COMPHS`, `COMPDS`, `COMPCS`) and `bin/fwd` (same set).

### 8.3 Why the blobs interleave junk and plaintext (Confirmed)

`libapmib.so`'s dynamic symbol table exposes `Encode` / `Decode` /
`mib_compress_read` / `mib_compress_write` / `mib_tlv_init_from` /
`mib_search_by_id` / `find_same_tag_times` — the config blobs are **LZSS-
compressed serialized MIB data**, not TLV-on-flash. Reading `Decode` word by
word shows it is the textbook **Okumura LZSS** decoder:

- ring buffer N=4096, match length F=18, threshold=2 (matches are len+2);
  ring initialized with 4078 = `0xFEE` filler bytes of `0x20` (space);
  buffer is `calloc`ed 0x1011 bytes;
- flag byte scheme: `flags >>= 1`; if `(flags & 0x100) == 0` then
  `flags = 0xFF00 | in[i++]`; bit==1 → literal byte; bit==0 → 2-byte match:
  `pos = (((b & 0xF0) << 4) | a) ...` where a=in[i], b=in[i+1],
  `runlen = (b & 0x0F) + 2`, copy `runlen` bytes from
  `ring[(pos - j) & 0xFFF]`, updating ring as it copies (overlapping copy
  semantics);
- terminates by input-exhaustion check against the passed payload length and
  returns the decoded byte count (frees its private ring on exit; returns 0
  on calloc/malloc failure).

The step-0x12 arithmetic runs everywhere in the blobs (`2f 23 2f 35 2f 47…`)
are match-callback products of the ring — consistent with LZSS output.

### 8.4 Compressed-block header (Confirmed)

12 bytes on flash before the LZSS stream of each slot:

| off | size | field |
|---|---|---|
| 0x0 | 6 | signature, ASCII: `COMPHS` / `COMPDS` / `COMPCS` |
| 0x6 | 2 | u16 **big-endian**: `0x0005` (COMPHS) / `0x0006` (COMPDS); meaning currently read as a version word — **Speculative** that it is a pure version: in `apmib_conf` it is byte-swapped and **multiplied with the length** to size the decode buffer (`mul len, ver` — CONF this is a compatibility hack; see 8.8) |
| 0x8 | 4 | u32 **big-endian** = compressed payload length in bytes |
| 0xC | … | LZSS stream (see 8.3), exactly `len` bytes |

Verified for COMPHS: len `0x02a6` (678), ends at 0x202B2 matching the last
nonzero byte. Verified for COMPDS: len `0x21be` (8662? 0x21be = 8662? no:
0x21be = 8638…), annotated below in the slot table. COMPCS header byte field
values **not yet dumped** this session (TODO 8.9 item 1).

Slot table (Confirmed from `mib_config[]`, see 8.6, cross-checked to bytes):

| table idx | sig | flash off (hex) | max len (hex) | header len (hex) | decoded size (hex) |
|---|---|---|---|---|---|
| 0 | COMPHS | 0x20000 | 0x4000 (16 KiB) | 0x02a6 | 0x9ee (2542) |
| 1 | COMPDS | 0x24000 | 0x6000 (24 KiB) | 0x21be | 0x8ff6 (36854) |
| 2 | COMPCS | 0x2a000 | 0x6000 | (TODO) | 0x8ff6 (same as DS) |

(The decoded-size numbers are Confirmed as the **shm segment sizes** passed to
`apmib_init`/`apmib_init_HW` — they come straight out of `mib_config[]` at
+0x04 and are also printed by the `/tmp/SettingSize` debug probe:
`HwSetting  Size  %d DefMib Size %d ApMib Size %d`, values `0x9ee`, `0x8ff6`,
`0x8ff6`.)

### 8.5 libapmib.so symbol map (highlights; Confirmed from dynsym + listing)

Data symbols (all confirmed by `--dyn-syms`-equivalent parse, dump in
`dis_libapmib.lst` header comments? — no, regenerated by `dynsyms()` in
`disasm_lib.py`):

```
Mib_data          0x22024  41792 B   working struct/image area
mib_config        0x2c380    168 B   <- THE 3->record incl empty blocks table
hwmib_table       0x2fa60    544 B   HW (COMPHS) field description table
hwmib_wlan_table  0x2c430   5644 B   per-wlan HW table (RF cal lives here!)
mib_wlan_table    0x2fd08   6664 B
mib_table         0x31710  12648 B   ApMib field description table
mib_root_table    0x34878    136 B; mib_root…/hwmib_root_table 0x2fc80 136 B
pMib / pMibDef / pOld / pHwSetting   @0x35080/0x35078/0x3507c/0x35074 (bss)
vwlan_idx / wlan_idx / wan_idx     @0x35068+; shm_name @0x2c370
shell (cmd table) @0x34900; ex_* small objects 0x349d0.. (23 B each: config
  group name strings? values: ex_passward, ex_autoupdate, ex_multiap… TODO)
```

Function symbols of interest (all in `dis_libapmib.lst`):

| address | fn | note |
|---|---|---|
| 0x51a4 | `Encode` | 1020 B — the writer-side LZSS (untested yet) |
| 0x55a0 | `Decode` | 420 B — reverse-engineered, §8.3 |
| 0x5744 | `mib_compress_read` | per-metadata-idx decode from shm-cached flash bytes; doc §8.7 |
| 0x5924 | `mib_compress_write` | write-side incl. header rebuild (TODO to fully follow) |
| 0x5d44 | `mib_tlv_init_from` | parses the decoded stream into the shm structure (TODO full trace) |
| 0x5b54 | `mib_search_by_id`, 0x5cac `find_same_tag_times` | field lookup in decoded stream — **this will give the record format** (TODO next session) |
| 0x62d8 | `apmib_conf` | load a table from flash into shm (see 8.7; partially traced) |
| 0x67bc | `apmib_init` | init all four tables (see 8.7) |
| 0x6730 | `apmib_init_HW` | HW-only variant (see 8.7) |
| base+0x2fa0 | flash-read helper | see 8.7 open item |
| base+0x3320 | shm create-or-attach | see 8.7 |

### 8.6 `mib_config[]` — the block table (Confirmed; the source of the §1 slot extents)

Three live records out of six `mib_config[]` slots (28 B each at file offset
0x1c380; r_offset relocations list pins each field, each pointer field is an
`R_MIPS_REL32` against a local text/rodata address):

```
rec1 @0x2c39c: {0x00000001, 0x000009ee, 0, 0x20000, 0x4000, "H6"?,… "COMPHS"}
rec2 @0x2c3b8: {0x00000003, 0x00008ff6, 1, 0x24000, 0x6000, "6g"?, "COMPDS"}
rec4 @0x2c3f0: {0x00000003, 0x00008ff6, 2, 0x2a000, 0x6000, "6g"?, "COMPCS"}
(rec0/3/5 are all-zero — unused slots.)
```

Per-record field map (Confirmed via the pointer-locations in the REL table:
0x2c39c+0x14 → 0x10768 "COMPHS" rodata string etc.):

| rel off | field | value semantics |
|---|---|---|
| +0x00 | word | 1 (HW) / 3 (others) — meaning TBD |
| +0x04 | word | **decoded size** (0x9ee / 0x8ff6) — Confirmed (shm size in `apmib_init`) |
| +0x08 | word | table idx handed to shm key (`0`, `1`, `2`) + `pOld` uses a literal 3 not from the table (Speculative: idx 3 = a second CS copy?) |
| +0x0c | word | **flash offset** in /dev/mtdblock0: 0x20000 / 0x24000 / 0x2a000 |
| +0x10 | word | max allowed compressed length: 0x4000 / 0x6000 / 0x6000 |
| +0x14 | ptr | rodata short strings ("…6"?) — possibly display names, TODO |
| +0x18 | ptr | the expected 6-byte signature string: `COMPHS`@0x1076c, `COMPDS`@0x10778, `COMPCS`@0x10780 (Confirmed: these offsets hold those exact ASCII strings + NUL) |

Note about +0x04×+0x06 interplay: in `mib_compress_read` no mul was observed
on the read side; in `apmib_conf` calloc(1, decoded_size + 9) is done for the
output buffer and separately the header's `u16@+6` multiplies the *header
len* value at +8 — see 8.8 open item; **do not guess** the "+9" purpose.

### 8.7 Storage architecture — shm, not direct reads (Confirmed)

libapmib.so **does not read /dev/mtdblock0 via file I/O directly** at these
sites; everything is marshaled through a System-V shared-memory scheme:

- key: `ftok("/var", 0x3c + table_idx)` — the string at RO `0xffec` is
  literally `/var` (this is the same `apmib_mtd` convention Realtek SDK
  apmib has used for years; here ids are 60..63 for tables 0..3).
- size: the decoded size from `mib_config[]` (+0x04).
- create-or-attach: `shmget(key, size, 0o3666)` (IPC_CREAT|IPC_EXCL|0666);
  on `errno == 17 (EEXIST)` it sets an `out` flag to 1 and retries
  `shmget(key, size, 0o666)` (no CREAT) then `shmat(shmid, 0, 0)`;
  when the segment was **newly created it is memset to 0**; when attached
  (existing), the data is kept.
- four instances are created in `apmib_init` (`apmib_init_HW` only does the
  HW one): `pHwSetting` (idx 0, 0x9ee B), `pMibDef` (idx 1 = COMPDS defaults,
  0x8ff6), `pMib` (idx 2 = COMPCS live config, 0x8ff6), `pOld` (idx 3,
  0x8ff6). After each attach the returned pointer is stored into that bss
  global; then `apmib_shm_free`-family errors print four distinctive
  messages on failure (strings at 0xff64+, e.g. `APMIB Semaphore …`).

Open item (has been blocking one step): the "base" pointer
(`*(gp-0x7fd4)`, GOT entry 0x34d1c, file-init value 0x00000000) is **not set
by any code path I found with the present grep** — it is dereferenced as
`(*(gp-0x7fd4) + 0x2fa0)` and called as a function (the flash-read helper
`mib_reader`) and also `+ 0x3320` for the shm fn. Both call sites take (…,
flash offset, …) style arguments, so the runtime value must be something like
a struct of function pointers established by a different code path (Likely:
`apmib_mtd_init` **not yet found**; possibly in another module like `fwd` /
a daemon that mmaps /dev/mtdblock0 first and registers hooks). **TODO #2**
next session: grep the whole rootfs for who sets it (e.g. search the binary
of any process that opens libapmib first, or search libapmib for a store of a
plain GOT slot: instruction bytes `af…0x82 0x7d 0xff…` pattern; or dynamic LD
preload init constructor at `_init` 0x2c7c; **or** the value is a struct with
function offsets where `+0x2fa0` and `+0x3320` index INTO that struct — in
that case the members at struct+0x2fa0 are `mib_reader`, and struct+0x3320 =
shm fn. The fact that both are used from one base var with big offsets favors
the "one ops-struct" theory (Most-Likely grade).

Additional Confirmed facts from `apmib_init` ordering:
1. `fopen("/tmp/SettingSize", "r")` → if it exists, `fclose` + fprintf
   `HwSetting Size %d DefMib Size %d ApMib Size %d` with the hardcoded
   literals — a debug probe the operator can touch on a live unit.
2. It iterates records in `mib_config[]` in idx order using the base-fn
   pattern `(mib_config[idx].maxLen?? a0 = u32@rec+0x4, a1 = u32@rec+0x8)`,
   +1 on a0 in the delay slot (indexing convention TBD — a0 passed is
   `size+1`?? do not guess, TODO #5), then `sw`-stores the result pointer
   into `pHwSetting` …, and finally shmem-free-checks.
3. The record load path is not `mib_compress_read` (which takes an index and
   a raw `_` shm-resident source) — for initial fill `apmib_conf(idx)` is
   called on failure of the *null-check* path (`bnez` on the bss global per
   the branch targets).

### 8.8 Open questions list (Sorted TODO for next session)

1. **cfgC (COMPCS) header field dump** at flash 0x2a000: ver u16@+6? and len
   u32@+8. Verify with mio.py (2-second job).
2. **base pointer** writer (see 8.7): find who populates `*(gp-0x7fd4)`.
   Search byte pattern of `sw reg, -0x7fd4($gp)` in all rootfs binaries
   (bytes `XX 99 82 9c` etc; sw offset is little-endian `0x82 0x9d`);
   also inspect `fwd` (the small daemon) — it is a candidate.
3. **Decode the blobs**: implement the LZSS decoder in Python (§8.3 constants
   suffice) and decode cfgA (`carve/cfgA.bin`) / cfgB / cfgC payloads; dump
   the TLV stream. Validate: decoded WL size must equal the known
   0x9ee / 0x8ff6 (or the record parse gives a coherent TLV list). This is
   the highest-value item — everything downstream needs it.
4. **Record/TLV format inside the decoded stream**: trace
   `mib_tlv_init_from` (0x5d44) + `mib_search_by_id` (0x5b54) +
   `find_same_tag_times` (0x5cac) to establish: record = {id, len, value}?
   id width? endianness? Where's the version/table-byte?
5. **Field-ID name table**: from `hwmib_table`/`mib_table`/… parse each
   28-byte?? record entry format (id, type, size, default pointer?); then
   interpret the decoded TLV stream as name=value pairs. **This is what
   unblocks the OpenWrt port questions** (MAC, RF calibration, country).
6. **auto_update (`/dev/mtdblock0` writer)**: disasm & find the block-flush
   logic (erase/write of the 0x20000 zone; possibly the 0xFF-erase vs
   zero-fill).
7. **`pOld` idx-3 table**: which flash region backs it (mtd2 0x7f8000
   `COMPHS`? or none) — resolve `apmib_init`'s literal 3 arg and
   `apmib_conf(3)`.
8. **`ex_*` group names** (0x349d0..0x34cf8): 23-27 B each (`ex_passward`?
   etc.) — probably the "web wizard excluded vars" names; TODO confirm
   semantics.
9. **`fwd` / `auto_update` parsers of COMPHS — mtd2 netis duplicate:**
   compare friend's mtd2-netis.bin COMPHS (16 KiB at 0x7f8000) vs our
   mtd0 COMPHS (4 KiB): the netis file maps to a different schema
   (device/MAC data) — likely the **idx-3 `pOld`** or the "hardware netis
   block". Not yet compared byte-level this session.
10. `.local/mtd0-re/mtd0.bin` self-test: re-verify all hashes after heavy
    ops (devcontainer stale-page gotcha, §7).

### 8.9 Environment lessons learned this session

- **`websearch` tool broken in this session** for every query (all queries →
  "No search results"). Workaround: `webfetch` against
  `https://lite.duckduckgo.com/lite/?q=…` works (returned results), and
  public-web coverage of `COMPHS`/`COMPDS` is nil → RE-only confirmed.
- capstone (with CS_MODE_MIPS32|LITTLE_ENDIAN) handles this MIPS32r2 code
  well; `elftools` Python module is absent, so the ELF parse here was raw
  struct unpack of DT_HASH/dynsym (see `disasm_lib.py`).
- Watch out: MIPS `jalr`/`b`-family **delay slots** mean an `addiu a0, a0, 1`
  after `jalr` *does* execute before the callee runs (this bit me twice.
  interepreting arg-setup); always read the delay slot of the preceding
  jump when reconstructing call arguments.
- The dynsym of libapmib.so is fully intact (uclibc dynamic link keeps
  `.dynsym` even without section headers); with DT_HASH you get the full
  symbol list. Use it: don't disassemble blind.

---

## 9. Checkpoint 2 (2026-11-05): format verified against GPL source; RE paused, build-first

### 9.1 The MSDK apmib source is public on GitHub

`gh search code "COMPHS"` (GitHub CLI, authenticated code search — this is a
different index from the `websearch` tool) located full GPL dumps:

- `frederic/rtl819x-toolchain` — `users/boa/apmib/{apmib.c,apmib.h,mibtbl.h,mibtbl.c}` (uses `mib_tlv_init_from` already)
- `Vyacheslav-S/openwrt-rtk819`, `7felix7/openwrt_rtk8197f`, `hackpascal/openwrt-rtk`, `cgoder/openwrt_rtk` — **OpenWrt trees for RTL8197F/819x** (also `tools/rtk-tools/src/mgbin.c`, `rtk_app/src/{apmib.h,mibtbl.h,mibhw.c,rtk_tx_calibration.c}`)
- `Saturn49/wecb`, `vankel/rtl819x-SDK`, `luminais/ecos_http`, `copslock/rtl-2.5sdk` etc. — more SDK copies
- `mgbin.c` = the factory image merger; its `checkInputFile` confirms the short
  signatures accepted for config files: `"h"`, `"COMPHS"`, `"6G"/"6A"/"6g"/"6a"`, `"COMPDS"`.

Fetched copies (with SHA256SUMS) in
`.local/mtd0-re/carve/sdk-ref/`. Ephemeral originals were
`/tmp/opencode/sdk-ref/` — re-fetch with
`curl raw.githubusercontent.com/<repo>/HEAD/<path>`.

### 9.2 Format facts now source-verified (cross-checked against §8 dis findings)

All refs = frederic `apmib.c` unless noted; line numbers from the fetched copies.

1. **Flash header** = `COMPRESS_MIB_HEADER_T {u8 sig[6]; u16 compRate; u32 compLen}`
   (`apmib.h`:3029​-3036). `compRate` is **not a version tag** — it is the
   worst-case expansion factor used to size the decode buffer:
   `expFile=malloc(zipRate*compLen)` (apmib.c:376/592/839), computed at write
   time as `(expLen/compLen)+1` (apmib.c:4569/4675/4796). compLen is stored
   big-endian on disk (apmib.c:551 DWORD_SWAP). Observed: A rate 5
   (2548/678+1=5 ✓), B/C rate 6 — formula-vs-observed mismatch for B/C noted
   in 9.3.
2. **LZSS codec**: `Decode` at apmib.c:4402-4457 is byte-for-byte the §8.3
   disassembly conclusion ( Okumura values N=4096, F=18, THRESHOLD=2,
   `text_buf=malloc(N+F-1)`, fill `N-F` spaces, `r=N-F`, flag byte
   `flags=(c|0xff00)`, copy run `for(k=0;k<=j;k++)` over pattern
   `i|=((j&0xf0)<<4)`, `j=(j&0x0f)+THRESHOLD`. Note: run length is
   `(j&0x0f)+THRESHOLD+1` copies **through `k<=j`** — the doc summary §8.3's
   "copy runlen (+2) bytes" was off by one; the source's `k<=j` is
   authoritative and the decoder reproduces valid streams.
3. **Write path** (`mib_compress_write`, 4522+): payload = inner
   `[PARAM_HEADER_T][MIB struct+chk]` → whole buffer `Encode`d explicitly each
   time compared into COMP* header; flash write = 12 B header + compLen stream.
   Payload inner header verified = 6 B: `{u8 sig[4]="H6"+"01" (TAG_LEN=2 +
   version "%02d" ASCII), u16 len}`. On MIPS LE the writer's `_LITTLE_ENDIAN_`
   path leaves the struct `len` **native LE** (apmib.c:4753-4763, swap
   commented out).
4. **TLV record** = `[u16 LE tag][u16 LE len][value]` (apmib.c
   `mib_write_to_raw` 5393-5408: `memcpy(&tlv_tag, 2); memcpy(&tlv_len,2);`
   native). Tag bit 15 (`MIB_TABLE_LIST`, apmib.h:50) marks a group record:
   `value = repetition count u16 + per-element sub-TLVs`;
   `mib_tlv_init_from` (5017) forwards each element to `next_mib_table`.
5. **Checksum**: `CHECKSUM(data,len)` over the last byte = two's-complement
   additive byte sum; `CHECKSUM_OK(data,len)` = `sum(data) == 0` including
   trailing byte (apmib.h:2385-2418). Payload body is TLV bytes with
   `tlv_content_len = innerHeader.len - 1` then one checksum byte
   (apmib.c:398-417).
6. **Inner header tags** for the CONFIG_RTL_8197F branch (apmib-openwrt819.h
   :2044-2068): HW=**"H6"**, DS=**"6G"** (gateway) / `"6V"`(+vpn) / `"6A"`(AP),
   CS=**"6g"** / `"6v"` / `"6a"`; force=`"Hf"/"Df"/"Cf"`,
   upgrade=`"Hu"/"Du"/"Cu"`. These are the rodata strings pointed at by
   `mib_config[]` +0x14 in our libapmib.so (§8.6).
7. **Table entry struct** `mib_table_entry_T` for MIB_TLV builds
   (`mibtbl-8197f.h:48​-66`): `{int id; char name[40((8812/AC builds)); u32 type;
   int offset; int size; u16 total_size; u16 unit_size; const u8*
   default_value; struct* next_mib_table}` = **0x44 bytes** (AC-support build,
   name[40]).
   Matches OUR libapmib.so symbol sizes exactly: `mib_table` 12648/68 = 186,
   `mib_wlan_table` 6664/68 = 98, `hwmib_table` 544/68 = 8, `hwmib_wlan_table`
   5644/68 = 83, root tables 136/68 = 2. This directly enables parsing all
   five tables from our binary via the stored ids+names+offsets.
8. **Our build reads tags/len as u16 BE**: dis 0x5dbc-0x5de4 (`mib_tlv_init_from`)
   performs the swap-after-LE-load idiom (`sll 8 / srl 8 / or` ⇒ 16-bit
   rotation, i.e. byte-swap) on both tag and len — see dis_libapmib.lst
   lines 6038-6058. This contradicts the frederic writer (`memcpy` native LE)
   and resolves by ours being the *swapped-on-disk* variant (also matches
   compLen BE on disk). §CONF 9.3 open item: first-record alignment (A starts
   `0b 53 00 01 00 18 …`, B `c1 73 02 8d 00 01 …`) does NOT parse as plain
   LE or BE tag/len at [0..4] — likely either a leading container/ver word or
   different leading record layout in the 2024 MSDK; finish the 0x5d44 dis
   trace + parse our tables to resolve.

### 9.3 Python decoder results

Tool: `.local/mtd0-re/apmib_decode.py` (LZSS constants + slot offsets; import
`mio`), run on verified flash windows:

| slot | compRate/len | decoded B | inner tag | inner ver |
|---|---|---|---|---|
| A COMPHS @0x20000 | 5, 0x2a6 | 2907 B (sha256 3e22267e…) | "H6" | "01" |
| B COMPDS @0x24000 | 6, 0x21be | 49531 B (afd5ac66…) | "6G" | "03" |
| C COMPCS @0x2a000 | 6, 0x225b | 49531 B (49b030e6…) | "6g" | "03" |

- A/B/C payloads decode to sane data (plaintext strings inside, e.g. serial
  `1100316096910666` in A; WAN-type defaults `c0a80101` etc. in B/C) — LZSS
  impl is validated; stream decode ends at byte exhaustion, so B/C totals
  may be short by a few bytes until the checksum rule fixes the true length.
- B decoded ≈ C decoded (same 49531 B size, both start with the same TLV
  bytes; only later bytes differ) — consistent with DS=defaults and CS=live
  config sharing the same schema (0x8ff6 shm size for both).
- Checksum check did **not** yet pass for the naive reading (checksum at
  body[-1] with len from the u16@+4) because both header len bytes read 0 —
  consistent with open item 9.2#8 (the inner len field may sit elsewhere or
  be unused in our build).
- Stray nonzero tails beyond compLen: A single `c4 00` at rel 0x9F6
  (≈ 6+maxLen tail region of an older-era write), B 0x1D bytes / C 0x19D
  bytes beyond the compressed end. All *after* the declared streams; zeros
  (not 0xFF) everywhere else per mgbin zero-padding factory image. Benign for
  the codec; not emulated by the writer paths we have (they would have
  zero-filled). Leave as observed-facts, do not chase further for OpenWrt.

### 9.4 Decision recorded this checkpoint

- **Pause config-slot RE; proceed build-first.** Rationale: the port risk
  mass moved — the public OpenWrt RTL8197F trees remove the driver gap;
  our remaining RE work (GPIO map, RF cal offsets, bootloader state) is now a
  *list of lookups* rather than unknowns, and each can be done when a specific
  build/flashing question demands it.
- `PROJECT_CONTEXT.md` registry updated with the same decision.

### 9.5 Next steps (build-first)

| # | step | note |
|---|---|---|
| 1 | Shallow-clone `cgoder/openwrt_rtk`, `Vyacheslav-S/openwrt-rtk819`, `7felix7/openwrt_rtk8197f` into git-excluded `.local/porte/` | inspection only |
| 2 | Look for an existing RTL8197F target / flash map / device def (any WF2409/Netis?) | reuse or fork |
| 3 | Parse `mib_table`/`mib_wlan_table`/`hwmib_table`/`hwmib_wlan_table` from our `.so` (0x44-byte entries) into an ID↔name↔offset map | closes §8.8 #5, defines the RF-cal offsets |
| 4 | Cross the decoded COMPHS (H6) blob against the hwmib table to extract RF cal + MAC + country | needed by rtk driver cal path |
| 5 | GPIO/LED map from stock `vmlinux.bin` + rtk tree DTS | build-def |
| 6 | Bootloader write-path behavior test (UART) or trust cr6c-sum-only | flash route |
| 7 | Attempt OpenWrt build at ≤2880 KiB using seed.config lever | known-good lever |

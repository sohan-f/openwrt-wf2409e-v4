# WF2409E v4 radio — reverse-engineering evidence ledger

This module is a clean-room rewrite. It never includes vendor source; it
re-implements only what the evidence below pins down. Grades: **Confirmed**
(read directly from a binary/source under controlled conditions),
**Likely** (source present, path not exercised in this session),
**Speculative** (RE inference, needs runtime proof).

Sources:

- **Stock** — friend-unit firmware kernel (Linux 3.10.90, Realtek MSDK GCC
  4.8.5p1, built 2024-04-24), carved from flash mtd0 in an earlier session.
  Binary: `.local/mtd0-re/carve/vmlinux.bin`; symbol table
  `.local/mtd0-re/carve/stock-kallsyms-full.txt` (30,546 symbols);
  annotated disassembler `.local/mtd0-re/carve/kdis.py`.
- **Donor** — vendor GPL tree kept in-tree at `package/kernel/rtl8192cd`
  (donor commit `cd3d082`), as it looks under the current patch chain
  (0002..0014). Used as *reference*, not compiled into the WF2409E image.
- **P** — our own patch RE output (`patches/0014-*`).

## Verified facts (M1)

| Constant | Value | Grade | Evidence |
|---|---|---|---|
| WMAC register base | phys 0x18640000 (KSEG1 0xb8640000) | Confirmed | Stock `wlan_device[0]` at VA 0x805bd67c: word stream `…, 0xb8640000, 0x0000006, …` read out of the binary directly |
| WMAC IRQ | 6 | Confirmed | same `wlan_device[0]` entry; unused until M3 (rings/ISR) |
| SoC chip tag | `REG32(0xB8000000) & 0xfffff000 == 0x8197F000` | Confirmed | donor `8192cd_osdep.c:16272`; same branch present in stock init path |
| WMAC clock gate | `REG32(0xB8000064) \|= 0x1f` | Confirmed | donor `8192cd_osdep.c` (same block, SOC init before any WMAC access) |
| PWR WRITE semantics | byte RMW; read `msk`, replace with `value&msk`, write back | Confirmed | donor `Hal88XXPwrSeqCmd.c`, `case PWR_CMD_WRITE:` (`HAL_RTL_R8`/`HAL_RTL_W8`) → our `wf_rmw8` |
| PWR POLLING semantics | bounded loop, `(val & msk) == value`, max 5000 iterations | Confirmed | donor `Hal88XXPwrSeqCmd.c`, `maxPollingCnt = 5000` |
| PWR walker stop condition | returns on `PWR_CMD_END`; zero-filled slack entries are never read | Confirmed | donor `Hal88XXPwrSeqCmd.c` `do { … } while(1)` + END `case` |
| card-enable sheet size | 11 rows (`RTL8197F_TRANS_96FSOC_POWER_ON`) + 10 restored rows + END | Confirmed | donor `Hal8197FPwrSeqCmd.h` (post-0012); the 10 rows came from the stock `rtl8197G_card_enable_flow` tail (patch 0014 commit msg) |
| PWR row shape | `{offset, cut_msk, fab_msk, intf_msk, base\|cmd, msk, value}` | Confirmed | donor `Hal8197FPwrSeqCmd.h` format comment + `wlan_pwr_cfg` struct usage |
| consumer of enable flow | `InitPON8197F` | Confirmed | donor `Hal8197FGen.c:173` |
| SoC tag test chip access | direct KSEG1, no bus glue needed for WMAC | Likely | donor uses `REG32()` (raw KSEG1) throughout for TYPE_EMBEDDED |

Deliberate deviations from the vendor walker (documented in the driver):

1. the polling loop sleeps 10 µs between reads (vendor burns the CPU);
   worst case ≈ 50 ms, still bounded by the same 5000-iteration budget;
2. a poll timeout aborts `start()` with `-ETIMEDOUT` instead of the
   vendor's log-and-continue — for bring-up we want the failure visible;
3. rows with `base != PWR_BASEADDR_MAC` are skipped at runtime (vendor
   does the same), and `tools/wf-pwr-gen.py` additionally asserts every
   generated row is all-mask and MAC-based. Other bases (PHY/RF spaces)
   belong to M2.

## Verified facts (M2)

Image format (donor `Image2HeaderVersion R3 1.5.5` sheets):

| Fact | Value | Grade | Evidence |
|---|---|---|---|
| data rows | 2 words `{addr, val}` | Confirmed | donor walkers consume strictly 2 words per step: `halhwimg8197f_mac.c` (31–324), `halhwimg8197f_bb.c` (walker heads), `halhwimg8197f_rf.c` (706+) |
| IF/ELSEIF record | 4 words `{cond1, cond2, BIT30 trigger, cond4}`; the IF word only stores `pre_v1/v2`, the **following** BIT30 word evaluates `check_positive(pre)` | Confirmed | walker `IF or ELSE IF` branch sets `pre_v1/pre_v2` only (mac 297–301); negative-condition branch (302–317) reads them back |
| match rule | only `cond1`: cut [27:24], pkg [15:12], intf [11:8], rfe low byte; zero field = wildcard | Confirmed | `check_positive` (mac 31–95); the three copies (mac/bb/rf) are byte-identical (normalized: 1508 chars each) |
| chain semantics | after a hit, `is_skipped=true` collapses later IF/ELSEIF and blocks ELSE; ELSE applies iff no branch matched; ENDIF resets both flags | Confirmed | walker branches (mac 274–317) |
| MAC apply | plain byte write of `val` | Confirmed | `odm_config_mac_8197f` → `odm_write_1byte` (`phydm_regconfig8197f.c:72–77`) |
| BB/AGC apply | plain 32-bit write of `val` (walker passes full dword mask) | Confirmed | `odm_config_bb_phy_8197f`/`odm_config_bb_agc_8197f` (regconfig 118–186) + `BIT_MASK_SET_MASKDWORD_COMMON = 0xffffffff` (`HalComPhyBit.h:683`) |
| BB delay ops | `fe/fd/fc` = 50/5/1 ms, `fb/fa/f9` = 50/5/1 µs | Confirmed | `odm_config_bb_phy_8197f:164–179` — none present in the resolved 97F tables (generator asserts) |
| RF write | `{((reg<<20)\|(data&0xfffff))&0x0fffffff}` into LRF `0x840`/`0x844` + 1 µs pacing; rows `0xfe/0xffe` = 50 ms pause | Confirmed | `config_phydm_write_rf_reg_8197f` (hal_api 342–392), `odm_config_rf_reg_8197f` (regconfig 21–40) |
| RF read | plain BB read at `0x2800`/`0x2c00 + (reg<<2)`, mask 0xfffff; invalid = 0xffffffff | Confirmed | `config_phydm_read_rf_reg_8197f` (hal_api 310–338); stock `PHYQueryRFRegHandler` binds to it (`Hal8197FGen.c:345`) |

Driver identity for condition resolution (`driver1 = 0x0F001001`):

| Field | Value | Grade | Evidence |
|---|---|---|---|
| cut | 0xF = `(SYS_CFG>>12)&0xF` (97F tag nibble; ≠ ODM_CUT_A=0 so A-cut fallbacks never match) | Confirmed | `8192cd_hw.c:15329–15331` |
| package | 1 ("97FS, PKG1" hardcoded for the 97F SoC build) | Confirmed | `8192cd_hw.c:15401` |
| interface | 0 — `ODM_CMNINFO_INTERFACE` has a setter (`phydm.c:2580`) but **no caller** in the rtl8192cd AP glue; all runtime uses are `== ODM_ITRF_PCIE` comparisons in interrupt-mask helpers | Confirmed | exhaustive `rg` over donor tree |
| rfe | 1 = `CONFIG_SOC_RFE_TYPE_1=y` → `pmib->dot11RFEntry.rfe_type = 1` | Confirmed | `8192cd_osdep.c:13325–13327` |

NB: an earlier session note read `0x80001001` as interface-coded; superseded —
it is pkg nibble 1, interface wildcard (0), rfe byte 1, i.e. it **matches** this
board. Consequence of the correct reading: IF blocks carrying `0x80001001`
apply at runtime, and the resolved tables are far richer than top-level+ELSE.

Resolution statistics of `tools/wf-macbb-gen.py` (walker replica, verified by
reparse byte-check and raw-array length mirror):

| Table (donor array) | resolved / raw | IF hits | note |
|---|---|---|---|
| mac (`array_mp_8197f_mac_reg`, 304 words) | 144 / 152 | 0 | its single chain is `0x80001002` (rfe 2) → miss; ELSE block applied |
| bb phy_reg (`array_mp_8197f_phy_reg`, 2984 w) | 464 / 1492 | 11 | all hits are `0x80001001` |
| agc (`array_mp_8197f_agc_tab`, 10100 w) | 196 / 5050 | 3 | incl. leading IF |
| radioa (`array_mp_8197f_radioa`, 1356 w) | 88 / 678 | 5 | |
| radiob (`array_mp_8197f_radiob`, 920 w) | 56 / 460 | 2 | |

97F init order (donor `rtl8192cd_init_hw_PCI`, hard evidence per line):

| Step | Grade | Evidence |
|---|---|---|
| InitMAC (rings) runs before all images | Confirmed (M3 scope, skipped here) | `8192cd_hw.c:17483` |
| PRE: `0x800[25:24] = 0` (disable OFDM/CCK blocks) | Confirmed | `8192cd_hw.c:17674–17676` + `config_phydm_parameter_8197f_init` (hal_api 1154–1175) |
| MAC img via `odm_config_mac_with_header_file` | Confirmed | `8192cd_hw.c:17697–17701` (USE_OUTSRC && `config_bbrf`) |
| BB phy_reg then AGC tab | Confirmed | `phy_BB88XX_Config_ParaFile` @ `8192cd_hw.c:17857`, phydm `odm_config_bb_with_header_file` |
| RF images A then B | Confirmed | `8192cd_hw.c:18201–18204` |
| `No_RF_Write = 1` after images | Confirmed; gates only the legacy `phy_set_rf_reg` wrapper (itself the only consumer, `8192cd_hw.c:969`), not the phydm paths our rewrite follows | `8192cd_hw.c:969–977`, `18228` |
| POST: `0x800[25:24] = 0x3` | Confirmed | `8192cd_hw.c:18254–18256` |
| extras: RF 0xb4 readback == 0x140a0 → mdelay(1) + `W8(0x7a,0)`; always `W8(0x7d4, 0x90)` | Confirmed | `8192cd_hw.c:18257–18261`; note the resolved radioa programs 0xb4 = **0x141f0**, so the gate fails and only `0x7d4` is written — our driver keeps the comparison live and logs the readback |
| trx 2T2R: `config_phydm_trx_mode_8197f(AB, AB, tx2path)` | Confirmed | `8192cd_hw.c:19909–19917`; MIMO_TR_mode default → else-branch (`8192cd_osdep.c:3957`); `tx2path` default 1 (`8192cd_ioctl.c:1431` mib row) |

trx 2T2R exact register effect (`config_phydm_trx_mode_8197f` hal_api
1054–1152, CCK RX helper `phydm_api.c:266–341`, CCA par hal_api 38–97):

| Register | Value | Note |
|---|---|---|
| 0x90c | RMW mask 0x0fffffff (`BIT27\|26\|25\|24\|MASKL3BYTES`), AB → `0x81121313`, tx2path → `0x83321333` | bits [31:28] survive |
| 0xc04 | byte0 = 0x33 | OFDM RX AB |
| 0xd04 | bits [3:0] = 0x3 | |
| 0xa04 | [31:28] = 0x8, then 0xc (tx2path) | CCK TX path AB |
| 0x80c | bit31 = 0 | |
| 0xa04 | [27:24] = 0 | CCK RX **path A only** — vendor FA workaround (hal_api 1111) |
| 0xa74 | bit8 = 0 | no CCK RX diversity |
| 0xa2c | [18:17] = 0, [22:21] = 0 | no CCK 2R/MRC |
| RF reg 0x30/31/32 | A: `08000/0005f/01042` then `18000/0004f/71fc2`; B: `08000/00050/01042` then `18000/00040/71fc2` | 2T standby → RX table switch |
| 0xc3c | RMW `& ~0x1f8 \| (0x2<<3)\|(0x6<<6)`, full dword writeback | `phydm_cca_par_by_bw_8197f` rfe1+pkg1, cut≠A, BW20, rx AB |
| 0xaa8 | [20:16] = 0x12 | same branch |

Channel switch (`config_phydm_switch_channel_8197f` hal_api 826–877): read RF18
(`0x18`, `phydm_regdefine11n.h:32`) through the RF read window, RMW `~(BIT18\|BIT17\|byte0) \| ch`,
full-mask write to both paths (vendor guards path B on `rf_type > 1T1R`; ours is
2T2R by construction). Its `0xc70` read is DIG bookkeeping for
`agc_table_idx` only — no hardware effect without our DIG, skipped.

Deliberate M2 deviations / skips, each with the vendor context:

1. The AGC big-jump limits (`odm_update_agc_big_jump_lmt_8197f`,
   regconfig 83–116) update DIG software state only; skipped until DIG
   exists (M3+).
2. `phydm_init_hw_info_by_rfe_type_8197f` (hal_api 207+; rfe1 sets
   `0x4c[24]=1`, `0x64[28]=1`, `0x40[27:24]=5` plus ext-PA/LNA ODM flags)
   is deferred — Speculative without board evidence of ext-PA/LNA paths.
3. `is_tx2_path` CCK 2-path zeroes in the vendor CCK-RX helper run twice
   (generic + 97F override) with identical values; the driver writes the
   net state once.
4. Images ship fully resolved — IF/ELSE structure and delay ops are
   consumed by `tools/wf-macbb-gen.py` at regeneration time, the driver
   consumes flat rows only.

## M1 module map

- `package/kernel/wf2409e-wlan/src/wf2409e_wlan.h` — constants (with
  provenance) + `wf_pwr_cmd_entry`.
- `package/kernel/wf2409e-wlan/src/wf2409e_wlan.c` — engine, generated
  card-enable table, SoC gate, mac80211 skeleton (AP/STA modes, 4 TX
  queues, 2.4 GHz field 1..13, HT20/single-MCS-band at M1).
- `tools/wf-pwr-gen.py` — regenerates the table from the sheet sources:

      python3 tools/wf-pwr-gen.py \
        <tree>/rtl8192cd/src/WlanHAL/RTL88XX/RTL8197F/Hal8197FPwrSeqCmd.h \
        <tree>/rtl8192cd/src/WlanHAL/RTL88XX/RTL8197F/Hal8197FPwrSeqCmd.c

The `.c` table is generated output; hand edits will be lost and reviewed
against the gold on the next regen, so never hand-edit it.

## Runtime expectations (M1)

- `dmesg` shows `SoC tag` warn only if the chip is not 8197F.
- No traffic flows until M2/M3 (`configure_filter` accepts nothing).

## M2 additions

- `package/kernel/wf2409e-wlan/src/wf2409e_tables.h` — **GENERATED**
  flat image tables (mac/bb/agc/rf_a/rf_b), one row per register apply,
  condition records pre-resolved for `driver1 = 0x0F001001`; the
  reparse byte-check at generation time makes hand edits fail loudly.
- `tools/wf-macbb-gen.py` — regenerates them from the donor sheets:

      python3 tools/wf-macbb-gen.py \
        /path/to/tree/package/kernel/rtl8192cd/src/phydm \
        --out package/kernel/wf2409e-wlan/src/wf2409e_tables.h

- `wf2409e_wlan.c` — M2 accessors (`wf_bb_*32`, `wf_rf_read/write`),
  image appliers (byte/dword/LRF), and the start() chain: PRE → MAC →
  BB → AGC → RF A/B → POST → POST extras → trx 2T2R → channel 6.

## Runtime expectations (M2)

(Log line formats as first shipped; superseded by the M2.1 formats
below — the semantic expectations still apply.)

- First `ip link set wlan0 up` runs PWR, then the 948 image rows (the
  50 ms pauses at RF rows 0xfe/0xffe are the only pacing), trx 2T2R and
  the ch6 switch, then logs:
  - `POST: RF 0xb4 readback 0x…` — expected 0x141f0 per the resolved
    table; a different value or 0xffffffff means LRF/RF-path trouble.
  - `ch6: RF18 pre-read 0x…` / `post-read 0x…` — expected pre-read
    0xc07 (the image programs RF18 = 0xc07) and a post-read ending in
    0x6; anything else says the RF write path or table identity is off.
  - `init chain complete: mac 144, bb 464, agc 196, rfA 88, rfB 56 rows`.
- On a dead-silicon or gating issue the `0xa0/0xa1` polls fail first and
  log a `PWR polling timeout @0x…` line — that message means the WMAC
  clock gate/base is wrong in a way that survived compile, and nothing
  downstream of PWR ran.

## M2.1: RF read window discrepancy + boot safety (Nov 7 2026)

Afferent evidence, both from the stock kernel binary
(`.local/mtd0-re/carve/`, friend-unit image):

- **Writes [S]** — `odm_set_rf_reg` (0x802d415c) → `phy_set_rf_reg`
  (0x8021b4f8, ops+0xd0) → `phy_RFSerialWrite` (0x8021b130): packs
  `(addr<<20)|data` into the *struct*-designated port and jumps to
  `phy_set_bb_reg`. The port table — filled by
  `phy_InitBBRFRegisterDefinition` (0x8021fb04) — holds **A = 0x840,
  B = 0x844**, matching the donor protocol and our LRF writes.
- **Reads: two protocols coexist in the ecosystem.**
  - *Donor/vendor 97F* — `config_phydm_read_rf_reg_8197f`
    (phydm_hal_api8197f.c:317): `odm_get_bb_reg(dm, 0x2800/0x2c00 +
    (reg & 0xff)<<2, mask)` — a per-register shadow window.
  - *Stock binary* — `odm_get_rf_reg` (0x802d41fc) → `phy_query_rf_reg`
    (0x8021b4e8, ops+0xd4) → `phy_RFSerialRead` (0x8021b160): final
    access = `phy_query_bb_reg(priv, s2[+0x40 | +0x44], 0xfffff)` with
    the same init struct giving **0x8a0 (A) / 0x8a4 (B)** — a single
    result register per path, NOT the 0x2800 window. The stock kernel
    contains **no `config_phydm_*_8197f` symbols** (only 8197G), so
    which binding actually runs on this chip was never settled by
    static evidence alone.
- Consequence for M2: reads were only ever correctness-checks, but the
  ch6 switch *fed* the write from the pre-read — a wrong window would
  have programmed arbitrary 20 bits into RF18.

M2.1 changes in `wf2409e_wlan.c`:

- `wf_switch_channel` writes RF18 = `0x000c06` on both paths, computed
  from the **image's own RF18 row (0x000c07)** with the vendor masks
  (`0x0c07 & ~(BIT18|BIT17|0xff) | 6`); the pre-read no longer feeds
  the write.
- Both read protocols are now *probed* and logged: the donor window
  (`0x2800` + `(reg<<2)`) and the stock result regs (`0x8a0/0x8a4`):
  - `POST: RF 0xb4 readback: win2800 0x… res8a0 0x…`
  - `ch6: RF18 pre-read 0x…, post-read A 0x… B 0x… (sent 0x…)` — the
    `(sent)` field shows the written value; whichever readback window
    returns `…000c … c06`-shaped values is the live one.
- Both candidate addresses sit inside the 0x3000 WMAC aperture, so a
  wrong guess cannot fault: worst case is a garbage log value.

Boot-safety overlay for the next artifacts:

- Radio seeded **disabled** (`20_wireless` uci-default →
  `disabled='1'`): netifd will not run M2's start() during boot, so a
  clean boot isolates the death to shared base code; radio bring-up
  moves to an explicit ssh step with netconsole already streaming.
- `kmod-netconsole` + `/etc/init.d/eednetcon` (S13, files overlay):
  arms `netconsole=@192.168.1.1/eth0,6666@192.168.1.2/` right after
  eth0 exists, before netifd, so the whole post-eth boot streams to a
  host listening with `nc -u -l 6666` (or `socat udp-recv:6666 -`) at
  static `192.168.1.2/24`. If the boot dies before S13, nothing is
  captured — that failure mode needs a different instrument.

## Open questions / next milestones

- **mac80211 channel wiring** — M2 programs channel 6 directly in
  `start()`; a `set_channel` op (plus `config_bbrf`-equivalent state)
  is needed before real network operation.
- **M3** — descriptor rings, DMA layout, ISR (IRQ 6 above), real TX/RX,
  and the TXPWR/IQK/DPK/LLT work the vendor does inside InitMAC/compute
  (donor 8192cd_hw.c:17483–17857).
- **RFE board evidence** — the deferred
  `phydm_init_hw_info_by_rfe_type_8197f` block (ext-PA/LNA, rfe1)
  needs the WF2409E schematic/RFE-type runtime proof before adding.
- Donor `rtl8192cd` must remain buildable as reference: keep its package
  intact alongside ours (switch by `DEVICE_PACKAGES`/`seed.config`).

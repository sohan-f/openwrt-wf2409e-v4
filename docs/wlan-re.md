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
- `ip link set wlan0 up` (first vif open) runs the card-enable sequence;
  on success `card-enable sequence complete` appears. On a dead-silicon
  or gating issue the `0xa0/0xa1` polls fail and log a `PWR polling
  timeout @0x…` line — that message means the WMAC clock gate/base is
  wrong in a way that survived compile.
- No traffic flows until M2/M3 (`configure_filter` accepts nothing).

## Open questions / next milestones

- **M2** — MAC/BB/RF init sheets (register tables from the donor
  `Hal8197FPhyCfg.c` etc.), TXPWR, loopback validation without UART.
- **M3** — descriptor rings, DMA layout, ISR (IRQ 6 above), real TX/RX.
- Donor `rtl8192cd` must remain buildable as reference: keep its package
  intact alongside ours (switch by `DEVICE_PACKAGES`/`seed.config`).

#!/usr/bin/env python3
"""Generate the M2 MAC/BB/RF register-image tables (wf2409e_tables.h) from
the RTL8197F phydm image sources of a chain-applied donor tree.

Usage:
    wf-macbb-gen.py <phydm-dir> --out <path>

    <phydm-dir> is the phydm root, e.g.
        <tree>/package/kernel/rtl8192cd/src/phydm

Sources read:
    rtl8197f/halhwimg8197f_mac.c        array_mp_8197f_mac_reg
    rtl8197f/halhwimg8197f_bb.c         array_mp_8197f_phy_reg, _agc_tab
    halrf/rtl8197f/halhwimg8197f_rf.c   array_mp_8197f_radioa, _radiob

Image format (Image2HeaderVersion R3 1.5.5): data rows are 2-word
{addr, value} pairs. IF/ELSE chains carry the driver-match condition in
4-word records {cond1, cond2, cond3, cond4}; only cond1 is evaluated:
cut nibble [27:24], package nibble [15:12], interface nibble [11:8],
and the low byte [7:0] compared against the driver rfe_type. The IF/
ELSEIF word itself only saves pre_v1/v2; the FOLLOWING word (0x40000000,
bit 30 only) is the evaluation trigger. ELSE rows apply iff no earlier
branch in the chain matched (is_skipped stays false); ENDIF resets.

Driver matching word (rtl8192cd AP build for WF2409E):
    cut version  0xF  = (SYS_CFG>>12)&0xF from the 0x8197F000 tag nibble;
                        != ODM_CUT_A (0), so A-cut fallbacks are dropped
    package type 1    = "97FS, PKG1" hardcoded in the donor glue
    interface    0    = dm->support_interface is never initialized in the
                        rtl8192cd AP build (setter exists, no caller)
    rfe type     1    = CONFIG_SOC_RFE_TYPE_1=y -> pmib rfe_type = 1

    driver1 = 0x0F001001

Every IF/ELSEIF condition in these tables carries a nonzero interface
nibble (1/2/0xF), so with interface 0 all of them fail and only top-level
rows and ELSE-block rows are emitted - exactly what the vendor walker
does at runtime on this board.
"""
import argparse
import re
import sys

# vendor odm_config_bb_phy_8197f delay rows (addr field carries the op)
BB_DELAY_MS = {0xfe: 50, 0xfd: 5, 0xfc: 1}
BB_DELAY_US = {0xfb: 50, 0xfa: 5, 0xf9: 1}
BB_DELAY_OPS = set(BB_DELAY_MS) | set(BB_DELAY_US)
# RF pseudo-addr rows (odm_config_rf_reg_8197f): both mean 50 ms
RF_DELAY_OPS = {0xfe, 0xffe}

# cut=0xF, pkg=1, intf=0, rfe=1 (see argparse defaults for the knobs)
DRIVER1 = (15 << 24) | (1 << 12) | (0 << 8) | 1
COND_MASK = 0xC0000000


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def arrays_of(text, tag):
    out = {}
    for m in re.finditer(r"const u32 (array_\w+)\s*\[\]\s*=\s*\{(.*?)\};",
                         text, re.S):
        body = m.group(2)
        toks = [t for t in re.split(r"[,\s]+", body.strip()) if t]
        try:
            words = [eval(t, {"__builtins__": {}},
                          {"BIT": lambda n: 1 << n}) for t in toks]
        except NameError as e:
            sys.exit(f"{tag}:{m.group(1)}: cannot evaluate {t}: {e}")
        if len(words) % 2:
            sys.exit(f"{tag}:{m.group(1)}: odd word count {len(words)}")
        out[m.group(1)] = words
    return out


def check_positive(cond1):
    """Vendor check_positive for this build: only cond1 drives the match
    (cond2..4 unused), nibble fields with 0 are wildcards."""
    if ((cond1 & 0x0F000000) != 0) and \
       ((cond1 & 0x0F000000) != (DRIVER1 & 0x0F000000)):
        return False
    if ((cond1 & 0x0000F000) != 0) and \
       ((cond1 & 0x0000F000) != (DRIVER1 & 0x0000F000)):
        return False
    if ((cond1 & 0x00000F00) != 0) and \
       ((cond1 & 0x00000F00) != (DRIVER1 & 0x00000F00)):
        return False
    return (cond1 & 0x000000FF) == (DRIVER1 & 0x000000FF)


def resolve(tag, words):
    """Byte-for-byte replica of the vendor odm_read_and_config walker.
    Returns (kept rows as (offset, v1, v2), stats)."""
    i = 0
    out = []
    is_matched, is_skipped = True, False
    pre = (0, 0)
    stats = {"if": 0, "if_hit": 0, "else": 0, "else_hit": 0, "endif": 0}
    while i + 1 < len(words):
        v1, v2 = words[i], words[i + 1]
        if v1 & COND_MASK:
            if v1 & 0x80000000:
                c_cond = (v1 & 0x30000000) >> 28
                if c_cond == 3:  # COND_ENDIF
                    stats["endif"] += 1
                    is_matched, is_skipped = True, False
                elif c_cond == 2:  # COND_ELSE
                    stats["else"] += 1
                    is_matched = not is_skipped
                    if is_matched:
                        stats["else_hit"] += 1
                else:  # IF / ELSE IF: record only; next word evaluates
                    pre = (v1, v2)
            elif v1 & 0x40000000:
                if not is_skipped:
                    if check_positive(pre[0]):
                        is_matched, is_skipped = True, True
                        stats["if_hit"] += 1
                    else:
                        is_matched, is_skipped = False, False
                else:
                    is_matched = False
            i += 2
            continue
        if is_matched:
            out.append((i, v1, v2))
        i += 2
    if i < len(words) - 1:  # walker loops while i+1 < len; nothing left over
        sys.exit(f"{tag}: walker consumed {i} of {len(words)} - stop bug?")
    stats["rows"] = len(out)
    stats["raw_rows"] = len(words) // 2
    return out, stats


def cvt_mac(rows):
    for off, v1, v2 in rows:
        assert not (v1 & 0xC0000000), ("marker in data rows", hex(off))
        assert v1 <= 0x1fff and v2 <= 0xff, ("mac row shape", hex(v1), hex(v2))
        yield v1, v2


def cvt_bb_phy(rows):
    for off, v1, v2 in rows:
        if v1 not in BB_DELAY_OPS and not (0x800 <= v1 <= 0xfff):
            sys.exit(f"bb_phy: addr 0x{v1:x} outside BB space")
        yield v1, v2


def cvt_bb_agc(rows):
    for off, v1, v2 in rows:
        assert v1 not in BB_DELAY_OPS, \
            f"agc: delay row present (vendor would miswrite it): {hex(v1)}"
        assert v1 <= 0xffff, ("agc addr", hex(v1))
        yield v1, v2


def cvt_rf(rows):
    for off, v1, v2 in rows:
        if v1 in RF_DELAY_OPS:
            assert v1 in (0xfe, 0xffe), hex(v1)
        else:
            assert v1 <= 0xff, ("rf addr", hex(v1))
        assert v2 & ~0xfffff == 0, ("rf data not 20-bit", hex(v1), hex(v2))
        yield v1, v2


def hline(f, s=""):
    f.write(s + "\n")


def open_hdr(path, defs):
    f = open(path, "w")
    hline(f, "// SPDX-License-Identifier: GPL-2.0-only")
    hline(f, "/*")
    hline(f, " * wf2409e_tables.h - GENERATED MAC/BB/RF register images for the")
    hline(f, " * RTL8197F clean-room driver. Source of truth: phydm image arrays in")
    hline(f, " * the chain-applied donor tree; regenerate with")
    hline(f, " *")
    hline(f, " * \tpython3 tools/wf-macbb-gen.py <phydm-dir> --out $file")
    hline(f, " *")
    hline(f, " * Hand edits are lost and will fail the next byte-check against the")
    hline(f, " * donor sheets. Driver identity used to resolve the image condition")
    hline(f, " * records: driver1 = 0x0F001001 (cut 0xF, package 1, interface 0,")
    hline(f, " * rfe 1) - provenance and grades in docs/wlan-re.md, M2 section.")
    hline(f, " */")
    hline(f)
    hline(f, "#ifndef WF2409E_TABLES_H")
    hline(f, "#define WF2409E_TABLES_H")
    hline(f)
    hline(f, "#include \"wf2409e_wlan.h\"")
    for name, rows in defs:
        hline(f)
        hline(f, f"static const struct wf_{name}_op wf_{name}_img[] = {{")
        for addr, val in rows:
            f.write(f"\t{{ .addr = 0x{addr:03x}, .val = 0x{val:02x} }},\n")
        hline(f, "};")
    return f


def close_hdr(f, tables):
    hline(f)
    for name, rows in tables:
        hline(f, f"#define WF_{name.upper()}_IMG_N\t(ARRAY_SIZE(wf_{name}_img))")
    hline(f)
    hline(f, "#endif	/* WF2409E_TABLES_H */")
    f.close()


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("phydm_dir")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    mac_src = open(f"{args.phydm_dir}/rtl8197f/halhwimg8197f_mac.c").read()
    bb_src = open(f"{args.phydm_dir}/rtl8197f/halhwimg8197f_bb.c").read()
    rf_src = open(f"{args.phydm_dir}/halrf/rtl8197f/halhwimg8197f_rf.c").read()

    macs = arrays_of(strip_comments(mac_src), "mac")
    bbs = arrays_of(strip_comments(bb_src), "bb")
    rfs = arrays_of(strip_comments(rf_src), "rf")

    for tag, arrs in (("mac", macs), ("bb", bbs), ("rf", rfs)):
        if not arrs:
            sys.exit(f"{tag}: no arrays parsed")
        print(f"[{tag}] arrays: " + ", ".join(
            f"{k} ({len(v)} words)" for k, v in arrs.items()), file=sys.stderr)

    print(f"driver1 = {DRIVER1:#010x} (cut 15, pkg 1, intf 0, rfe 1)",
          file=sys.stderr)

    mac_rows, st = resolve("mac", macs["array_mp_8197f_mac_reg"])
    print("[mac]: " + str(st), file=sys.stderr)
    phy_rows, st = resolve("bb_phy", bbs["array_mp_8197f_phy_reg"])
    print("[bb_phy]: " + str(st), file=sys.stderr)
    agc_rows, st = resolve("bb_agc", bbs["array_mp_8197f_agc_tab"])
    print("[bb_agc]: " + str(st), file=sys.stderr)
    rfa_rows, st = resolve("rf_a", rfs["array_mp_8197f_radioa"])
    print("[rf_a]: " + str(st), file=sys.stderr)
    rfb_rows, st = resolve("rf_b", rfs["array_mp_8197f_radiob"])
    print("[rf_b]: " + str(st), file=sys.stderr)

    mac = list(cvt_mac(mac_rows))
    phy = list(cvt_bb_phy(phy_rows))
    agc = list(cvt_bb_agc(agc_rows))
    rfa = list(cvt_rf(rfa_rows))
    rfb = list(cvt_rf(rfb_rows))

    f = open(args.out, "w")
    hline = f.write
    hline("// SPDX-License-Identifier: GPL-2.0-only\n"
          "/*\n"
          " * wf2409e_tables.h - GENERATED MAC/BB/RF register images for the\n"
          " * RTL8197F clean-room driver. Source of truth: the phydm image\n"
          " * arrays in the chain-applied donor tree; regenerate with\n"
          " *\n"
          " * \tpython3 tools/wf-macbb-gen.py <phydm-dir> --out $file\n"
          " *\n"
          " * Hand edits are lost and will fail the next byte-check against\n"
          " * the donor sheets. The image condition records were resolved for\n"
          " * driver1 = 0x0F001001 (cut 0xF, package 1, interface 0, rfe 1);\n"
          " * provenance and grades live in docs/wlan-re.md, M2 section.\n"
          " */\n"
          "\n"
          "#ifndef WF2409E_TABLES_H\n"
          "#define WF2409E_TABLES_H\n"
          "\n"
          "#include \"wf2409e_wlan.h\"\n")

    def emit(name, rows, valfmt):
        st = {"mac": "wf_mac_op", "bb": "wf_bb_op", "agc": "wf_bb_op",
              "rf_a": "wf_rf_op", "rf_b": "wf_rf_op"}[name]
        hline("\n")
        hline(f"static const struct {st} wf_{name}_img[] = {{\n")
        for addr, val in rows:
            hline("\t{ .addr = 0x%03x, .val = 0x%0*x },\n" % (addr, valfmt, val))
        hline("};\n")
        hline(f"#define WF_{name.upper()}_IMG_N	(ARRAY_SIZE(wf_{name}_img))\n")

    emit("mac", mac, 2)
    emit("bb", phy, 8)
    emit("agc", agc, 8)
    # rf tables write data to RF path A/B; addr 0xfe/0xffe rows expect 50ms
    emit("rf_a", rfa, 2)
    emit("rf_b", rfb, 2)

    hline("\n#endif	/* WF2409E_TABLES_H */\n")
    f.close()

    # byte-check: reparse the emitted file and compare against the lists
    vals = {}
    src = open(args.out).read()
    structs = {"mac": "wf_mac_op", "bb": "wf_bb_op", "agc": "wf_bb_op",
               "rf_a": "wf_rf_op", "rf_b": "wf_rf_op"}
    for name in ("mac", "bb", "agc", "rf_a", "rf_b"):
        blk = re.search(r"static const struct %s wf_%s_img\[\] = \{(.*?)\};"
                        % (structs[name], name), src, re.S)
        assert blk, name
        got = [(int(a, 16), int(b, 16))
               for a, b in re.findall(r"\.addr = 0x([0-9a-f]+), "
                                      r"\.val = 0x([0-9a-f]+)", blk.group(1))]
        vals[name] = got
    # spot equality against what we parsed out of the donor sources
    assert vals["mac"] == mac
    assert vals["bb"] == phy
    assert vals["agc"] == agc
    assert vals["rf_a"] == rfa
    assert vals["rf_b"] == rfb
    # independent count mirror of the vendor array lens
    n = {
        "mac": 304, "bb_phy": 2984, "bb_agc": 10100,
        "rf_a": 1356, "rf_b": 920,
    }
    for tag, words_arr in (("mac", macs["array_mp_8197f_mac_reg"]),
                           ("bb_phy", bbs["array_mp_8197f_phy_reg"]),
                           ("bb_agc", bbs["array_mp_8197f_agc_tab"]),
                           ("rf_a", rfs["array_mp_8197f_radioa"]),
                           ("rf_b", rfs["array_mp_8197f_radiob"])):
        assert len(words_arr) == n[tag], (tag, len(words_arr))

    print("resolved: mac=%d bb=%d agc=%d rfA=%d rfB=%d"
          % (len(mac), len(phy), len(agc), len(rfa), len(rfb)),
          file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate the M1 card-enable PWR table (designated initializers) from the
RTL8197F vendor power-sheet sources.

Usage:
    wf-pwr-gen.py <Hal8197FPwrSeqCmd.h> <Hal8197FPwrSeqCmd.c>

- <h>: reads the RTL8197F_TRANS_96FSOC_POWER_ON macro (11 rows).
- <c>: reads the ten rows restored by patches/0014 into
       rtl8197F_card_enable_flow (between RTL8197F_TRANS_96FSOC_POWER_ON
       and RTL8197F_TRANS_END).

Every row must have the shape
    {offset, CUT_ALL, FAB_ALL, INTF_ALL, PWR_BASEADDR_MAC, CMD, msk, value}
with offset <= 0xff (WMAC MAC space is byte-addressed). Rows violating the
M1 assumptions abort the generation instead of being silently skipped.
 BITn expands to (1<<n); plain hex/decimal arithmetic is evaluated.
"""
import re
import sys


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def find_rows(text):
    out = []
    for m in re.finditer(r"\{([^{}]*)\}", strip_comments(text)):
        f = [t.strip() for t in m.group(1).split(",")]
        if len(f) != 8:
            continue
        out.append(f)
    return out


def ev(s):
    s = s.replace(" ", "")
    s = re.sub(r"BIT(\d+)", r"(1<<\1)", s)
    return eval(s)


def convert(raw, tag):
    if len(raw) != len({tuple(r) for r in raw}):
        sys.exit(f"{tag}: duplicate rows")
    parsed = []
    for (off, cut, fab, intf, base, cmd, msk, val) in raw:
        ov = int(off, 16)
        assert ov <= 0xFF, (tag, off, "offset exceeds byte MAC space")
        assert cut == "PWR_CUT_ALL_MSK", (tag, off, cut)
        assert fab == "PWR_FAB_ALL_MSK", (tag, off, fab)
        assert intf == "PWR_INTF_ALL_MSK", (tag, off, intf)
        assert base == "PWR_BASEADDR_MAC", (tag, off, base)
        assert cmd in ("PWR_CMD_WRITE", "PWR_CMD_POLLING"), (tag, off, cmd)
        m, v = ev(msk), ev(val)
        assert 0 <= m <= 0xFF and 0 <= v <= 0xFF, (tag, off, msk, val)
        parsed.append((ov, cmd, m, v))
    return parsed


def macro_rows(h_path):
    lines = open(h_path).read().splitlines()
    at = next(i for i, l in enumerate(lines)
              if l.startswith("#define RTL8197F_TRANS_96FSOC_POWER_ON"))
    end = at + 1
    # the macro body runs through backslash-continued lines only
    while lines[end].rstrip().endswith("\\"):
        end += 1
    raw = find_rows("\n".join(lines[at:end]))
    return convert(raw, f"{h_path}: macro")


def tail_rows(c_path):
    sig = "    RTL8197F_TRANS_96FSOC_POWER_ON"
    text = open(c_path).read()
    at = text.index(sig)
    stop = text.index("};", at)
    raw = find_rows(text[at:stop])
    return convert(raw, f"{c_path}: 0014 tail")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rows = macro_rows(sys.argv[1]) + tail_rows(sys.argv[2])
    print(f"// {len(rows)} rows: 96FSOC sheet + stripes/0014 tail", file=sys.stderr)
    for off, cmd, msk, val in rows:
        print(f"\t{{ .offset = 0x{off:02x}, .base = PWR_BASEADDR_MAC, "
              f".cmd = {cmd}, .msk = 0x{msk:02x}, .value = 0x{val:02x} }},")
    print("\t{ .cmd = PWR_CMD_END },")


if __name__ == "__main__":
    main()

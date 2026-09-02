#!/usr/bin/env python3
"""Verify NVMe host study-set Korean annotations (gating criteria from annotation.md / plan).

Drives the real annotated sources under drivers/nvme/host and include/linux/nvme.h.
Exit 0 only if all 22 files have dense Hangul/[한국어] coverage and balanced comments,
and annotation.md reflects completion.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = sorted((ROOT / "drivers/nvme/host").glob("*.[ch]")) + [ROOT / "include/linux/nvme.h"]
HANGUL = re.compile(r"[가-힣]")
MARKER = re.compile(r"\[한국어")


def main() -> int:
    if len(FILES) != 22:
        print(f"FAIL expected 22 study files, got {len(FILES)}", file=sys.stderr)
        return 1

    failures: list[str] = []
    for f in FILES:
        text = f.read_text(errors="replace")
        lines = text.splitlines()
        hangul = sum(1 for L in lines if HANGUL.search(L) or MARKER.search(L))
        pct = 100.0 * hangul / len(lines) if lines else 0.0
        opens, closes = text.count("/*"), text.count("*/")
        bal = opens == closes
        if hangul == 0:
            failures.append(f"{f.relative_to(ROOT)}: hangul_lines=0")
        if pct < 35.0:
            failures.append(f"{f.relative_to(ROOT)}: density {pct:.1f}% < 35%")
        if not bal:
            failures.append(
                f"{f.relative_to(ROOT)}: unbalanced comments /*={opens} */={closes}"
            )
        if not MARKER.search(text):
            failures.append(f"{f.relative_to(ROOT)}: missing [한국어] markers")
        print(f"OK {pct:5.1f}% bal={bal} {f.relative_to(ROOT)}")

    amd_path = ROOT / "annotation.md"
    if not amd_path.is_file():
        failures.append("annotation.md missing")
    else:
        amd = amd_path.read_text(errors="replace")
        if amd.count("[x]") < 15:
            failures.append(f"annotation.md too few [x] markers: {amd.count('[x]')}")
        if "미착수 ← 현재 작업" in amd:
            failures.append("annotation.md still claims NVMe not started")
        if "NVMe host study set 완료" not in amd and "전원 완료" not in amd:
            failures.append("annotation.md missing NVMe completion claim")

    if failures:
        print("FAIL")
        for item in failures:
            print(" ", item)
        return 1
    print("PASS all gating checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

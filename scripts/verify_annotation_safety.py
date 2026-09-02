#!/usr/bin/env python3
"""Guard: Korean annotations must never alter kernel code.

CLAUDE.md requires annotation passes to be comment-only. Three failure modes
have actually broken the tree before, and this script gates all of them:

  1. COMMENT LEAK - annotation text containing the ``*/`` sequence (e.g. writing
     ``pci_read_config_*/pci_write_config_*`` inside a block comment) closes the
     comment early, so the remaining Korean text is parsed as C.
  2. BROKEN MACRO - an inline comment appended *after* a macro's line
     continuation backslash makes the backslash non-final, truncating the macro.
  3. CODE DRIFT - any token change outside comments (identifier swaps,
     EXPORT_SYMBOL -> EXPORT_SYMBOL_GPL, dropped designated-initializer dots).

Usage:
    python3 scripts/verify_annotation_safety.py [--base REV] [PATH ...]

Exit status is 0 only when every scanned file is clean.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

HANGUL = re.compile(r"[가-힣]")
# String/char literals first so their contents are never split into tokens,
# then identifiers, numbers, and finally any single non-space punctuator.
TOKEN = re.compile(
    r'"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
    r"|[A-Za-z_$][A-Za-z0-9_$]*"
    r"|\d[\w.]*"
    r"|\S"
)
DEFAULT_PATHS = ("block", "drivers/pci", "drivers/nvme", "drivers/vfio", "include/linux")
DEFAULT_BASE = "1f0e418bb6"  # "Linux kernel base snapshot" - pristine upstream tree


def strip_comments(src: str) -> tuple[str, int | None]:
    """Drop comments, keeping line numbering intact (each comment -> its newlines).

    String and character literals are honoured so that a ``/*`` inside a literal
    is not mistaken for a comment. Returns (stripped_source, unterminated_line).
    """
    out: list[str] = []
    i, n, unterminated = 0, len(src), None
    while i < n:
        c = src[i]
        if c in "\"'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\":
                    out.append(src[i : i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            end = src.find("*/", i + 2)
            if end < 0:
                unterminated = src[:i].count("\n") + 1
                out.append("\n" * src[i:].count("\n"))
                break
            out.append("\n" * src[i : end + 2].count("\n"))
            i = end + 2
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            end = src.find("\n", i)
            i = n if end < 0 else end
            continue
        out.append(c)
        i += 1
    return "".join(out), unterminated


def check_leaks(src: str) -> list[str]:
    """Mode 1: comment text that escaped into code because a comment closed early."""
    problems: list[str] = []
    stripped, unterminated = strip_comments(src)
    if unterminated is not None:
        problems.append(f"L{unterminated}: unterminated comment")
    for lineno, line in enumerate(stripped.split("\n"), 1):
        text = line.strip()
        if not text:
            continue
        if HANGUL.search(text):
            problems.append(f"L{lineno}: comment text in code: {text[:60]}")
        elif text.startswith("*/"):
            problems.append(f"L{lineno}: stray comment close")
    return problems


def check_macros(src: str) -> list[str]:
    r"""Mode 2: a stray backslash left inside code by a badly placed comment.

    Line splicing (translation phase 2) runs *before* comments are replaced
    (phase 3), so writing ``#define Q(v) \ /* note */ \`` still splices, but the
    first backslash survives into the macro body as a stray token and gcc
    rejects it. A backslash is only ever legal at end-of-line or inside a
    literal, so anything else is damage.
    """
    problems: list[str] = []
    stripped, _ = strip_comments(src)
    for lineno, line in enumerate(stripped.split("\n"), 1):
        body = line.rstrip()
        if body.endswith("\\"):
            body = body[:-1]  # the legitimate continuation backslash
        if "\\" not in body:
            continue
        if any(tok == "\\" for tok in TOKEN.findall(body)):
            problems.append(
                f"L{lineno}: stray backslash in code: {line.strip()[:60]}"
            )
    return problems


def code_fingerprint(src: str) -> str:
    """Comment-free logical token stream used for drift detection.

    Line continuations are spliced exactly as the C preprocessor does in
    translation phase 2, so reflowing a multi-line macro (or adding a blank
    continuation line between Korean comment blocks) is correctly seen as
    identical, while dropping a backslash that a macro actually needed still
    changes the splice and is reported.
    """
    stripped, _ = strip_comments(src)
    spliced = re.sub(r"\\[ \t]*\n", "", stripped)
    return " ".join(TOKEN.findall(spliced))


def git_show(rev: str, path: str) -> str | None:
    try:
        return subprocess.check_output(
            ["git", "show", f"{rev}:{path}"], text=True, errors="replace",
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        return None  # file did not exist at base; nothing to compare against


def iter_sources(paths: tuple[str, ...]) -> list[str]:
    found: list[str] = []
    for path in paths:
        if os.path.isfile(path):
            found.append(path)
            continue
        for root, _dirs, files in os.walk(path):
            found.extend(
                os.path.join(root, f) for f in files if f.endswith((".c", ".h"))
            )
    return sorted(found)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default=DEFAULT_BASE,
                        help="pristine revision to compare code against")
    parser.add_argument("paths", nargs="*", default=list(DEFAULT_PATHS))
    args = parser.parse_args()

    sources = iter_sources(tuple(args.paths) or DEFAULT_PATHS)
    leaks: dict[str, list[str]] = {}
    macros: dict[str, list[str]] = {}
    drift: list[str] = []

    for path in sources:
        with open(path, errors="replace") as handle:
            src = handle.read()
        if found := check_leaks(src):
            leaks[path] = found
        if found := check_macros(src):
            macros[path] = found
        original = git_show(args.base, path)
        if original is not None and code_fingerprint(original) != code_fingerprint(src):
            drift.append(path)

    print(f"scanned {len(sources)} files against base {args.base}")
    for title, table in (("comment leak", leaks), ("broken macro", macros)):
        print(f"{title}: {len(table)} file(s)")
        for path, found in table.items():
            print(f"  {path}")
            for item in found[:5]:
                print(f"      {item}")
    print(f"code drift: {len(drift)} file(s)")
    for path in drift:
        print(f"  {path}")

    if leaks or macros or drift:
        print("FAIL annotations altered code")
        return 1
    print("PASS annotations are comment-only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

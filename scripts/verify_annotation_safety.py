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


def check_nested_open(src: str) -> list[str]:
    """Style: a '/*' sequence inside a block comment.

    C does not nest comments, so this cannot break a build -- the outer comment
    simply runs on. It still matters twice over: annotation.md forbids it, and
    it makes the '/*' vs '*/' totals in verify_nvme_annotations.py disagree, so
    a file trips the density gate for a reason that has nothing to do with
    density. It usually comes from writing a path glob such as
    "drivers/nvme/host/*" inside prose. Reported, but not fatal, because
    pre-existing occurrences predate this check.
    """
    problems: list[str] = []
    for lineno, line in enumerate(src.split("\n"), 1):
        for match in re.finditer(r"[A-Za-z0-9_)]/\*", line):
            problems.append(f"L{lineno}: '/*' inside prose: {line.strip()[:60]}")
            break
    return problems


def check_macros(src: str) -> list[str]:
    r"""Mode 2: a comment that defeats a macro's line continuation.

    Line splicing (translation phase 2) runs *before* comments are replaced
    (phase 3). So a backslash is only a continuation when the newline follows it
    immediately -- ``#define Q(v) \ /* note */`` does NOT splice, and the macro
    silently ends at that line. Neither does a comment line placed *between* two
    continuation lines: the first backslash is then followed by comment text, not
    a newline.

    Both mistakes are invisible to the token fingerprint, because that strips
    comments before splicing and so sees a well-formed macro either way. They
    have to be caught on the raw text, which is what this does:

      1. a line whose code ends in ``\`` followed by ``/*`` or ``//``
      2. a comment-only line whose previous line ends in ``\``

    A stray backslash anywhere else in code is damage too, and is still flagged.
    """
    problems: list[str] = []
    stripped, _ = strip_comments(src)
    raw_lines = src.split("\n")
    stripped_lines = stripped.split("\n")
    trailing = re.compile(r"\\[ \t]*(?:/\*|//)")
    prev_continues = False
    for lineno, line in enumerate(stripped_lines, 1):
        raw = raw_lines[lineno - 1] if lineno - 1 < len(raw_lines) else ""
        continues = line.rstrip().endswith("\\")
        if continues and trailing.search(raw):
            problems.append(
                f"L{lineno}: comment after continuation backslash: {raw.strip()[:60]}"
            )
        elif (prev_continues and raw.strip() and not line.strip()
              and not raw.rstrip().endswith("\\")):
            # A comment line inside a macro body is fine as long as it carries
            # its own continuation backslash -- splicing runs before comment
            # removal, so the comment is joined in and then dropped. Without
            # the backslash the macro simply ends here.
            problems.append(
                f"L{lineno}: comment line ends a macro body: {raw.strip()[:60]}"
            )
        body = line.rstrip()
        if body.endswith("\\"):
            body = body[:-1]  # the legitimate continuation backslash
        if "\\" in body and any(tok == "\\" for tok in TOKEN.findall(body)):
            problems.append(
                f"L{lineno}: stray backslash in code: {line.strip()[:60]}"
            )
        prev_continues = continues
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
    nested: dict[str, list[str]] = {}
    drift: list[str] = []

    for path in sources:
        with open(path, errors="replace") as handle:
            src = handle.read()
        if found := check_leaks(src):
            leaks[path] = found
        if found := check_macros(src):
            macros[path] = found
        if found := check_nested_open(src):
            nested[path] = found
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
    nested_total = sum(len(v) for v in nested.values())
    print(f"nested '/*' in prose (style, not fatal): "
          f"{nested_total} in {len(nested)} file(s)")
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

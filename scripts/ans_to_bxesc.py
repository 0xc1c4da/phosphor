#!/usr/bin/env python3
"""
Convert .ans files to a text form that's friendly to LLMs/tools that can't open ANSI.

For each input `<name>.ans`, writes `<name>.txt` containing a Python bytes-literal style
representation using only `\\xHH` escapes (so it's unambiguous and copy/pasteable).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class Options:
    input_dir: Path
    output_dir: Path | None
    recursive: bool
    overwrite: bool
    wrap: int
    dry_run: bool


def iter_ans_files(input_dir: Path, recursive: bool) -> Iterable[Path]:
    if recursive:
        yield from input_dir.rglob("*.ans")
        yield from input_dir.rglob("*.ANS")
    else:
        yield from input_dir.glob("*.ans")
        yield from input_dir.glob("*.ANS")


def bytes_to_wrapped_bx_literal(data: bytes, wrap: int) -> str:
    """
    Return a multi-line Python-ish bytes literal that only uses \\xHH escapes.

    Example:
        b'\\x1b\\x5b\\x30\\x6d...'

    Wrapped as:
        b'...'
        b'...'
    inside parentheses, so copy/paste is straightforward.
    """
    if wrap < 16:
        raise ValueError("--wrap must be >= 16")

    hex_esc = "".join(f"\\x{b:02x}" for b in data)
    if not hex_esc:
        return "data = b''\n"

    # Wrap on escape-boundaries: each byte is 4 chars ("\\xHH")
    chunk_chars = (wrap // 4) * 4
    if chunk_chars <= 0:
        chunk_chars = 4

    chunks: list[str] = [hex_esc[i : i + chunk_chars] for i in range(0, len(hex_esc), chunk_chars)]

    out_lines: list[str] = ["data = ("]
    for c in chunks:
        out_lines.append(f"    b'{c}'")
    out_lines.append(")\n")
    return "\n".join(out_lines)


def resolve_output_path(input_file: Path, opts: Options) -> Path:
    if opts.output_dir is None:
        return input_file.with_suffix(".txt")

    rel = input_file.relative_to(opts.input_dir)
    return (opts.output_dir / rel).with_suffix(".txt")


def convert_one(input_file: Path, opts: Options) -> tuple[Path, Path] | None:
    out_path = resolve_output_path(input_file, opts)
    if out_path.exists() and not opts.overwrite:
        return None

    if opts.dry_run:
        return (input_file, out_path)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    data = input_file.read_bytes()
    text = bytes_to_wrapped_bx_literal(data, wrap=opts.wrap)
    out_path.write_text(text, encoding="utf-8", newline="\n")
    return (input_file, out_path)


def parse_args() -> Options:
    p = argparse.ArgumentParser(
        description="Convert .ans files to .txt containing a Python-style b'\\\\xHH' escaped representation."
    )
    p.add_argument("input_dir", type=Path, help="Directory containing .ans files")
    p.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Optional output directory (mirrors input tree). Default: alongside each .ans.",
    )
    p.add_argument("--recursive", action="store_true", help="Recurse into subdirectories")
    p.add_argument("--overwrite", action="store_true", help="Overwrite existing .txt outputs")
    p.add_argument(
        "--wrap",
        type=int,
        default=120,
        help="Approximate max characters per b'...' line (must be >= 16). Default: 120",
    )
    p.add_argument("--dry-run", action="store_true", help="Print planned conversions; do not write files")
    args = p.parse_args()

    input_dir = args.input_dir.expanduser().resolve()
    if not input_dir.exists() or not input_dir.is_dir():
        raise SystemExit(f"input_dir is not a directory: {input_dir}")

    output_dir = args.output_dir
    if output_dir is not None:
        output_dir = output_dir.expanduser().resolve()

    return Options(
        input_dir=input_dir,
        output_dir=output_dir,
        recursive=bool(args.recursive),
        overwrite=bool(args.overwrite),
        wrap=int(args.wrap),
        dry_run=bool(args.dry_run),
    )


def main() -> int:
    opts = parse_args()

    ans_files = sorted(iter_ans_files(opts.input_dir, opts.recursive))
    if not ans_files:
        print("No .ans files found.")
        return 0

    converted = 0
    skipped = 0
    for f in ans_files:
        res = convert_one(f, opts)
        if res is None:
            skipped += 1
            continue
        inp, outp = res
        print(f"{inp} -> {outp}")
        converted += 1

    if skipped:
        print(f"Skipped {skipped} (already exists; use --overwrite).")
    print(f"Converted {converted}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())



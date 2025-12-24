#!/usr/bin/env python3
"""
Generate C++ char32_t[256] forward tables for OEM codepages using Python's built-in codecs.

Notes:
- This prints to stdout by default (so you can redirect to a file).
- Some codecs (e.g. cp857) have undefined bytes; we map those to U+FFFD to keep tables total.
- Some historical charsets aren't available as Python codecs; for those, we parse mapping reference
  tables (e.g. references/mappings/Amiga-1251.txt).

Run with nix develop -c python3 references/mappings/generate-maps.py --out src/core/encodings_tables_generated.h
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


_SCRIPT_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _SCRIPT_DIR.parent.parent


@dataclass(frozen=True)
class EncodingSpec:
    cpp_name: str
    py_codec: str | None = None
    mapping_file: str | None = None  # repo-relative


ENCODINGS: list[EncodingSpec] = [
    EncodingSpec("Cp437", py_codec="cp437"),
    EncodingSpec("Cp850", py_codec="cp850"),
    EncodingSpec("Cp852", py_codec="cp852"),
    EncodingSpec("Cp855", py_codec="cp855"),
    EncodingSpec("Cp857", py_codec="cp857"),
    EncodingSpec("Cp860", py_codec="cp860"),
    EncodingSpec("Cp861", py_codec="cp861"),
    EncodingSpec("Cp862", py_codec="cp862"),
    EncodingSpec("Cp863", py_codec="cp863"),
    EncodingSpec("Cp865", py_codec="cp865"),
    EncodingSpec("Cp866", py_codec="cp866"),
    EncodingSpec("Cp775", py_codec="cp775"),
    EncodingSpec("Cp737", py_codec="cp737"),
    EncodingSpec("Cp869", py_codec="cp869"),
    # AmigaOS "Latin-1" (ISO-8859-1 / ECMA-94) is a reasonable baseline for many Amiga fonts.
    # Note: Topaz-style fonts commonly draw a "house" glyph at 0x7F; we patch that below.
    EncodingSpec("AmigaLatin1", py_codec="latin-1"),
    # Amiga-flavored ISO-8859-* (useful for locales / text import-export semantics).
    # We apply the same Topaz-style 0x7F -> U+2302 (HOUSE) patch for consistency with common Amiga fonts.
    EncodingSpec("AmigaIso8859_15", py_codec="iso8859-15"),  # Latin-9 (adds € and related swaps)
    EncodingSpec("AmigaIso8859_2", py_codec="iso8859-2"),    # Latin-2 (Central/Eastern European)
    # Amiga-1251 (Cyrillic, Amiga) from a published Unicode mapping table.
    EncodingSpec("Amiga1251", mapping_file="references/mappings/Amiga-1251.txt"),
]

# Canonical "PC BIOS / CP437 glyph" mapping for bytes that are often treated as drawable
# glyphs in ANSI art even though they overlap the C0 control range.
#
# This matches the in-tree CP437 table in src/core/fonts.cpp.
_PC_OEM_CONTROL_GLYPHS: dict[int, int] = {
    0x00: 0x0000,
    0x01: 0x263A,
    0x02: 0x263B,
    0x03: 0x2665,
    0x04: 0x2666,
    0x05: 0x2663,
    0x06: 0x2660,
    0x07: 0x2022,
    0x08: 0x25D8,
    0x09: 0x25CB,
    0x0A: 0x25D9,
    0x0B: 0x2642,
    0x0C: 0x2640,
    0x0D: 0x266A,
    0x0E: 0x266B,
    0x0F: 0x263C,
    0x10: 0x25BA,
    0x11: 0x25C4,
    0x12: 0x2195,
    0x13: 0x203C,
    0x14: 0x00B6,
    0x15: 0x00A7,
    0x16: 0x25AC,
    0x17: 0x21A8,
    0x18: 0x2191,
    0x19: 0x2193,
    0x1A: 0x2192,
    0x1B: 0x2190,
    0x1C: 0x221F,
    0x1D: 0x2194,
    0x1E: 0x25B2,
    0x1F: 0x25BC,
    0x7F: 0x2302,
}


def table_for(py_codec: str) -> list[int]:
    out = []
    for b in range(256):
        # Some OEM codepages have undefined bytes in Python's mapping tables (e.g. cp857).
        # Use replacement so generation is total and deterministic.
        # Only apply the PC OEM control-glyph convention to DOS/OEM "cp*" encodings.
        # Do NOT apply it to ISO-8859-* style encodings (e.g. latin-1 for Amiga).
        if py_codec.startswith("cp") and b in _PC_OEM_CONTROL_GLYPHS:
            out.append(_PC_OEM_CONTROL_GLYPHS[b])
            continue

        s = bytes([b]).decode(py_codec, errors="replace")
        assert len(s) == 1
        out.append(ord(s))
    return out


_MAP_LINE_RE = re.compile(r"^\s*0x([0-9A-Fa-f]{2})\s+0x([0-9A-Fa-f]{4,6})\b")


def table_from_mapping_file(map_path: Path) -> list[int]:
    # Parse tables in the common "Format A" style:
    #   0xA4 0x20AC #EURO
    # We ignore everything else.
    text = map_path.read_text(encoding="utf-8", errors="replace").splitlines()
    tbl: list[int] = [0xFFFD] * 256
    seen: set[int] = set()
    for line in text:
        m = _MAP_LINE_RE.match(line)
        if not m:
            continue
        b = int(m.group(1), 16)
        cp = int(m.group(2), 16)
        if not (0 <= b <= 0xFF):
            raise ValueError(f"{map_path}: byte out of range: {b!r}")
        if not (0 <= cp <= 0x10FFFF):
            raise ValueError(f"{map_path}: codepoint out of range for byte 0x{b:02X}: 0x{cp:X}")
        tbl[b] = cp
        seen.add(b)

    if len(seen) != 256:
        missing = [f"0x{i:02X}" for i in range(256) if i not in seen]
        raise ValueError(f"{map_path}: expected 256 mappings, found {len(seen)}; missing: {', '.join(missing[:16])}")
    return tbl


def patch_amiga_latin1(tbl: list[int]) -> list[int]:
    # Amiga Topaz "house" glyph: traditionally shown at byte 0x7F, where ISO-8859-1 would
    # normally be DEL (U+007F). Patch the Unicode representative to U+2302 (HOUSE).
    #
    # This keeps round-trip behavior sane for tools/UI that want to show that glyph.
    if len(tbl) == 256:
        tbl = list(tbl)
        tbl[0x7F] = 0x2302
    return tbl


def patch_amiga_house_at_7f(tbl: list[int]) -> list[int]:
    # Many Amiga bitmap fonts (Topaz lineage) draw a "house" glyph at 0x7F, where ISO-8859-*
    # defines DEL (U+007F). Patch the Unicode representative to U+2302 (HOUSE).
    if len(tbl) == 256:
        tbl = list(tbl)
        tbl[0x7F] = 0x2302
    return tbl


def _cpp_char32(cp: int) -> str:
    # Prefer explicit hex codepoint constants (avoids escaping issues in generated code).
    return f"(char32_t)0x{cp:04X}" if cp <= 0xFFFF else f"(char32_t)0x{cp:X}"


def emit_cpp(name: str, tbl: list[int]) -> str:
    items = [_cpp_char32(cp) for cp in tbl]
    lines = []
    lines.append(f"static constexpr char32_t k{name}[256] = {{")
    # 16 entries per line for readability.
    for i in range(0, 256, 16):
        chunk = ", ".join(items[i : i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines) + "\n"

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--out",
        help="Output file path. If omitted, writes to stdout (redirect with > file).",
        default="",
    )
    args = ap.parse_args()

    out_s = []
    out_s.append("// Generated by generate-maps.py")
    out_s.append("#pragma once")
    out_s.append("#include <cstdint>")
    out_s.append("")
    out_s.append("namespace phos::encodings {")
    out_s.append("")
    for spec in ENCODINGS:
        if spec.py_codec:
            tbl = table_for(spec.py_codec)
        elif spec.mapping_file:
            tbl = table_from_mapping_file(_REPO_ROOT / spec.mapping_file)
        else:
            raise ValueError(f"Invalid EncodingSpec: {spec!r}")

        if spec.cpp_name == "AmigaLatin1":
            tbl = patch_amiga_latin1(tbl)
        elif spec.cpp_name in ("AmigaIso8859_15", "AmigaIso8859_2"):
            tbl = patch_amiga_house_at_7f(tbl)
        out_s.append(emit_cpp(spec.cpp_name, tbl))
    out_s.append("} // namespace phos::encodings")
    out_s.append("")

    text = "\n".join(out_s)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
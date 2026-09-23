#!/usr/bin/env python3
"""Prove the Russobit UI-bind hook seam against the exact supported executable.

Address provenance:
  * ``0x5C93D6`` is ``CButtonInterf::assignFunctor`` in the Russobit build.  The
    five-argument ``__stdcall`` ABI and the direct xrefs are documented in the
    source lobby's ``RE/13-auto-nav-functor-invoke.md`` and
    ``RE/14-address-derivation.md``.
  * The last working lobby harness supplied 173 sites from
    ``dll/winmm_proxy/hooks.cpp::kBindCallSites``.  A byte-level proof against
    the pinned image found five additional real, instruction-boundary calls
    hidden from that IDA xref query by two incorrectly inferred noreturn
    functions: 0x6306F8, 0x630768, 0x6307D8, 0x638CE4 and 0x638D98.
  * MSS patches those direct CALL operands, not the function entry: C4 timerhost
    is allowed to Detour the entry and both observers then chain exactly once.

This proof intentionally accepts only the one initially supported game image.
It parses PE32 itself (no optional packages), verifies the exact identity and
entry bytes, scans the complete .text raw section for every E8 rel32 candidate
that targets 0x5C93D6, and requires exact set equality with the C++ manifest.
It never modifies the executable or source.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path


RUSSOBIT_SIZE = 4_187_648
RUSSOBIT_SHA256 = "1375cdef09ec470ee64fe5693fb734d7c69fb215212311d997f792b258a642eb"
RUSSOBIT_IMAGE_BASE = 0x00400000
ASSIGN_FUNCTOR_VA = 0x005C93D6
ASSIGN_FUNCTOR_BYTES = bytes.fromhex(
    "B8 E8 B9 6B 00 E8 F0 3F 0A 00 81 EC 08 04 00 00"
)
EXPECTED_XREF_COUNT = 178


class ProofError(RuntimeError):
    pass


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_pe32(data: bytes) -> tuple[int, list[tuple[str, int, int, int, int]]]:
    if data[:2] != b"MZ":
        raise ProofError("missing DOS MZ signature")
    pe_offset = u32(data, 0x3C)
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ProofError("missing PE signature")

    section_count = u16(data, pe_offset + 6)
    optional_size = u16(data, pe_offset + 20)
    optional_offset = pe_offset + 24
    if u16(data, optional_offset) != 0x10B:
        raise ProofError("expected a PE32 optional header")
    image_base = u32(data, optional_offset + 28)

    sections: list[tuple[str, int, int, int, int]] = []
    section_offset = optional_offset + optional_size
    for index in range(section_count):
        header = section_offset + index * 40
        name = data[header : header + 8].split(b"\0", 1)[0].decode("ascii")
        virtual_size = u32(data, header + 8)
        virtual_address = u32(data, header + 12)
        raw_size = u32(data, header + 16)
        raw_offset = u32(data, header + 20)
        sections.append((name, virtual_address, virtual_size, raw_offset, raw_size))
    return image_base, sections


def va_to_file_offset(
    va: int,
    image_base: int,
    sections: list[tuple[str, int, int, int, int]],
) -> int:
    rva = va - image_base
    for _name, section_rva, virtual_size, raw_offset, raw_size in sections:
        if section_rva <= rva < section_rva + max(virtual_size, raw_size):
            delta = rva - section_rva
            if delta >= raw_size:
                raise ProofError(f"VA 0x{va:08X} has no bytes in the PE file")
            return raw_offset + delta
    raise ProofError(f"VA 0x{va:08X} is outside all PE sections")


def source_manifest(source: str) -> list[int]:
    match = re.search(
        r"constexpr\s+uintptr_t\s+kBindCallSites\[\]\s*=\s*\{(?P<body>.*?)\};",
        source,
        re.DOTALL,
    )
    if not match:
        raise ProofError("kBindCallSites manifest not found in uistatereporter.cpp")
    sites = [int(token, 16) for token in re.findall(r"0x[0-9A-Fa-f]+", match["body"])]
    if len(sites) != EXPECTED_XREF_COUNT:
        raise ProofError(f"source manifest has {len(sites)} sites, expected 178")
    if sites != sorted(set(sites)):
        raise ProofError("source manifest must be strictly ordered and unique")
    return sites


def direct_call_candidates(
    data: bytes,
    image_base: int,
    sections: list[tuple[str, int, int, int, int]],
) -> list[int]:
    text_sections = [section for section in sections if section[0] == ".text"]
    if len(text_sections) != 1:
        raise ProofError(f"expected exactly one .text section, found {len(text_sections)}")
    _name, text_rva, _virtual_size, raw_offset, raw_size = text_sections[0]
    text = data[raw_offset : raw_offset + raw_size]

    sites: list[int] = []
    for offset in range(0, len(text) - 4):
        if text[offset] != 0xE8:
            continue
        displacement = struct.unpack_from("<i", text, offset + 1)[0]
        site = image_base + text_rva + offset
        if site + 5 + displacement == ASSIGN_FUNCTOR_VA:
            sites.append(site)
    return sites


def prove(exe_path: Path, source_path: Path) -> None:
    data = exe_path.read_bytes()
    if len(data) != RUSSOBIT_SIZE:
        raise ProofError(f"EXE size is {len(data):,}, expected {RUSSOBIT_SIZE:,}")
    digest = hashlib.sha256(data).hexdigest()
    if digest != RUSSOBIT_SHA256:
        raise ProofError(f"EXE SHA-256 is {digest}, expected {RUSSOBIT_SHA256}")

    image_base, sections = parse_pe32(data)
    if image_base != RUSSOBIT_IMAGE_BASE:
        raise ProofError(f"PE ImageBase is 0x{image_base:08X}, expected 0x00400000")

    entry_offset = va_to_file_offset(ASSIGN_FUNCTOR_VA, image_base, sections)
    actual_entry = data[entry_offset : entry_offset + len(ASSIGN_FUNCTOR_BYTES)]
    if actual_entry != ASSIGN_FUNCTOR_BYTES:
        raise ProofError(
            "assignFunctor entry bytes differ: "
            f"{actual_entry.hex(' ').upper()} != {ASSIGN_FUNCTOR_BYTES.hex(' ').upper()}"
        )

    manifest = source_manifest(source_path.read_text(encoding="utf-8"))
    discovered = direct_call_candidates(data, image_base, sections)
    if discovered != manifest:
        missing = sorted(set(discovered) - set(manifest))
        extra = sorted(set(manifest) - set(discovered))
        raise ProofError(
            "direct-xref set differs from the MSS manifest; "
            f"missing-in-source={[f'0x{x:08X}' for x in missing]}, "
            f"not-in-exe={[f'0x{x:08X}' for x in extra]}"
        )

    print("Russobit UI-bind seam proof PASS")
    print(f"  EXE: {exe_path}")
    print(f"  size/SHA-256: {len(data):,} / {digest.upper()}")
    print(f"  ImageBase: 0x{image_base:08X}")
    print(f"  assignFunctor: VA 0x{ASSIGN_FUNCTOR_VA:08X}, file offset 0x{entry_offset:08X}")
    print(f"  direct E8 xrefs: {len(discovered)}; exact equality with MSS manifest")


def main() -> int:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path, help="exact Russobit Discipl2.exe")
    parser.add_argument(
        "--source",
        type=Path,
        default=repository / "mss32/src/testdrv/uistatereporter.cpp",
        help="MSS UI reporter source containing kBindCallSites",
    )
    args = parser.parse_args()
    try:
        prove(args.exe.resolve(), args.source.resolve())
    except (OSError, ProofError, struct.error, UnicodeError) as error:
        print(f"Russobit UI-bind seam proof FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


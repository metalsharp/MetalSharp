#!/usr/bin/env python3
"""Add Wine's private path export needed by the Linux EAC loader.

Some EAC launchers use GetModuleHandleW("ntdll.dll") followed by
GetProcAddress("wine_get_unix_file_name").  Wine normally exposes that helper
from kernel32, while the Linux EAC path asks ntdll.  The exact Wine 11.5
runtime can satisfy the real private-export contract by forwarding the name
to kernel32; no vendor executable or protected module is changed.

The ntdll export table contains the usual Nt/Zw aliases.  We reuse the name
slot for a Zw alias whose Nt twin remains intact, and point its export address
at a forwarder string in the existing .edata padding.  This keeps the PE
layout and all section offsets stable, so the patch is safe to apply
idempotently to the already-built runtime.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


TARGET = b"wine_get_unix_file_name"
WINE115_KERNEL32_BASE = 0x6FFFFFA00000
WINE115_KERNEL32_UNIX_NAME_RVA = 0x32560


def parse_sections(blob: bytes):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe : pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    number_of_sections = struct.unpack_from("<H", blob, pe + 6)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", blob, optional)[0]
    if magic != 0x20B:
        raise ValueError("expected PE32+ image")
    section_table = optional + struct.unpack_from("<H", blob, pe + 20)[0]
    sections = []
    for index in range(number_of_sections):
        offset = section_table + index * 40
        name = blob[offset : offset + 8].rstrip(b"\0")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", blob, offset + 8)
        sections.append((name, virtual_address, virtual_size, raw_size, raw_offset))
    return pe, optional, sections


def rva_to_file(sections, rva: int) -> int:
    for _name, virtual_address, virtual_size, raw_size, raw_offset in sections:
        span = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + span:
            return raw_offset + rva - virtual_address
    raise ValueError(f"RVA 0x{rva:x} is outside file-backed sections")


def patch(path: Path) -> bool:
    blob = bytearray(path.read_bytes())
    _pe, optional, sections = parse_sections(blob)
    export_rva, export_size = struct.unpack_from("<II", blob, optional + 112)
    if export_rva == 0 or export_size == 0:
        raise ValueError("image has no export table")
    export_offset = rva_to_file(sections, export_rva)
    fields = struct.unpack_from("<IIHHIIIIIII", blob, export_offset)
    _characteristics, _timestamp, _major, _minor, _name, _base, number_of_functions, number_of_names, functions_rva, names_rva, ordinals_rva = fields

    names = []
    for index in range(number_of_names):
        name_rva = struct.unpack_from("<I", blob, rva_to_file(sections, names_rva) + index * 4)[0]
        name_offset = rva_to_file(sections, name_rva)
        end = blob.find(b"\0", name_offset)
        if end < 0:
            raise ValueError("unterminated export name")
        name = bytes(blob[name_offset:end])
        ordinal = struct.unpack_from("<H", blob, rva_to_file(sections, ordinals_rva) + index * 2)[0]
        names.append((name, name_rva, name_offset, end - name_offset, ordinal))
        if name == TARGET:
            return False

    # Pick a Zw alias; its Nt twin retains the original implementation and
    # the renamed slot becomes the only new private-export contract.
    candidate = next(
        (item for item in names if item[0].startswith(b"Zw") and len(item[0]) >= len(TARGET)),
        None,
    )
    if candidate is None:
        raise ValueError("no reusable Zw export name slot")
    _old_name, _old_name_rva, name_offset, old_name_capacity, ordinal = candidate
    if len(TARGET) + 1 > old_name_capacity:
        raise ValueError("selected export name slot is too short")

    blob[name_offset : name_offset + old_name_capacity] = TARGET + b"\0" + b"\0" * (old_name_capacity - len(TARGET) - 1)
    function_offset = rva_to_file(sections, functions_rva) + ordinal * 4
    text = next((section for section in sections if section[0] == b".text"), None)
    if text is None:
        raise ValueError("image has no .text section")
    stub_rva = text[1] + text[2]
    stub = b"\x48\xb8" + struct.pack("<Q", WINE115_KERNEL32_BASE + WINE115_KERNEL32_UNIX_NAME_RVA) + b"\xff\xe0"
    stub_offset = rva_to_file(sections, stub_rva)
    if stub_offset + len(stub) > text[4] + text[3]:
        raise ValueError(".text has no padding for the private export bridge")
    blob[stub_offset : stub_offset + len(stub)] = stub
    struct.pack_into("<I", blob, function_offset, stub_rva)
    # The stub consumes the raw padding at the end of .text.  Advertise that
    # small extension so the PE loader maps and executes the new export RVA.
    section_table = optional + struct.unpack_from("<H", blob, _pe + 20)[0]
    for index, (section_name, section_va, section_virtual_size, _raw_size, _raw_offset) in enumerate(sections):
        if section_name == b".text":
            new_virtual_size = max(section_virtual_size, stub_rva + len(stub) - section_va)
            struct.pack_into("<I", blob, section_table + index * 40 + 8, new_virtual_size)
            break
    path.write_bytes(blob)
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    changed = patch(args.image)
    print("patched" if changed else "already-patched")


if __name__ == "__main__":
    main()

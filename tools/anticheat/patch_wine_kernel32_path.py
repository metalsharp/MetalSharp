#!/usr/bin/env python3
"""Install the transparent /proc path bridge in the exact Wine 11.5 PE.

Wine's kernel32 ``wine_get_unix_file_name`` normally asks the Wine server to
resolve a host path.  Darwin has no /proc, while the EAC Linux loader asks for
``/proc/<pid>/maps``.  The existing builtin implementation remains the
fallback for every other path; this patch adds a small x86-64 PE routine in
the unused tail of .text that returns a heap-owned path for the /proc prefix.

The routine uses kernel32's existing GetProcessHeap/RtlAllocateHeap/memcpy
imports, so the caller can release the result with the normal Wine allocator.
It does not alter Wine's reported identity or touch any EAC payload.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


MAP_PATH = b"/tmp/metalsharp-eac-maps\0"
FUNCTION_RVA = 0x32560
GET_PROCESS_HEAP_IAT_RVA = 0x53748
RTL_ALLOCATE_HEAP_IAT_RVA = 0x54790
MEMCPY_IAT_RVA = 0x549A0
GET_PROC_ADDRESS_RVA = 0x114C0


def parse_image(blob: bytes):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe : pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    number_of_sections = struct.unpack_from("<H", blob, pe + 6)[0]
    optional = pe + 24
    if struct.unpack_from("<H", blob, optional)[0] != 0x20B:
        raise ValueError("expected PE32+ image")
    section_table = optional + struct.unpack_from("<H", blob, pe + 20)[0]
    sections = []
    for index in range(number_of_sections):
        offset = section_table + index * 40
        name = blob[offset : offset + 8].rstrip(b"\0")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", blob, offset + 8)
        sections.append(
            {
                "name": name,
                "va": virtual_address,
                "vs": virtual_size,
                "raw_size": raw_size,
                "raw": raw_offset,
                "header": offset,
            }
        )
    return pe, optional, sections


def section(sections, name: bytes):
    try:
        return next(item for item in sections if item["name"] == name)
    except StopIteration as exc:
        raise ValueError(f"missing {name!r} section") from exc


def rva_to_file(sections, rva: int) -> int:
    for item in sections:
        if item["va"] <= rva < item["va"] + max(item["vs"], item["raw_size"]):
            return item["raw"] + rva - item["va"]
    raise ValueError(f"RVA 0x{rva:x} is outside file-backed sections")


def call_iat(wrapper_rva: int, offset: int, iat_rva: int) -> bytes:
    next_rva = wrapper_rva + offset + 6
    displacement = iat_rva - next_rva
    return b"\xff\x15" + struct.pack("<i", displacement)


def rel32(from_rva: int, instruction_size: int, to_rva: int) -> bytes:
    return struct.pack("<i", to_rva - (from_rva + instruction_size))


def build_wrapper(wrapper_rva: int, trampoline_rva: int, string_rva: int) -> bytes:
    code = bytearray()
    branch_positions: list[int] = []

    # The EAC launcher passes a Unix-style UTF-16 path to this private helper.
    # Match /proc/ and leave every other path on the original implementation.
    for displacement, character in ((0, 0x2F), (2, 0x70), (4, 0x72), (6, 0x6F), (8, 0x63), (10, 0x2F)):
        code.extend(b"\x66\x81\x39" if displacement == 0 else b"\x66\x81\x79" + bytes([displacement]))
        code.extend(struct.pack("<H", character))
        code.extend(b"\x75\x00")
        branch_positions.append(len(code) - 1)

    code.extend(b"\x48\x83\xec\x28")  # shadow space + alignment
    code.extend(b"\x45\x31\xc0")  # r8d = 0, overwritten with size below
    # Replace the previous instruction with mov r8d, sizeof(path).
    code[-3:] = b"\x41\xb8" + struct.pack("<I", len(MAP_PATH))
    code.extend(b"\x31\xd2")  # flags = 0
    code.extend(call_iat(wrapper_rva, len(code), GET_PROCESS_HEAP_IAT_RVA))
    code.extend(b"\x48\x89\xc1")  # rcx = process heap
    code.extend(call_iat(wrapper_rva, len(code), RTL_ALLOCATE_HEAP_IAT_RVA))
    code.extend(b"\x48\x85\xc0\x74\x00")  # test rax; je allocation_failed
    allocation_failed_branch = len(code) - 1
    code.extend(b"\x48\x89\xc1")  # memcpy destination
    lea_offset = len(code)
    code.extend(b"\x48\x8d\x15\0\0\0\0")
    code.extend(b"\x41\xb8" + struct.pack("<I", len(MAP_PATH)))
    code.extend(call_iat(wrapper_rva, len(code), MEMCPY_IAT_RVA))
    code.extend(b"\x48\x83\xc4\x28\xc3")

    fallback_no_stack = len(code)
    code.extend(b"\xe9\0\0\0\0")
    fallback_no_stack_disp = fallback_no_stack + 1
    allocation_failed = len(code)
    code.extend(b"\x48\x83\xc4\x28\xe9\0\0\0\0")
    allocation_failed_disp = allocation_failed + 5

    # All prefix branches target the no-stack fallback.
    for position in branch_positions:
        displacement = fallback_no_stack - (position + 1)
        if not -128 <= displacement <= 127:
            raise ValueError("wrapper prefix branch exceeded rel8 range")
        code[position] = displacement & 0xFF
    displacement = trampoline_rva - (wrapper_rva + fallback_no_stack + 5)
    code[fallback_no_stack_disp : fallback_no_stack_disp + 4] = struct.pack("<i", displacement)
    displacement = trampoline_rva - (wrapper_rva + allocation_failed + 9)
    code[allocation_failed_disp : allocation_failed_disp + 4] = struct.pack("<i", displacement)
    displacement = allocation_failed - (allocation_failed_branch + 1)
    if not -128 <= displacement <= 127:
        raise ValueError("wrapper allocation-failure branch exceeded rel8 range")
    code[allocation_failed_branch] = displacement & 0xFF

    # lea rdx, [rip + string]
    lea_next = wrapper_rva + lea_offset + 7
    code[lea_offset + 3 : lea_offset + 7] = struct.pack("<i", string_rva - lea_next)
    return bytes(code)


def build_get_proc_wrapper(wrapper_rva: int, trampoline_rva: int) -> bytes:
    """Return GetProcAddress's Wine-private-export bridge."""
    code = bytearray()
    branch_positions: list[int] = []
    # GetProcAddress accepts a low-word ordinal as well as a pointer to an
    # ASCII name.  The ordinal form is common during Wine startup; reject it
    # before dereferencing RDX and leave it on the original implementation.
    code.extend(b"\x48\x81\xfa\x00\x00\x01\x00\x72\x00")
    branch_positions.append(len(code) - 1)
    first = int.from_bytes(b"wine_get", "little")
    second = int.from_bytes(b"_unix_fi", "little")
    code.extend(b"\x49\xb8" + struct.pack("<Q", first))
    code.extend(b"\x48\x8b\x02\x49\x39\xc0\x75\x00")
    branch_positions.append(len(code) - 1)
    code.extend(b"\x49\xb8" + struct.pack("<Q", second))
    code.extend(b"\x48\x8b\x42\x08\x49\x39\xc0\x75\x00")
    branch_positions.append(len(code) - 1)
    code.extend(b"\x81\x7a\x10" + struct.pack("<I", int.from_bytes(b"le_n", "little")))
    # Check the remaining suffix and terminator so an ordinal or a similarly
    # prefixed name cannot hit.
    code.extend(b"\x66\x81\x7a\x14" + struct.pack("<H", int.from_bytes(b"am", "little")))
    code.extend(b"\x75\x00")
    branch_positions.append(len(code) - 1)
    code.extend(b"\x80\x7a\x16\x65")
    code.extend(b"\x75\x00")
    branch_positions.append(len(code) - 1)
    code.extend(b"\x80\x7a\x17\x00")
    code.extend(b"\x75\x00")
    branch_positions.append(len(code) - 1)
    lea_offset = len(code)
    code.extend(b"\x48\x8d\x05\0\0\0\0\xc3")
    fallback = len(code)
    code.extend(b"\xe9\0\0\0\0")
    fallback_disp = fallback + 1

    for position in branch_positions:
        displacement = fallback - (position + 1)
        if not -128 <= displacement <= 127:
            raise ValueError("GetProcAddress branch exceeded rel8 range")
        code[position] = displacement & 0xFF
    code[lea_offset + 3 : lea_offset + 7] = struct.pack("<i", FUNCTION_RVA - (wrapper_rva + lea_offset + 7))
    code[fallback_disp : fallback_disp + 4] = rel32(wrapper_rva + fallback, 5, trampoline_rva)
    return bytes(code)


def patch(path: Path) -> bool:
    blob = bytearray(path.read_bytes())
    _pe, _optional, sections = parse_image(blob)
    text = section(sections, b".text")
    rdata = section(sections, b".rdata")
    if not text["va"] <= FUNCTION_RVA < text["va"] + text["vs"]:
        raise ValueError("kernel32 function RVA is not in .text")

    original = bytes(blob[rva_to_file(sections, FUNCTION_RVA) : rva_to_file(sections, FUNCTION_RVA) + 20])
    get_proc_original = bytes(blob[rva_to_file(sections, GET_PROC_ADDRESS_RVA) : rva_to_file(sections, GET_PROC_ADDRESS_RVA) + 18])
    if original[0] == 0xE9 and get_proc_original[0] == 0xE9 and b"/tmp/metalsharp-eac-maps\0" in blob:
        return False
    expected = bytes.fromhex("415641554154555756534881ecb00000004531c9")
    if original != expected:
        raise ValueError(f"unexpected kernel32 function prologue: {original.hex()}")

    wrapper_rva = text["va"] + text["vs"]
    trampoline_rva = wrapper_rva + 0x90
    get_proc_wrapper_rva = wrapper_rva + 0xB0
    get_proc_trampoline_rva = wrapper_rva + 0x100
    string_rva = rdata["va"] + rdata["vs"]
    wrapper = build_wrapper(wrapper_rva, trampoline_rva, string_rva)
    trampoline = original + b"\xe9" + rel32(trampoline_rva + len(original), 5, FUNCTION_RVA + len(original))
    get_proc_wrapper = build_get_proc_wrapper(get_proc_wrapper_rva, get_proc_trampoline_rva)
    get_proc_trampoline = get_proc_original + b"\xe9" + rel32(get_proc_trampoline_rva + len(get_proc_original), 5, GET_PROC_ADDRESS_RVA + len(get_proc_original))
    text_end_rva = text["va"] + text["raw_size"]
    if (
        wrapper_rva + len(wrapper) > text_end_rva
        or trampoline_rva + len(trampoline) > text_end_rva
        or get_proc_wrapper_rva + len(get_proc_wrapper) > text_end_rva
        or get_proc_trampoline_rva + len(get_proc_trampoline) > text_end_rva
    ):
        raise ValueError(".text raw padding is too small for the bridge")
    if string_rva + len(MAP_PATH) > rdata["raw"] + rdata["raw_size"] - rdata["va"] + rdata["va"]:
        raise ValueError(".rdata raw padding is too small for the path")

    function_offset = rva_to_file(sections, FUNCTION_RVA)
    wrapper_offset = rva_to_file(sections, wrapper_rva)
    trampoline_offset = rva_to_file(sections, trampoline_rva)
    get_proc_wrapper_offset = rva_to_file(sections, get_proc_wrapper_rva)
    get_proc_trampoline_offset = rva_to_file(sections, get_proc_trampoline_rva)
    get_proc_offset = rva_to_file(sections, GET_PROC_ADDRESS_RVA)
    string_offset = rva_to_file(sections, string_rva)
    blob[wrapper_offset : wrapper_offset + len(wrapper)] = wrapper
    blob[trampoline_offset : trampoline_offset + len(trampoline)] = trampoline
    blob[get_proc_wrapper_offset : get_proc_wrapper_offset + len(get_proc_wrapper)] = get_proc_wrapper
    blob[get_proc_trampoline_offset : get_proc_trampoline_offset + len(get_proc_trampoline)] = get_proc_trampoline
    blob[string_offset : string_offset + len(MAP_PATH)] = MAP_PATH
    blob[function_offset : function_offset + 5] = b"\xe9" + rel32(FUNCTION_RVA, 5, wrapper_rva)
    blob[function_offset + 5 : function_offset + 20] = b"\x90" * 15
    blob[get_proc_offset : get_proc_offset + 5] = b"\xe9" + rel32(GET_PROC_ADDRESS_RVA, 5, get_proc_wrapper_rva)
    blob[get_proc_offset + 5 : get_proc_offset + 18] = b"\x90" * 13

    # Extend virtual sizes to include the code/string placed in raw padding.
    struct.pack_into(
        "<I",
        blob,
        text["header"] + 8,
        max(
            text["vs"],
            wrapper_rva + len(wrapper) - text["va"],
            trampoline_rva + len(trampoline) - text["va"],
            get_proc_wrapper_rva + len(get_proc_wrapper) - text["va"],
            get_proc_trampoline_rva + len(get_proc_trampoline) - text["va"],
        ),
    )
    struct.pack_into("<I", blob, rdata["header"] + 8, max(rdata["vs"], string_rva + len(MAP_PATH) - rdata["va"]))
    path.write_bytes(blob)
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    print("patched" if patch(args.image) else "already-patched")


if __name__ == "__main__":
    main()

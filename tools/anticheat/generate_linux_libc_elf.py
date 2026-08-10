#!/usr/bin/env python3
"""Build the small ELF symbol image used by MetalSharp's Darwin substrate.

The EAC Wine launcher does not ask dyld to load libc.  It reads the ELF image
named by a Linux-style /proc/<pid>/maps entry, then resolves a handful of
symbols from its SysV hash table.  This file is deliberately a normal ET_DYN
image with a PT_DYNAMIC segment and a real ELF symbol table; the Darwin
substrate fills the absolute jump targets after it maps the image.

This is not an EAC payload and does not contain vendor code.  It is the ABI
boundary owned by MetalSharp.  Keeping its generation deterministic makes it
possible to inspect and test the exact image without touching the protected
module downloaded by EAC.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


PAGE = 0x1000
STUB_SIZE = 16
CODE_OFF = PAGE
STR_OFF = 0x2000
SYM_OFF = 0x3000
HASH_OFF = 0x4000
DYNAMIC_OFF = 0x5000
SHSTR_OFF = 0x6000
SHOFF = 0x7000
FILE_SIZE = 0x7400


# The loader can resolve functions that are not needed by the launcher's
# initial __libc_dlopen_mode/__libc_dlsym lookup as well.  Keeping the names in
# the ELF image gives the loaded Linux module the same symbol namespace that
# it would see from a glibc process.  Target addresses are patched by the
# native substrate, never by changing the EAC payload.
SYMBOLS = [
    "__libc_dlopen_mode",
    "dlopen",
    "__libc_dlsym",
    "dlsym",
    "dlclose",
    "dlerror",
    "malloc",
    "calloc",
    "realloc",
    "free",
    "memcpy",
    "memmove",
    "memset",
    "memcmp",
    "strlen",
    "strcmp",
    "strncmp",
    "strstr",
    "strchr",
    "strrchr",
    "open",
    "open64",
    "close",
    "read",
    "pread",
    "write",
    "lseek",
    "fstat",
    "mmap",
    "munmap",
    "mprotect",
    "getpid",
    "getppid",
    "getenv",
    "abort",
    "exit",
    "pthread_create",
    "pthread_join",
    "pthread_detach",
    "pthread_self",
    "pthread_mutex_init",
    "pthread_mutex_destroy",
    "pthread_mutex_lock",
    "pthread_mutex_unlock",
    "pthread_cond_init",
    "pthread_cond_destroy",
    "pthread_cond_wait",
    "pthread_cond_signal",
    "clock_gettime",
    "syscall",
    "__errno_location",
    "__stack_chk_fail",
    "raise",
]


def align(value: int, boundary: int = PAGE) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def sysv_hash(name: bytes) -> int:
    value = 0
    for byte in name:
        value = (value << 4) + byte
        high = value & 0xF0000000
        if high:
            value ^= high >> 24
            value ^= high
    return value & 0xFFFFFFFF


def build() -> bytes:
    names = ["libc.so.6", *SYMBOLS]
    dynstr = bytearray(b"\0")
    name_offsets: dict[str, int] = {}
    for name in names:
        name_offsets[name] = len(dynstr)
        dynstr.extend(name.encode("ascii") + b"\0")

    symbol_count = len(SYMBOLS) + 1
    dynsym = bytearray(symbol_count * 24)
    for index, name in enumerate(SYMBOLS, start=1):
        struct.pack_into(
            "<IBBHQQ",
            dynsym,
            index * 24,
            name_offsets[name],
            0x12,  # STB_GLOBAL | STT_FUNC
            0,
            1,  # .text
            CODE_OFF + (index - 1) * STUB_SIZE,
            STUB_SIZE,
        )

    # SysV hash: one bucket and one chain per symbol.  The EAC launcher uses
    # the same 33-based hash when GNU_HASH is absent.
    bucket_count = 1
    hash_table = bytearray(struct.pack("<II", bucket_count, symbol_count))
    hash_table.extend(struct.pack("<I", 1))
    for index in range(1, symbol_count):
        hash_table.extend(struct.pack("<I", index + 1 if index + 1 < symbol_count else 0))

    dynamic = bytearray()
    entries = [
        (4, HASH_OFF),
        (5, STR_OFF),
        (6, SYM_OFF),
        (10, len(dynstr)),
        (11, 24),
        (14, name_offsets["libc.so.6"]),
        (0, 0),
    ]
    for tag, value in entries:
        dynamic.extend(struct.pack("<qQ", tag, value))

    shstr = b"\0.text\0.dynstr\0.dynsym\0.hash\0.dynamic\0.shstrtab\0"
    section_name = {
        "text": shstr.index(b".text"),
        "dynstr": shstr.index(b".dynstr"),
        "dynsym": shstr.index(b".dynsym"),
        "hash": shstr.index(b".hash"),
        "dynamic": shstr.index(b".dynamic"),
        "shstrtab": shstr.index(b".shstrtab"),
    }

    # ELF64 section header: name, type, flags, addr, offset, size, link,
    # info, addralign, entsize.
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (section_name["text"], 1, 0x6, CODE_OFF, CODE_OFF, len(SYMBOLS) * STUB_SIZE, 0, 0, 16, 0),
        (section_name["dynstr"], 3, 0x2, STR_OFF, STR_OFF, len(dynstr), 0, 0, 1, 0),
        (section_name["dynsym"], 11, 0x2, SYM_OFF, SYM_OFF, len(dynsym), 2, 1, 8, 24),
        (section_name["hash"], 5, 0x2, HASH_OFF, HASH_OFF, len(hash_table), 3, 0, 8, 4),
        (section_name["dynamic"], 6, 0x3, DYNAMIC_OFF, DYNAMIC_OFF, len(dynamic), 2, 0, 8, 16),
        (section_name["shstrtab"], 3, 0, SHSTR_OFF, SHSTR_OFF, len(shstr), 0, 0, 1, 0),
    ]

    blob = bytearray(FILE_SIZE)
    # ELF identification, class/data/version/OSABI.
    blob[:16] = b"\x7fELF\x02\x01\x01\0" + b"\0" * 8
    struct.pack_into(
        "<HHIQQQIHHHHHH",
        blob,
        16,
        3,  # ET_DYN
        62,  # EM_X86_64
        1,
        0,
        64,
        SHOFF,
        0,
        64,
        56,
        2,
        64,
        len(sections),
        6,
    )

    # PT_LOAD covers the image so the substrate can map the artifact as one
    # contiguous address range.  PT_DYNAMIC points at the dynamic table.
    struct.pack_into("<IIQQQQQQ", blob, 64, 1, 7, 0, 0, 0, FILE_SIZE, FILE_SIZE, PAGE)
    struct.pack_into("<IIQQQQQQ", blob, 64 + 56, 2, 6, DYNAMIC_OFF, DYNAMIC_OFF, DYNAMIC_OFF, len(dynamic), len(dynamic), 8)

    for index, name in enumerate(SYMBOLS):
        offset = CODE_OFF + index * STUB_SIZE
        # movabs rax, imm64; jmp rax; nop...  The host substrate patches the
        # immediate with the corresponding Darwin function address.
        blob[offset : offset + STUB_SIZE] = b"\x48\xb8" + b"\0" * 8 + b"\xff\xe0\x90\x90"

    blob[STR_OFF : STR_OFF + len(dynstr)] = dynstr
    blob[SYM_OFF : SYM_OFF + len(dynsym)] = dynsym
    blob[HASH_OFF : HASH_OFF + len(hash_table)] = hash_table
    blob[DYNAMIC_OFF : DYNAMIC_OFF + len(dynamic)] = dynamic
    blob[SHSTR_OFF : SHSTR_OFF + len(shstr)] = shstr
    for index, section in enumerate(sections):
        struct.pack_into("<IIQQQQIIQQ", blob, SHOFF + index * 64, *section)
    return bytes(blob)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build())


if __name__ == "__main__":
    main()

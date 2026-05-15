"""Decode hl_type tags discovered by v5 into class names.

HashLink hl_type layout (libhl 1.15, x64):
  +0  u32   kind   (HOBJ = 11)
  +4  pad
  +8  ptr   obj    (hl_type_obj* for HOBJ)
  +16 ptr   t_params (nullable)
  ...

hl_type_obj (libhl 1.15 source confirms order):
  +0  i32   nfields
  +4  i32   nproto
  +8  i32   nbindings
  +12 pad
  +16 ptr   name    (UCS-2/UTF-16 string, null-terminated)
  +24 ptr   super
  ...

Strategy: for each candidate type tag, read kind, then obj pointer,
then name pointer, then read utf-16 until null. If anything looks
malformed, mark as 'unknown'.
"""
from __future__ import annotations
import struct
import sys
from pathlib import Path

PROJECT_TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(PROJECT_TOOLS))
from probe import attach   # type: ignore[import-not-found]


# Top tags from v5 run (hit count in parens)
CANDIDATES = [
    (0x00000199291a1f78, 270),
    (0x00000199291a3158, 223),
    (0x00007ff86e5b3da0, 133),
    (0x00007ff86e5b3de0, 102),
    (0x0000010000000100,  99),
    (0x000001992919d178,  94),
    (0x000001992923e5d8,  68),
    (0x00000199bb2fff90,  57),
    (0x0000019a51c24140,  56),
    (0x00000199291a0258,  55),
    (0x000001992925ad98,  44),
    (0x00000199292ae2f8,  41),
    (0x0000019a4e37ae68,  38),
    (0x000001992812a260,  33),
    (0x0000000100000000,  30),
]

HL_KIND_NAMES = {
    0:"VOID",1:"UI8",2:"UI16",3:"I32",4:"I64",5:"F32",6:"F64",7:"BOOL",
    8:"BYTES",9:"DYN",10:"FUN",11:"OBJ",12:"ARRAY",13:"TYPE",14:"REF",
    15:"VIRTUAL",16:"DYNOBJ",17:"ABSTRACT",18:"ENUM",19:"NULL",20:"METHOD",
    21:"STRUCT",22:"PACKED",23:"GUID",
}


def read_utf16(pm, addr: int, max_chars: int = 128) -> str:
    out = []
    for i in range(max_chars):
        try:
            ch = struct.unpack("<H", pm.read_bytes(addr + i*2, 2))[0]
        except Exception:
            break
        if ch == 0:
            break
        if ch < 0x20 or ch > 0x7e:
            # non-ascii, show hex
            out.append(f"\\x{ch:04x}")
        else:
            out.append(chr(ch))
    return "".join(out)


def decode_tag(pm, tag: int) -> str:
    try:
        kind = struct.unpack("<I", pm.read_bytes(tag, 4))[0]
        if kind > 23:
            return f"(bad-kind={kind})"
        kname = HL_KIND_NAMES.get(kind, "?")
        if kind != 11:
            return f"kind={kname}"
        # HOBJ: read obj ptr at +8
        obj_ptr = struct.unpack("<Q", pm.read_bytes(tag + 8, 8))[0]
        if obj_ptr == 0:
            return "OBJ but obj=null"
        # hl_type_obj.name at +16 (after nfields/nproto/nbindings + pad)
        nfields = struct.unpack("<i", pm.read_bytes(obj_ptr, 4))[0]
        nproto  = struct.unpack("<i", pm.read_bytes(obj_ptr + 4, 4))[0]
        name_ptr = struct.unpack("<Q", pm.read_bytes(obj_ptr + 16, 8))[0]
        if name_ptr == 0:
            return f"OBJ nf={nfields} np={nproto} (name=null)"
        name = read_utf16(pm, name_ptr)
        return f"OBJ nf={nfields} np={nproto} name='{name}'"
    except Exception as e:
        return f"!! {e}"


def main():
    pm, _ = attach()
    print(f"{'hl_type*':<20}{'hits':>5}  decoded")
    for tag, hits in CANDIDATES:
        s = decode_tag(pm, tag)
        print(f"0x{tag:016x} {hits:>5}  {s}")


if __name__ == "__main__":
    main()

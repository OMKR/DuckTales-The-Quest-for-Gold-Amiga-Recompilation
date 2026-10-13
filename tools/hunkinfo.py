"""List the hunks in an Amiga executable (hunk format)."""
import struct
import sys

HUNK_NAMES = {
    0x3E7: "UNIT", 0x3E8: "NAME", 0x3E9: "CODE", 0x3EA: "DATA", 0x3EB: "BSS",
    0x3EC: "RELOC32", 0x3ED: "RELOC16", 0x3EE: "RELOC8", 0x3EF: "EXT",
    0x3F0: "SYMBOL", 0x3F1: "DEBUG", 0x3F2: "END", 0x3F3: "HEADER",
}


def u32(data, pos):
    return struct.unpack_from(">I", data, pos)[0]


def parse(data):
    pos = 0
    assert u32(data, 0) == 0x3F3, "not a hunk executable"
    pos = 4
    while u32(data, pos):  # resident library names
        pos += 4 + u32(data, pos) * 4
    pos += 4
    table_size, first, last = u32(data, pos), u32(data, pos + 4), u32(data, pos + 8)
    pos += 12
    sizes = [u32(data, pos + 4 * i) & 0x3FFFFFFF for i in range(last - first + 1)]
    pos += 4 * len(sizes)
    print(f"hunks: {table_size}, alloc sizes (longs): {sizes}")

    hunks = []
    while pos < len(data):
        kind = u32(data, pos) & 0x3FFFFFFF
        pos += 4
        name = HUNK_NAMES.get(kind, hex(kind))
        if kind in (0x3E9, 0x3EA):
            n = u32(data, pos) * 4
            hunks.append((name, pos + 4, n))
            print(f"  {name:8} file_off=0x{pos + 4:06x} size={n}")
            pos += 4 + n
        elif kind == 0x3EB:
            print(f"  BSS      size={u32(data, pos) * 4}")
            hunks.append(("BSS", None, u32(data, pos) * 4))
            pos += 4
        elif kind == 0x3EC:
            total = 0
            while (n := u32(data, pos)):
                total += n
                pos += 8 + n * 4
            pos += 4
            print(f"  RELOC32  entries={total}")
        elif kind == 0x3F0:
            while (n := u32(data, pos)):
                pos += 4 + n * 4 + 4
            pos += 4
            print("  SYMBOL")
        elif kind == 0x3F1:
            pos += 4 + u32(data, pos) * 4
            print("  DEBUG")
        elif kind == 0x3F2:
            pass
        else:
            print(f"  unknown hunk {name} at 0x{pos - 4:x}, stopping")
            break
    return hunks


if __name__ == "__main__":
    parse(open(sys.argv[1], "rb").read())

#!/usr/bin/env python3
"""Dump NBT from world files.

    nbtdump.py FILE.dat                  gzip-compressed NBT (level.dat, player data, saved data)
    nbtdump.py FILE.mca [X Z]            region file: list chunks, or dump chunk (X, Z) (region-local 0..31)
    --json                               print as JSON instead of the SNBT-like tree
    --depth N                            limit nesting shown (default: unlimited)
"""
import gzip
import json
import struct
import sys
import zlib


class R:
    def __init__(self, d):
        self.d, self.i = d, 0

    def take(self, n):
        b = self.d[self.i:self.i + n]
        self.i += n
        return b

    def u8(self):
        return self.take(1)[0]

    def fmt(self, f, n):
        return struct.unpack(">" + f, self.take(n))[0]


def mutf8(b):
    # Java modified UTF-8: NUL as C0 80, supplementary chars as surrogate pairs.
    out, i = [], 0
    while i < len(b):
        c = b[i]
        if c < 0x80:
            out.append(c)
            i += 1
        elif c >> 5 == 6:
            out.append((c & 0x1F) << 6 | (b[i + 1] & 0x3F))
            i += 2
        else:
            out.append((c & 0x0F) << 12 | (b[i + 1] & 0x3F) << 6 | (b[i + 2] & 0x3F))
            i += 3
    return bytes(struct.pack(f">{len(out)}H", *out)).decode("utf-16-be", "surrogatepass").encode("utf-16", "surrogatepass").decode("utf-16")


def payload(r, t):
    if t == 1: return ("b", r.fmt("b", 1))
    if t == 2: return ("s", r.fmt("h", 2))
    if t == 3: return ("i", r.fmt("i", 4))
    if t == 4: return ("l", r.fmt("q", 8))
    if t == 5: return ("f", r.fmt("f", 4))
    if t == 6: return ("d", r.fmt("d", 8))
    if t == 7: return ("B[]", list(r.take(r.fmt("i", 4))))
    if t == 8: return ("str", mutf8(r.take(r.fmt("H", 2))))
    if t == 9:
        et, n = r.u8(), r.fmt("i", 4)
        return ("list", [payload(r, et) for _ in range(n)])
    if t == 10:
        out = {}
        while True:
            ct = r.u8()
            if ct == 0:
                return ("compound", out)
            name = mutf8(r.take(r.fmt("H", 2)))
            out[name] = payload(r, ct)
    if t == 11: return ("I[]", [r.fmt("i", 4) for _ in range(r.fmt("i", 4))])
    if t == 12: return ("L[]", [r.fmt("q", 8) for _ in range(r.fmt("i", 4))])
    raise ValueError(f"bad tag {t}")


def read_named(data):
    r = R(data)
    t = r.u8()
    r.take(r.fmt("H", 2))
    return payload(r, t)


def plain(v):
    t, x = v
    if t == "compound": return {k: plain(e) for k, e in x.items()}
    if t == "list": return [plain(e) for e in x]
    return x


def show(v, indent=0, depth=None, key=""):
    t, x = v
    pad = "  " * indent
    if t == "compound":
        print(f"{pad}{key}{{" if x else f"{pad}{key}{{}}")
        if x and (depth is None or indent < depth):
            for k, e in x.items():
                show(e, indent + 1, depth, f"{k}: ")
            print(f"{pad}}}")
        elif x:
            print(f"{pad}  ... {len(x)} entries }}")
    elif t == "list":
        if all(e[0] not in ("compound", "list") for e in x):
            print(f"{pad}{key}[{', '.join(repr(e[1]) for e in x[:16])}{', ...' if len(x) > 16 else ''}] ({len(x)})")
        else:
            print(f"{pad}{key}[ ({len(x)})")
            if depth is None or indent < depth:
                for e in x:
                    show(e, indent + 1, depth)
            print(f"{pad}]")
    elif t in ("B[]", "I[]", "L[]"):
        print(f"{pad}{key}{t}({len(x)}) {x[:6]}{'...' if len(x) > 6 else ''}")
    else:
        print(f"{pad}{key}{x!r}{t if t in 'bsl' else ''}")


def region_chunks(path):
    data = open(path, "rb").read()
    for idx in range(1024):
        off, = struct.unpack(">I", data[idx * 4:idx * 4 + 4])
        sector, count = off >> 8, off & 0xFF
        if not sector:
            continue
        length, ctype = struct.unpack(">IB", data[sector * 4096:sector * 4096 + 5])
        body = data[sector * 4096 + 5:sector * 4096 + 4 + length]
        yield idx % 32, idx // 32, ctype, body, sector, count


def decompress(ctype, body):
    ctype &= 0x7F
    return {1: gzip.decompress, 2: zlib.decompress, 3: lambda b: b}[ctype](body)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    as_json = "--json" in sys.argv
    depth = None
    if "--depth" in sys.argv:
        depth = int(sys.argv[sys.argv.index("--depth") + 1])
        args.remove(str(depth))
    path = args[0]
    if path.endswith(".mca"):
        if len(args) == 1:
            for x, z, ctype, body, sector, count in region_chunks(path):
                print(f"chunk {x},{z}: sector {sector} x{count}, compression {ctype}, {len(body)} bytes")
            return
        want = (int(args[1]), int(args[2]))
        for x, z, ctype, body, _, _ in region_chunks(path):
            if (x, z) == want:
                v = read_named(decompress(ctype, body))
                break
        else:
            sys.exit("chunk not present")
    else:
        raw = open(path, "rb").read()
        v = read_named(gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw)
    if as_json:
        print(json.dumps(plain(v), indent=1))
    else:
        show(v, depth=depth)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""AMFM #534 — read a saved .gox as the authority for cells and modifiers.

GOX v2 is "GOX " + int32 version + chunks(type, len, data, crc).  gox.c writes
a literal 0 for the CRC ("CRC XXX: todo"), so nothing in the file is checksummed
and the caller must hash the file itself.  A BL16 chunk is a 64x64 RGBA PNG
holding one 16^3 volume block; a LAYR chunk lists which block sits at which
cell-space origin and then carries the layer dict, including the #534 surface
modifier keys.

Only the parts the AMFM export needs are decoded.
"""
import struct
import zlib

KIND_NAME = {0: "slope", 1: "bevel", 2: "cylinder", 3: "sphere"}
DIR_NAME = {0: "px", 1: "nx", 2: "py", 3: "ny"}
ANCHOR_NAME = {0: "floor", 1: "mid", 2: "ceil"}


def _i32(b):
    return struct.unpack("<i", b[:4])[0]


def png_rgba(data):
    """Decode an 8-bit non-interlaced PNG (RGBA, RGB, gray or palette)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, info = 8, b"", {}
    while pos < len(data):
        ln = struct.unpack(">i", data[pos:pos + 4])[0]      # PNG lengths are BE
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
            depth, color, _c, _f, inter = struct.unpack("5B", body[8:13])
            info.update(w=w, h=h, depth=depth, color=color, inter=inter)
        elif typ == b"PLTE":
            info["pal"] = body
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln
    w, h, color, inter = info["w"], info["h"], info["color"], info["inter"]
    assert info["depth"] == 8 and inter == 0, (info["depth"], inter)
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = zlib.decompress(idat)
    stride = w * nch
    out, prev = [], bytearray(stride)
    pos = 0
    for _ in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out.append(bytes(line))
        prev = line
    pal = info.get("pal", b"")
    px = []
    for line in out:
        for x in range(w):
            if color == 6:
                px.append(tuple(line[x * 4:x * 4 + 4]))
            elif color == 2:
                px.append(tuple(line[x * 3:x * 3 + 3]) + (255,))
            elif color == 0:
                px.append((line[x],) * 3 + (255,))
            else:
                j = line[x] * 3
                px.append(tuple(pal[j:j + 3]) + (255,))
    return w, h, px


def _layer_dict(body, start):
    out, off = {}, start
    while off + 4 <= len(body):
        klen = _i32(body[off:off + 4])
        off += 4
        if klen == 0:
            break
        key = body[off:off + klen].decode("ascii", "replace")
        off += klen
        vlen = _i32(body[off:off + 4])
        off += 4
        if vlen < 0 or off + vlen > len(body):
            raise ValueError(f"bad value length for {key!r}")
        out[key] = body[off:off + vlen]
        off += vlen
    return out


def load(path):
    """Return the layers of a .gox: name, id, blocks and the surface modifier."""
    data = open(path, "rb").read()
    if data[:4] != b"GOX " or _i32(data[4:8]) != 2:
        raise ValueError(f"{path}: not a GOX v2 file")
    blocks, layers = [], []
    pos = 8
    while pos + 12 <= len(data):
        typ = data[pos:pos + 4].decode("ascii", "replace")
        ln = _i32(data[pos + 4:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if typ == "BL16":
            blocks.append(body)
        elif typ == "LAYR":
            n = _i32(body[0:4])
            bl = [struct.unpack("<5i", body[4 + i * 20:24 + i * 20]) for i in range(n)]
            dd = _layer_dict(body, 4 + 20 * n)
            lay = {
                "name": dd["name"].decode("ascii", "replace"),
                "id": _i32(dd["id"]) if "id" in dd else None,
                "base_id": _i32(dd["base_id"]) if "base_id" in dd else 0,
                "blocks": [(i, x, y, z) for i, x, y, z, _ in bl],
                "slope": None,
            }
            if "slope" in dd:
                sp = struct.unpack("<11i", dd["slope"])
                lay["slope"] = {
                    "region_min_xyz": list(sp[0:3]),
                    "region_max_xyz": list(sp[3:6]),
                    "dir": sp[6],
                    "kind": sp[7],
                    "anchor": sp[8],
                    "closed": sp[9],
                    "applied_hash": sp[10] & 0xFFFFFFFF,
                    "rise": round(struct.unpack("<f", dd["slope-rise"])[0], 6)
                            if "slope-rise" in dd else None,
                    "color": list(struct.unpack("<4B", dd["slope-color"]))
                             if "slope-color" in dd else None,
                }
            layers.append(lay)
        pos += 12 + ln
    for lay in layers:
        pts = {}
        for idx, bx, by, bz in lay["blocks"]:
            w, h, px = png_rgba(blocks[idx])
            if (w, h) != (64, 64):
                raise ValueError(f"{lay['name']}: block {idx} is {w}x{h}, expected 64x64")
            for p, rgba in enumerate(px):
                if rgba[3] == 0:
                    continue
                # The block origin in a LAYR is already in cell coordinates.
                pts[(bx + p % 16, by + (p // 16) % 16, bz + p // 256)] = rgba
        lay["cells"] = pts
    return layers


def cells(layers):
    """Every occupied cell in the file: position -> (rgba, layer name)."""
    out = {}
    for lay in layers:
        for p, rgba in lay["cells"].items():
            if p in out:
                raise ValueError(f"{p} occupied by two layers: {out[p][1]} and {lay['name']}")
            out[p] = (rgba, lay["name"])
    return out

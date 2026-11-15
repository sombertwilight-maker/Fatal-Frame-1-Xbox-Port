"""Decode and re-encode the DXT textures of an XPR0 pack in place."""
import struct

def parse(u8):
    assert u8[:4] == b'XPR0'
    hsize = struct.unpack_from('<I', u8, 8)[0]
    p, texes = 12, []
    while p + 12 <= hsize:
        common, = struct.unpack_from('<I', u8, p)
        if common == 0xFFFFFFFF:
            break
        t = (common >> 16) & 7
        if t == 4 and p + 20 <= hsize:
            dataOff, = struct.unpack_from('<I', u8, p + 4)
            f, size = struct.unpack_from('<II', u8, p + 12)
            code = (f >> 8) & 0xFF
            w = 1 << ((f >> 20) & 15); h = 1 << ((f >> 24) & 15)
            if size:
                w = (size & 0xFFF) + 1; h = ((size >> 12) & 0xFFF) + 1
            texes.append(dict(i=len(texes), w=w, h=h, code=code, off=hsize + dataOff))
            p += 20
        else:
            p += 12
    return texes

def _c565(c):
    return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)

def decode_dxt(u8, off, w, h, kind):  # kind 1=DXT1 3=DXT3 5=DXT5
    out = bytearray(w * h * 4)
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    p = off
    for by in range(bh):
        for bx in range(bw):
            ap = -1
            if kind != 1:
                ap = p; p += 8
            c0, c1 = struct.unpack_from('<HH', u8, p)
            pal = [_c565(c0) + (255,), _c565(c1) + (255,)]
            if kind != 1 or c0 > c1:
                pal.append(tuple((2 * a + b + 1) // 3 for a, b in zip(pal[0][:3], pal[1][:3])) + (255,))
                pal.append(tuple((a + 2 * b + 1) // 3 for a, b in zip(pal[0][:3], pal[1][:3])) + (255,))
            else:
                pal.append(tuple((a + b) // 2 for a, b in zip(pal[0][:3], pal[1][:3])) + (255,))
                pal.append((0, 0, 0, 0))
            bits, = struct.unpack_from('<I', u8, p + 4)
            p += 8
            al = [0] * 8
            if kind == 5:
                a0, a1 = u8[ap], u8[ap + 1]
                al[0], al[1] = a0, a1
                if a0 > a1:
                    for k in range(1, 7):
                        al[1 + k] = ((7 - k) * a0 + k * a1 + 3) // 7
                else:
                    for k in range(1, 5):
                        al[1 + k] = ((5 - k) * a0 + k * a1 + 2) // 5
                    al[6], al[7] = 0, 255
            for j in range(4):
                for i in range(4):
                    x, y = bx * 4 + i, by * 4 + j
                    if x >= w or y >= h:
                        continue
                    t = j * 4 + i
                    ci = (bits >> (t * 2)) & 3
                    r, g, b, a = pal[ci]
                    if kind == 3:
                        a = ((u8[ap + (t >> 1)] >> ((t & 1) * 4)) & 15) * 17
                    elif kind == 5:
                        bit = t * 3
                        byte = ap + 2 + (bit >> 3)
                        a = al[((u8[byte] | (u8[byte + 1] << 8 if byte + 1 < len(u8) else 0)) >> (bit & 7)) & 7]
                    o = (y * w + x) * 4
                    out[o:o + 4] = bytes((r, g, b, a))
    return bytes(out)

def _to565(r, g, b):
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)

def encode_dxt(rgba, w, h, kind, out, off):
    """Encode rgba into DXT3/5 at out[off:], same layout the game uses."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    p = off
    for by in range(bh):
        for bx in range(bw):
            px = []
            for j in range(4):
                for i in range(4):
                    x, y = min(bx * 4 + i, w - 1), min(by * 4 + j, h - 1)
                    o = (y * w + x) * 4
                    px.append(tuple(rgba[o:o + 4]))
            # alpha block
            if kind == 3:
                for k in range(8):
                    a0 = (px[k * 2][3] * 15 + 127) // 255
                    a1 = (px[k * 2 + 1][3] * 15 + 127) // 255
                    out[p + k] = a0 | (a1 << 4)
                p += 8
            elif kind == 5:
                amin = min(q[3] for q in px); amax = max(q[3] for q in px)
                if amin == amax:
                    amax = min(255, amax + 1) if amax < 255 else amax
                    amin = max(0, amin - 1) if amax == amin else amin
                out[p], out[p + 1] = amax, amin  # a0 > a1: 8-level ramp
                ramp = [amax, amin] + [((7 - k) * amax + k * amin + 3) // 7 for k in range(1, 7)]
                bits = 0
                for t, q in enumerate(px):
                    ci = min(range(8), key=lambda k: abs(ramp[k] - q[3]))
                    bits |= ci << (t * 3)
                for k in range(6):
                    out[p + 2 + k] = (bits >> (k * 8)) & 0xFF
                p += 8
            # colour block: min/max luminance endpoints
            lum = [q[0] * 3 + q[1] * 6 + q[2] for q in px]
            hi = px[lum.index(max(lum))][:3]
            lo = px[lum.index(min(lum))][:3]
            c0, c1 = _to565(*hi), _to565(*lo)
            if c0 == c1:
                pal = [hi, hi, hi, hi]
                if c0 == 0:
                    c0 = 1
                struct.pack_into('<HH', out, p, max(c0, c1), min(c0, c1))
                struct.pack_into('<I', out, p + 4, 0)
                p += 8
                continue
            if c0 < c1:
                c0, c1 = c1, c0
                hi, lo = lo, hi
            pal = [hi, lo,
                   tuple((2 * a + b + 1) // 3 for a, b in zip(hi, lo)),
                   tuple((a + 2 * b + 1) // 3 for a, b in zip(hi, lo))]
            struct.pack_into('<HH', out, p, c0, c1)
            bits = 0
            for t, q in enumerate(px):
                ci = min(range(4), key=lambda k: sum((pal[k][m] - q[m]) ** 2 for m in range(3)))
                bits |= ci << (t * 2)
            struct.pack_into('<I', out, p + 4, bits)
            p += 8
    return p - off

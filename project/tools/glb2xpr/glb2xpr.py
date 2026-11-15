#!/usr/bin/env python3
"""glb2xpr -- pack a Blender GLB's textures into a Fatal Frame .xpr.

Usage:  python glb2xpr.py donor.xpr yourmodel.glb out.xpr

Paint on the mesh exported by the model viewer; keep the image names it
gave them (m000_<model>_NN_...). Each named image lands in slot NN of the
donor pack; unnamed images and missing slots keep the donor's texture.
Needs: pip install pillow
"""
import struct, json, re, io, sys
from xprtool import parse, encode_dxt
from PIL import Image

def main(donor, glb, out):
    raw = open(glb, 'rb').read()
    assert raw[:4] == b'glTF', 'not a .glb'
    p, jso, bin_ = 12, None, None
    while p < len(raw):
        clen, ctype = struct.unpack_from('<II', raw, p); p += 8
        if ctype == 0x4E4F534A: jso = json.loads(raw[p:p+clen])
        elif ctype == 0x004E4942: bin_ = raw[p:p+clen]
        p += clen
    bvs = jso['bufferViews']
    slot_img = {}
    for im in jso.get('images', []):
        name = im.get('name') or ''
        slots = re.findall(r'_(\d\d)_', name)
        if not slots or 'bufferView' not in im: continue
        bv = bvs[im['bufferView']]
        data = bin_[bv.get('byteOffset', 0): bv.get('byteOffset', 0) + bv['byteLength']]
        slot_img[int(slots[-1])] = (name, Image.open(io.BytesIO(data)).convert('RGBA'))
    u8 = bytearray(open(donor, 'rb').read())
    used = []
    texes = parse(bytes(u8))
    ends = [texes[i + 1]['off'] for i in range(len(texes) - 1)] + [len(u8)]
    for t, end in zip(texes, ends):
        kind = {0x0c: 1, 0x0e: 3, 0x0f: 5}.get(t['code'])
        if kind is None or t['i'] not in slot_img: continue
        name, src = slot_img[t['i']]
        bpb = 8 if kind == 1 else 16
        w, h, off = t['w'], t['h'], t['off']
        while w >= 1 and h >= 1:   # full mip chain: the game samples these
            size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bpb
            if off + size > end: break
            im = src.resize((w, h), Image.LANCZOS) if src.size != (w, h) else src
            encode_dxt(im.tobytes(), w, h, kind, u8, off)
            off += size
            if w == 1 and h == 1: break
            w = max(1, w // 2); h = max(1, h // 2)
        used.append(t['i'])
    open(out, 'wb').write(bytes(u8))
    print('replaced slots:', used)
    print('wrote', out)

if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__); sys.exit(1)
    main(*sys.argv[1:4])

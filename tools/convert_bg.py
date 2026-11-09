#!/usr/bin/env python3
"""
Convierte imágenes (PNG/JPG) al formato .565 que lee el companion desde la SD.

  pip install pillow
  python tools/convert_bg.py src/assets sd/bg

- Escala y recorta al centro a 480x320 si no lo son ya.
- Salida: RGB565 crudo little-endian, 307200 bytes por imagen, sin cabecera.
- Copia el contenido de sd/ a la raíz de la microSD (FAT32).

Nombre de salida = nombre del fichero sin extensión ni sufijo _480x320.
Usa --map viejo=nuevo para renombrar, p.ej. --map background=home
"""
import argparse
import os
import struct

from PIL import Image

W, H = 480, 320


def to_565(img):
    img = img.convert("RGB")
    if img.size != (W, H):
        src_ratio = img.width / img.height
        if src_ratio > W / H:
            nw = int(img.height * W / H)
            img = img.crop(((img.width - nw) // 2, 0, (img.width - nw) // 2 + nw, img.height))
        else:
            nh = int(img.width * H / W)
            img = img.crop((0, (img.height - nh) // 2, img.width, (img.height - nh) // 2 + nh))
        img = img.resize((W, H), Image.LANCZOS)
    out = bytearray()
    raw = img.tobytes()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="carpeta con PNG/JPG")
    ap.add_argument("dst", help="carpeta de salida, p.ej. sd/bg")
    ap.add_argument("--map", action="append", default=[], help="renombrar: origen=destino")
    a = ap.parse_args()

    ren = dict(m.split("=", 1) for m in a.map)
    os.makedirs(a.dst, exist_ok=True)
    done = set()
    # PNG primero: si hay PNG y JPG con el mismo nombre, gana el PNG (sin pérdida)
    files = sorted(os.listdir(a.src), key=lambda f: (not f.lower().endswith(".png"), f))
    for f in files:
        base, ext = os.path.splitext(f)
        if ext.lower() not in (".png", ".jpg", ".jpeg"):
            continue
        name = base.replace("_480x320", "")
        name = ren.get(name, name)
        if name in done:
            continue
        data = to_565(Image.open(os.path.join(a.src, f)))
        with open(os.path.join(a.dst, name + ".565"), "wb") as o:
            o.write(data)
        done.add(name)
        print(f"{f} -> {name}.565")


if __name__ == "__main__":
    main()

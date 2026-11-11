#!/usr/bin/env python3
"""Ordena la microSD del companion en la estructura de /companion.

Uso (con la tarjeta montada en el Mac):

    python3 tools/sd_migrate.py /Volumes/<TARJETA> --dry-run   # solo dice lo que haría
    python3 tools/sd_migrate.py /Volumes/<TARJETA>

Estructura final:

    /companion/fondos, /companion/fondos-vertical      tus fotos (las de Wallpapers > My pictures)
    /companion/diarias, /companion/diarias-vertical    fotos diarias de Spotlight
    /companion/iconos                                  iconos de marca
    /companion/sonidos                                 sonidos
    /companion/antiguo                                 lo del firmware anterior, tal cual

Qué hace:
  1. Mueve lo de TouchDeck (/touchdeck/wall/l, p, dl, dp, /touchdeck/icons,
     /touchdeck/sound) a su sitio nuevo. El firmware también lo hace solo al
     montar la tarjeta; aquí sirve si la migras antes de meterla.
  2. Convierte los fondos .565 del firmware anterior (/bg, /fotos) y las fotos
     sueltas de /fotos a JPEG 480x320 en /companion/fondos, con el mismo
     formato y nombre que usa el portal (8 hex = crc32 del JPEG).
  3. Mueve /bg, /icons y /fotos a /companion/antiguo/. No borra nada.

Necesita Pillow (pip install pillow).
"""
import argparse
import io
import pathlib
import shutil
import sys
import zlib

from PIL import Image, ImageOps

ROOT = "companion"
TOUCHDECK_MOVES = [
    ("touchdeck/wall/l", "fondos"),
    ("touchdeck/wall/p", "fondos-vertical"),
    ("touchdeck/wall/dl", "diarias"),
    ("touchdeck/wall/dp", "diarias-vertical"),
    ("touchdeck/icons", "iconos"),
    ("touchdeck/sound", "sonidos"),
]
OLD_DIRS = ["bg", "icons", "fotos"]
W, H = 480, 320
RAW_SIZE = W * H * 2
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}


def jpeg(img):
    """Same encoding as tools/wall_tool.py: centre crop, baseline JPEG, quality 85."""
    img = ImageOps.fit(img.convert("RGB"), (W, H), Image.LANCZOS)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=85, optimize=True, progressive=False)
    return out.getvalue()


def from_565(data):
    """Raw RGB565 little-endian, 480x320 row by row (the old firmware's .565)."""
    px = bytearray(W * H * 3)
    for i in range(W * H):
        v = data[2 * i] | data[2 * i + 1] << 8
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        px[3 * i] = (r << 3) | (r >> 2)
        px[3 * i + 1] = (g << 2) | (g >> 4)
        px[3 * i + 2] = (b << 3) | (b >> 2)
    return Image.frombytes("RGB", (W, H), bytes(px))


def pictures(folder):
    if not folder.is_dir():
        return []
    return sorted(p for p in folder.rglob("*") if p.is_file() and not p.name.startswith("._"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("card", help="punto de montaje de la tarjeta, p. ej. /Volumes/SD")
    ap.add_argument("--dry-run", action="store_true", help="solo enseña lo que haría")
    a = ap.parse_args()
    card = pathlib.Path(a.card)
    if not card.is_dir():
        sys.exit("No encuentro %s" % card)
    dry = a.dry_run
    root = card / ROOT

    def mkdir(p):
        if not p.exists():
            print("crear     %s" % p.relative_to(card))
            if not dry:
                p.mkdir(parents=True)

    def move(src, dst):
        print("mover     %s -> %s" % (src.relative_to(card), dst.relative_to(card)))
        if not dry:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(src), str(dst))

    mkdir(root)
    for sub in ("fondos", "fondos-vertical", "diarias", "diarias-vertical"):
        mkdir(root / sub)

    # 1. TouchDeck's layout
    for src, dst in TOUCHDECK_MOVES:
        s, d = card / src, root / dst
        if not s.is_dir():
            continue
        if d.exists() and any(d.iterdir()):
            # Both have pictures: move the files over, keep whatever is already there
            for f in pictures(s):
                if not (d / f.name).exists():
                    move(f, d / f.name)
        else:
            if d.exists() and not dry:
                d.rmdir()
            move(s, d)

    # 2. The old firmware's pictures into /companion/fondos
    fondos = root / "fondos"
    converted = skipped = 0
    for old in ("bg", "fotos"):
        for f in pictures(card / old):
            ext = f.suffix.lower()
            try:
                if ext == ".565":
                    data = f.read_bytes()
                    if len(data) != RAW_SIZE:
                        print("saltar    %s (no mide %d bytes)" % (f.relative_to(card), RAW_SIZE))
                        skipped += 1
                        continue
                    out = jpeg(from_565(data))
                elif ext in IMAGE_EXTS:
                    out = jpeg(ImageOps.exif_transpose(Image.open(f)))
                else:
                    continue
            except Exception as e:  # a broken picture must not stop the rest
                print("saltar    %s (%s)" % (f.relative_to(card), e))
                skipped += 1
                continue
            dst = fondos / ("%08x.jpg" % zlib.crc32(out))
            if dst.exists():
                continue
            print("convertir %s -> %s" % (f.relative_to(card), dst.relative_to(card)))
            converted += 1
            if not dry:
                dst.write_bytes(out)

    # 3. The old folders, untouched, into /companion/antiguo
    for old in OLD_DIRS:
        s = card / old
        if s.is_dir():
            move(s, root / "antiguo" / old)

    # /touchdeck left empty by step 1
    td = card / "touchdeck"
    if td.is_dir() and not dry:
        for d in sorted((p for p in td.rglob("*") if p.is_dir()), reverse=True):
            if not any(d.iterdir()):
                d.rmdir()
        if not any(td.iterdir()):
            td.rmdir()
            print("quitar    touchdeck/ (vacía)")

    print("\n%s: %d fotos convertidas, %d saltadas." % ("Simulación" if dry else "Hecho", converted, skipped))


if __name__ == "__main__":
    main()

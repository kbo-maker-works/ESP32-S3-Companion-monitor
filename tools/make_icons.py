#!/usr/bin/env python3
"""
Iconos del companion: SVG/PNG monocromos -> máscara alpha de 4 bits (se tiñen en la placa).

  python tools/make_icons.py                 # genera src/assets/icons_builtin.h y build_icons/*.a4
  python tools/make_icons.py --upload 192.168.1.225   # además sube los .a4 a /icons en la SD

Entrada: src/assets/icons/<nombre>.svg | .png
  - icon_<id>   módulos de la home, 48x48 (id de 3 letras: env, ai, sys...)
  - sb_<nombre> barra superior, 20x20 (sb_wifi_0..3, sb_wifi_off, sb_usb, sb_bat_0..4...)
  - Lienzo 48x48 (o múltiplo: 96, 192... se reduce), blanco sobre transparente.
  - Si hay SVG y está instalado cairosvg o resvg_py, se usa el SVG; si no, el PNG.
  - PNG sin transparencia: se usa la luminancia (blanco = icono, negro = fondo).

Salida:
  - src/assets/icons_builtin.h   iconos compilados (fallback si no hay SD)
  - build_icons/<nombre>.a4      para /icons/ en la microSD (mandan sobre los compilados)

Formato .a4: 'A' '4' ancho alto, y luego ancho*alto/2 bytes, 2 píxeles por byte,
nibble alto primero (el mismo empaquetado que las fuentes de TextAA).
"""
import argparse
import io
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "assets", "icons")
HDR = os.path.join(ROOT, "src", "assets", "icons_builtin.h")
OUT = os.path.join(ROOT, "build_icons")
SIZES = {"icon_": 48, "sb_": 20}   # prefijo -> lado en px


def svg_to_png(path, size):
    try:
        import cairosvg
        return Image.open(io.BytesIO(cairosvg.svg2png(url=path, output_width=size, output_height=size)))
    except ImportError:
        pass
    try:
        import resvg_py
        return Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(svg_path=path, width=size, height=size))))
    except ImportError:
        return None


def load(base, size):
    svg, png = base + ".svg", base + ".png"
    im = svg_to_png(svg, size) if os.path.exists(svg) else None
    src = svg
    if im is None:
        if not os.path.exists(png):
            return None, None
        im, src = Image.open(png), png
    im = im.convert("RGBA")
    alpha = im.split()[3]
    if alpha.getextrema()[0] == 255:   # opaco: blanco sobre negro
        alpha = im.convert("L")
    else:                              # alpha x luminancia: lo oscuro no cuenta como icono
        from PIL import ImageChops
        alpha = ImageChops.multiply(alpha, im.convert("L"))
    if alpha.size != (size, size):
        alpha = alpha.resize((size, size), Image.LANCZOS)
    return alpha, src


def pack(alpha):
    w, h = alpha.size
    px = [min(15, (v * 15 + 127) // 255) for v in alpha.getdata()]
    if len(px) % 2:
        px.append(0)
    return bytes((px[i] << 4) | px[i + 1] for i in range(0, len(px), 2)), w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upload", metavar="IP", help="sube los .a4 a /icons de la SD del companion")
    args = ap.parse_args()

    ids = sorted({os.path.splitext(f)[0] for f in os.listdir(SRC)
                  if f.startswith(tuple(SIZES)) and f.lower().endswith((".svg", ".png"))})
    if not ids:
        sys.exit("No hay iconos en " + SRC)
    os.makedirs(OUT, exist_ok=True)

    lines = ["#pragma once", "// Generado por tools/make_icons.py — no editar a mano", "#include <stdint.h>", "",
             "struct BuiltinIcon { const char *id; uint8_t w, h; const uint8_t *data; };", ""]
    table = []
    for i in ids:
        size = next(s for p, s in SIZES.items() if i.startswith(p))
        alpha, src = load(os.path.join(SRC, i), size)
        if alpha is None:
            continue
        data, w, h = pack(alpha)
        with open(os.path.join(OUT, i + ".a4"), "wb") as f:
            f.write(bytes([ord("A"), ord("4"), w, h]) + data)
        name = "ICO_" + i.upper()
        lines.append("static const uint8_t %s[%d] = {" % (name, len(data)))
        for k in range(0, len(data), 24):
            lines.append("    " + ",".join("0x%02X" % b for b in data[k:k + 24]) + ",")
        lines.append("};")
        table.append('    {"%s", %d, %d, %s},' % (i, w, h, name))
        print("  %-12s %dx%d  <- %s" % (i, w, h, os.path.relpath(src, ROOT)))
    lines += ["", "static const BuiltinIcon BUILTIN_ICONS[] = {"] + table + ["};",
              "static const int BUILTIN_ICON_COUNT = %d;" % len(table), ""]
    with open(HDR, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("-> %s (%d iconos)\n-> %s/*.a4" % (os.path.relpath(HDR, ROOT), len(table), os.path.relpath(OUT, ROOT)))

    if args.upload:
        subprocess.run(["curl", "-s", "-X", "POST", "http://%s/api/sd/mkdir?path=/icons" % args.upload,
                        "-F", "path=/icons"], check=False)
        for i in ids:
            p = os.path.join(OUT, i + ".a4")
            if os.path.exists(p):
                r = subprocess.run(["curl", "-s", "-F", "file=@%s;filename=%s.a4" % (p, i),
                                    "http://%s/api/sd/upload?path=/icons" % args.upload],
                                   capture_output=True, text=True)
                print("  subido %s.a4: %s" % (i, r.stdout.strip()))


if __name__ == "__main__":
    main()

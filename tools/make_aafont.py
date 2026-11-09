#!/usr/bin/env python3
"""
Genera fuentes con antialiasing (4 bits de alpha por pixel) para el companion
a partir de un TTF. Salida: cabecera C++ en src/fonts/ que usa src/core/TextAA.h.

  pip install pillow
  python tools/make_aafont.py assets/fonts/Nunito[wght].ttf

Cada entrada de FONTS es: (nombre, tamaño px, peso, conjunto de caracteres)
El peso solo aplica a fuentes variables (Nunito: 200-1000).
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

LATIN1 = "".join(chr(c) for c in range(0x20, 0x7F)) + "".join(chr(c) for c in range(0xA0, 0x100))
DIGITS = "0123456789:.,-+ %°C"

FONTS = [
    # nombre           px   peso  caracteres
    ("Nunito14",       14,  600,  LATIN1),
    ("Nunito18",       18,  700,  LATIN1),
    ("Nunito24",       24,  800,  LATIN1),
    ("Nunito36",       36,  800,  LATIN1),
    ("Nunito72Num",    72,  800,  DIGITS),   # reloj / temperatura grandes
]

FIRST, LAST = 0x20, 0xFF


def load(ttf, px, weight):
    f = ImageFont.truetype(ttf, px)
    try:
        f.set_variation_by_axes([weight])
    except Exception:
        pass  # TTF estático: ignora el peso
    return f


def build(ttf, name, px, weight, chars):
    font = load(ttf, px, weight)
    ascent, descent = font.getmetrics()
    bitmap = bytearray()
    glyphs = []
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        if ch not in chars:
            glyphs.append((0, 0, 0, 0, 0, 0))
            continue
        adv = int(round(font.getlength(ch)))
        l, t, r, b = font.getbbox(ch, anchor="ls")
        w, h = max(0, r - l), max(0, b - t)
        off = len(bitmap)
        if w and h:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
            px4 = [(v + 8) // 17 for v in img.tobytes()]       # 0..255 -> 0..15
            if len(px4) % 2:
                px4.append(0)
            for i in range(0, len(px4), 2):
                bitmap.append((px4[i] << 4) | px4[i + 1])
        assert w < 256 and h < 256 and -128 <= l < 128 and -128 <= t < 128 and adv < 256, (name, ch)
        glyphs.append((off, w, h, adv, l, t))

    out = [f"// Generado por tools/make_aafont.py — {os.path.basename(ttf)} {px}px peso {weight}",
           "#pragma once", '#include "../core/TextAA.h"', "",
           f"static const uint8_t {name}_bitmap[] PROGMEM = {{"]
    for i in range(0, len(bitmap), 24):
        out.append("  " + ",".join(f"0x{v:02X}" for v in bitmap[i:i + 24]) + ",")
    out.append("};")
    out.append(f"static const AAGlyph {name}_glyphs[] PROGMEM = {{")
    for code, g in zip(range(FIRST, LAST + 1), glyphs):
        c = chr(code) if 0x20 < code < 0x7F and chr(code) not in "\\'" else ""
        out.append(f"  {{{g[0]},{g[1]},{g[2]},{g[3]},{g[4]},{g[5]}}}, // 0x{code:02X} {c}")
    out.append("};")
    out.append(f"static const AAFont {name} = {{ {name}_bitmap, {name}_glyphs, 0x{FIRST:02X}, 0x{LAST:02X}, "
               f"{ascent + descent}, {ascent} }};")
    return "\n".join(out) + "\n", len(bitmap)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ttf = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "src", "fonts")
    os.makedirs(outdir, exist_ok=True)
    includes = []
    for name, px, weight, chars in FONTS:
        src, size = build(ttf, name, px, weight, chars)
        with open(os.path.join(outdir, f"{name}.h"), "w", encoding="utf-8") as f:
            f.write(src)
        includes.append(f'#include "{name}.h"')
        print(f"{name}: {size / 1024:.1f} KB")
    with open(os.path.join(outdir, "Fonts.h"), "w", encoding="utf-8") as f:
        f.write("#pragma once\n// Todas las fuentes AA del proyecto\n" + "\n".join(includes) + "\n")


if __name__ == "__main__":
    main()

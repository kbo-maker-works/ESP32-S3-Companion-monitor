"""
CompaDeck agent: a sheet for the workbench on the deck's Bench app.

The soldering order, which wire goes where, a pinout: send it to the deck and
read it there instead of on the phone or the PC.

  python td_bench.py steps.txt           # text: one step per line
  python td_bench.py pinout.png          # a picture (resized and sent as JPEG)
  python td_bench.py --clip              # whatever is copied: a picture or text
  python td_bench.py --text "Red -> 5V"  # text from the command line
  some_command | python td_bench.py -    # text from stdin
  python td_bench.py --sendto            # Windows: add "CompaDeck Bench" to Send to

Text: one step per line, "# Title" lines are section headings, list markers
("- ", "1. ", "[ ] ") are dropped, colour words (red, rojo...) get a swatch on
the deck and "->" becomes an arrow. A Markdown table becomes one step per row
("red -> 5V"), its header row a heading.

On the deck: tap the right half for the next step, the left half to go back.
A picture: tap to zoom at that point, drag to pan, tap again to fit.

The deck only takes a sheet from a PC whose agent (touchdeck_agent.py) is
running: same PC id, same address.

  POST http://<deck>/api/pc/bench        {"pc_id", "title", "text"}
  POST http://<deck>/api/pc/bench/image?pc_id=&title=   multipart "image" (JPEG)
"""
import argparse
import hashlib
import io
import json
import math
import os
import platform
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid

# The deck's limits (app_bench.h).
MAX_TEXT = 8192
MAX_TITLE = 47
MAX_JPEG = 400 * 1024
MAX_SIDE = 1280
MAX_PIXELS = 800 * 600
TIMEOUT_S = 15
IMAGE_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tif", ".tiff"}

# What the deck's fonts lack, as something they have (they cover Latin-1 and
# Latin Extended-A, plus the bullet and the euro sign).
REPLACE = {
    "–": "-", "—": "-", "−": "-", "‘": "'", "’": "'", "“": '"', "”": '"',
    "…": "...", "≈": "~", "≤": "<=", "≥": ">=", "⇒": "=>", "←": "<-",
    "➡": "->", "➔": "->", "➜": "->", "Ω": "ohm", "Ω": "ohm", "✓": "", "✔": "",
}
KEEP_EXTRA = {"•", "€", "→"}  # bullet, euro, the arrow (the deck draws it)


def pc_identity():
    """Same id as touchdeck_agent.py: the deck knows this PC by it."""
    host = platform.node() or "PC"
    return hashlib.sha1(host.encode()).hexdigest()[:12]


def clip(text, max_bytes):
    return (text or "").encode("utf-8")[:max_bytes].decode("utf-8", "ignore")


def deck_text(text):
    out = []
    for ch in text:
        ch = REPLACE.get(ch, ch)
        out.append("".join(c for c in ch if c in "\n\t" or 0x20 <= ord(c) < 0x7f or 0xa0 <= ord(c) <= 0x17f
                           or c in KEEP_EXTRA))
    return "".join(out)


def table_cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def clean_text(text):
    """Markdown into the deck's format: tables become rows of "a -> b", emphasis
    and code marks go."""
    lines, in_table = [], False
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw.strip()
        if line.startswith("|") and line.endswith("|") and len(line) > 1:
            cells = table_cells(line)
            if all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c):
                continue  # |---|---|
            row = " -> ".join(c for c in cells if c)
            lines.append(("# " + row) if not in_table else row)
            in_table = True
            continue
        in_table = False
        if line.startswith("```"):
            continue
        line = re.sub(r"(\*\*|__|`)", "", line)
        lines.append(line)
    text = deck_text("\n".join(lines)).strip()
    while len(text.encode("utf-8")) > MAX_TEXT:  # whole lines off the end
        text = text.rsplit("\n", 1)[0] if "\n" in text else clip(text, MAX_TEXT)
    return text


def jpeg_for_deck(img):
    """A Pillow image as a baseline JPEG within the deck's limits."""
    from PIL import Image, ImageOps
    img = ImageOps.exif_transpose(img)
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        flat = Image.new("RGB", img.size, (255, 255, 255))  # a transparent pinout reads on white
        flat.paste(img, mask=img.split()[-1])
        img = flat
    img = img.convert("RGB")
    w, h = img.size
    k = min(1.0, math.sqrt(MAX_PIXELS / float(w * h)), MAX_SIDE / float(max(w, h)))
    if k < 1.0:
        img = img.resize((max(1, int(w * k)), max(1, int(h * k))), Image.LANCZOS)
    for quality in (88, 80, 70, 60, 50):
        buf = io.BytesIO()
        # 4:4:4 keeps thin coloured lines and small text sharp; baseline: the deck's decoder has no progressive.
        img.save(buf, "JPEG", quality=quality, subsampling=0, progressive=False, optimize=True)
        if buf.tell() <= MAX_JPEG:
            return buf.getvalue(), img.size
    raise SystemExit("the picture does not fit in 400 KB")


def load_image(path):
    try:
        from PIL import Image
    except ImportError:
        if path.lower().endswith((".jpg", ".jpeg")):
            with open(path, "rb") as f:
                return f.read(), None  # as it is: the deck checks it
        raise SystemExit("pictures need Pillow: pip install pillow")
    with Image.open(path) as img:
        return jpeg_for_deck(img)


def clipboard():
    """The clipboard's picture (bytes, size) or text (str), None when empty."""
    try:
        from PIL import ImageGrab
        got = ImageGrab.grabclipboard()
    except (ImportError, OSError, NotImplementedError):
        got = None
    if got is not None and not isinstance(got, list):
        return jpeg_for_deck(got)
    if isinstance(got, list) and got:  # files copied in Explorer / Finder
        path = got[0]
        if os.path.splitext(path)[1].lower() in IMAGE_EXT:
            return load_image(path)
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    return clipboard_text()


def clipboard_text():
    if sys.platform == "darwin":
        import subprocess
        return subprocess.run(["pbpaste"], capture_output=True, text=True).stdout or None
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes
    user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
    user32.GetClipboardData.restype = wintypes.HANDLE
    kernel32.GlobalLock.argtypes = [wintypes.HGLOBAL]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
    if not user32.OpenClipboard(None):
        return None
    try:
        handle = user32.GetClipboardData(13)  # CF_UNICODETEXT
        if not handle:
            return None
        ptr = kernel32.GlobalLock(handle)
        try:
            return ctypes.wstring_at(ptr) if ptr else None
        finally:
            kernel32.GlobalUnlock(handle)
    finally:
        user32.CloseClipboard()


def deck_reply(e):
    try:
        return json.loads(e.read().decode("utf-8")).get("message") or str(e)
    except (ValueError, OSError, AttributeError):
        return str(e)


def send(req):
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
            return True, json.loads(r.read().decode("utf-8")).get("message", "OK")
    except urllib.error.HTTPError as e:
        return False, deck_reply(e)
    except (urllib.error.URLError, OSError) as e:
        return False, "cannot reach the deck: %s" % getattr(e, "reason", e)


def send_text(host, title, text):
    body = json.dumps({"pc_id": pc_identity(), "title": clip(title, MAX_TITLE), "text": text}).encode("utf-8")
    req = urllib.request.Request("http://%s/api/pc/bench" % host, data=body,
                                 headers={"Content-Type": "application/json"}, method="POST")
    return send(req)


def send_image(host, title, jpeg):
    boundary = uuid.uuid4().hex
    body = (("--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"sheet.jpg\"\r\n"
             "Content-Type: image/jpeg\r\n\r\n" % boundary).encode() + jpeg + ("\r\n--%s--\r\n" % boundary).encode())
    query = urllib.parse.urlencode({"pc_id": pc_identity(), "title": clip(title, MAX_TITLE)})
    req = urllib.request.Request("http://%s/api/pc/bench/image?%s" % (host, query), data=body,
                                 headers={"Content-Type": "multipart/form-data; boundary=%s" % boundary},
                                 method="POST")
    return send(req)


def install_sendto():
    """Windows: a "CompaDeck Bench" entry in Explorer's Send to menu."""
    if sys.platform != "win32":
        raise SystemExit("--sendto is for Windows")
    folder = os.path.join(os.environ["APPDATA"], "Microsoft", "Windows", "SendTo")
    path = os.path.join(folder, "CompaDeck Bench.cmd")
    with open(path, "w", encoding="utf-8") as f:
        # The window stays open only when something failed.
        f.write('@"%s" "%s" %%* || pause\r\n' % (sys.executable, os.path.abspath(__file__)))
    print("added: right click a file > Send to > CompaDeck Bench (%s)" % path)


def main():
    ap = argparse.ArgumentParser(description="Send a sheet to the CompaDeck's Bench app")
    ap.add_argument("file", nargs="?", help="text or picture file, - for stdin")
    ap.add_argument("--clip", action="store_true", help="send the clipboard (picture or text)")
    ap.add_argument("--text", help="send this text")
    ap.add_argument("--title", help="title on the deck (default: the file name)")
    ap.add_argument("--host", default=os.environ.get("TOUCHDECK_HOST", "companion.local"),
                    help="deck name or IP (default companion.local, env TOUCHDECK_HOST)")
    ap.add_argument("--sendto", action="store_true", help="Windows: add CompaDeck Bench to Send to")
    args = ap.parse_args()

    if args.sendto:
        install_sendto()
        return
    sources = [bool(args.file), args.clip, args.text is not None]
    if sum(sources) != 1:
        ap.error("give one of: a file, -, --clip or --text")

    title, payload = args.title, None
    if args.text is not None:
        payload = args.text.replace("\\n", "\n")
    elif args.clip:
        payload = clipboard()
        if not payload:
            raise SystemExit("the clipboard is empty")
    elif args.file == "-":
        sys.stdin.reconfigure(encoding="utf-8", errors="replace")
        payload = sys.stdin.read()
    else:
        title = title or os.path.splitext(os.path.basename(args.file))[0]
        if os.path.splitext(args.file)[1].lower() in IMAGE_EXT:
            payload = load_image(args.file)
        else:
            with open(args.file, encoding="utf-8", errors="replace") as f:
                payload = f.read()

    if isinstance(payload, tuple):
        jpeg, size = payload
        ok, msg = send_image(args.host, title or ("Clipboard" if args.clip else ""), jpeg)
        what = "picture %s, %d KB" % ("%dx%d" % size if size else "as is", len(jpeg) // 1024)
    else:
        text = clean_text(payload)
        if not text:
            raise SystemExit("nothing to send")
        if not title:  # no file name: the text's first heading
            first = re.search(r"^#+\s*(.+)$", text, re.M)
            title = first.group(1).strip() if first else ("Clipboard" if args.clip else "")
        ok, msg = send_text(args.host, title, text)
        what = "%d lines" % len([l for l in text.split("\n") if l.strip()])
    print("%s (%s)" % (msg, what) if ok else "not sent: %s" % msg)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

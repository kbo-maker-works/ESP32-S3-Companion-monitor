// Guition deck: icon fonts. See deck_icons.h.
#include "ui/deck_icons.h"
#include "ui/deck_config.h"
#include "hardware/deck_sd.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <algorithm>
#include <vector>

namespace deck {
namespace {

constexpr size_t MAX_FILE = 3 * 1024 * 1024;
constexpr size_t MAX_ICONS = 20000;
constexpr int MAX_FONTS = 8;  // distinct icon sizes in use (6 seen)

// name = offset of the glyph's Pascal string inside the font data.
struct Entry {
  uint32_t name;
  uint32_t cp;
};

uint8_t* s_names = nullptr;  // the indexed names as Pascal strings
size_t s_size = 0;
Entry* s_index = nullptr;
size_t s_count = 0;
lv_fs_drv_t s_fsDrv;
bool s_fsReg = false;
struct Sized {
  int px;
  lv_font_t* font;
};
Sized s_fonts[MAX_FONTS] = {};

// --- Icon packs ------------------------------------------------------------------
constexpr uint32_t PACK_LOCAL = 0x100000;  // glyph i of a pack font is U+100000+i
constexpr uint32_t PACK_BASE = 0x100000;   // device codepoints: the packs one after another
constexpr uint32_t PACK_END = 0x10FFFE;    // U+10FFFE and U+10FFFF are noncharacters

struct PackData {
  char* names = nullptr;
  uint32_t* offsets = nullptr;
  size_t count = 0;
  size_t size = 0;
  String version;
  ~PackData() { heap_caps_free(names); heap_caps_free(offsets); }
  void swap(PackData& other) {
    std::swap(names, other.names);
    std::swap(offsets, other.offsets);
    std::swap(count, other.count);
    std::swap(size, other.size);
    std::swap(version, other.version);
  }
};

struct Pack {
  String prefix;
  PackData data;
  uint32_t base = 0;
  Sized fonts[MAX_FONTS] = {};
};
// In prefix order. Fonts point at their pack (user_data): the array only moves
// with every font closed.
Pack s_packs[PACK_MAX];
size_t s_packCount = 0;
lv_fs_drv_t s_packDrv;
bool s_packReg = false;

// The fonts of one pixel size, chained by fallback: MDI first, then each pack
// in the order its first icon of that size was drawn. Labels hold the head.
struct Chain {
  int px;
  lv_font_t* head;
  lv_font_t* tail;
};
Chain s_chains[MAX_FONTS] = {};
decltype(lv_font_t::get_glyph_dsc) s_ttfGlyphDsc = nullptr;

// TinyTTF logs missing glyphs as cache errors, even when a fallback has them,
// so each font only answers for its own range.
bool mdiGlyphDsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc, uint32_t cp, uint32_t next) {
  if (cp >= PACK_BASE) return false;
  return s_ttfGlyphDsc(font, dsc, cp, next);
}

bool packGlyphDsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc, uint32_t cp, uint32_t) {
  const Pack* p = static_cast<const Pack*>(font->user_data);
  if (cp < p->base || cp - p->base >= p->data.count) return false;
  return s_ttfGlyphDsc(font, dsc, PACK_LOCAL + (cp - p->base), 0);
}

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Pascal string at off versus (s, n), memcmp order.
int cmpName(const uint8_t* data, uint32_t off, const char* s, size_t n) {
  const size_t len = data[off];
  const int c = memcmp(data + off + 1, s, std::min(len, n));
  if (c) return c;
  return len < n ? -1 : (len > n ? 1 : 0);
}

bool findTable(const uint8_t* d, size_t size, const char* tag, uint32_t& off, uint32_t& len) {
  if (size < 12) return false;
  const uint16_t n = be16(d + 4);
  if (12 + 16u * n > size) return false;
  for (uint16_t i = 0; i < n; i++) {
    const uint8_t* r = d + 12 + 16 * i;
    if (memcmp(r, tag, 4)) continue;
    off = be32(r + 8);
    len = be32(r + 12);
    return off < size && len <= size - off;
  }
  return false;
}

// Builds the sorted name index of font data d. Returns nullptr with err set
// when the file is not an MDI-style font (needs post v2 names + cmap format 12).
Entry* buildIndex(const uint8_t* d, size_t size, size_t& count, String& err) {
  count = 0;
  uint32_t po, pl, co, cl;
  if (size < 12 || (be32(d) != 0x00010000 && memcmp(d, "true", 4))) {
    err = "not a TrueType font";
    return nullptr;
  }
  if (!findTable(d, size, "post", po, pl) || pl < 34 || be32(d + po) != 0x00020000 ||
      !findTable(d, size, "cmap", co, cl) || cl < 4) {
    err = "font has no glyph names (post v2) or no cmap";
    return nullptr;
  }
  const uint16_t glyphs = be16(d + po + 32);
  const uint32_t idxOff = po + 34;
  if (idxOff + 2u * glyphs > po + pl) {
    err = "broken post table";
    return nullptr;
  }
  // Offsets of the custom names, in order.
  std::vector<uint32_t> custom;
  for (uint32_t p = idxOff + 2u * glyphs; p < po + pl && p + 1 + d[p] <= po + pl; p += 1 + d[p])
    custom.push_back(p);

  // The Unicode full-repertoire subtable (format 12) carries the U+F0000+ icons.
  const uint8_t* groups = nullptr;
  uint32_t nGroups = 0;
  const uint16_t subs = be16(d + co + 2);
  for (uint16_t i = 0; i < subs && 4 + 8u * (i + 1) <= cl; i++) {
    const uint8_t* r = d + co + 4 + 8 * i;
    const uint32_t so = co + be32(r + 4);
    if (so + 16 > co + cl || be16(d + so) != 12) continue;
    const uint32_t n = be32(d + so + 12);
    if (so + 16 + 12ull * n > co + cl) continue;
    groups = d + so + 16;
    nGroups = n;
    break;
  }
  if (!groups) {
    err = "font has no format 12 cmap";
    return nullptr;
  }

  auto nameOf = [&](uint32_t gid) -> int64_t {
    if (gid >= glyphs) return -1;
    const uint16_t ni = be16(d + idxOff + 2 * gid);
    if (ni < 258 || ni - 258u >= custom.size()) return -1;
    return custom[ni - 258];
  };
  size_t n = 0;
  for (int pass = 0; pass < 2; pass++) {
    Entry* out = pass ? (Entry*)heap_caps_malloc(n * sizeof(Entry), MALLOC_CAP_SPIRAM) : nullptr;
    if (pass && !out) {
      err = "out of memory";
      return nullptr;
    }
    size_t k = 0;
    for (uint32_t g = 0; g < nGroups; g++) {
      const uint8_t* gr = groups + 12 * g;
      const uint32_t start = be32(gr), end = be32(gr + 4), gid = be32(gr + 8);
      for (uint32_t cp = start; cp <= end && k < MAX_ICONS; cp++) {
        const int64_t name = nameOf(gid + (cp - start));
        if (name < 0 || d[name] == 0) continue;
        if (pass) out[k] = {(uint32_t)name, cp};
        k++;
      }
    }
    if (!pass) {
      n = k;
      if (n < 1) {
        err = "font has no named icons";
        return nullptr;
      }
      continue;
    }
    std::sort(out, out + n, [d](const Entry& a, const Entry& b) {
      return cmpName(d, a.name, (const char*)d + b.name + 1, d[b.name]) < 0;
    });
    count = n;
    return out;
  }
  return nullptr;
}

void freeFonts() {
  for (Chain& c : s_chains) c = {};
  for (Sized& s : s_fonts) if (s.font) s.font->fallback = nullptr;
  for (size_t i = 0; i < s_packCount; i++)
    for (Sized& s : s_packs[i].fonts) if (s.font) s.font->fallback = nullptr;
  for (Sized& s : s_fonts) {
    if (s.font) lv_tiny_ttf_destroy(s.font);
    s = {};
  }
  for (size_t i = 0; i < s_packCount; i++)
    for (Sized& s : s_packs[i].fonts) {
      if (s.font) lv_tiny_ttf_destroy(s.font);
      s = {};
    }
}

// LVGL drive "I:" = LittleFS, read only, for TinyTTF's file mode.
void* fsOpen(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (mode != LV_FS_MODE_RD) return nullptr;
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  // stdio would buffer a whole 4 KB block in internal RAM per open font;
  // LVGL's read cache (s_fsDrv.cache_size, PSRAM) sits in front anyway.
  f.setBufferSize(128);
  return new File(f);
}
lv_fs_res_t fsClose(lv_fs_drv_t*, void* f) {
  static_cast<File*>(f)->close();
  delete static_cast<File*>(f);
  return LV_FS_RES_OK;
}
lv_fs_res_t fsRead(lv_fs_drv_t*, void* f, void* buf, uint32_t n, uint32_t* got) {
  const int r = static_cast<File*>(f)->read((uint8_t*)buf, n);
  *got = r > 0 ? r : 0;
  return r < 0 ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}
lv_fs_res_t fsSeek(lv_fs_drv_t*, void* f, uint32_t pos, lv_fs_whence_t w) {
  const SeekMode m = w == LV_FS_SEEK_CUR ? SeekCur : w == LV_FS_SEEK_END ? SeekEnd : SeekSet;
  return static_cast<File*>(f)->seek(pos, m) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}
lv_fs_res_t fsTell(lv_fs_drv_t*, void* f, uint32_t* pos) {
  *pos = static_cast<File*>(f)->position();
  return LV_FS_RES_OK;
}

void registerFs() {
  if (s_fsReg) return;
  lv_fs_drv_init(&s_fsDrv);
  s_fsDrv.letter = 'I';
  s_fsDrv.cache_size = 256;
  s_fsDrv.open_cb = fsOpen;
  s_fsDrv.close_cb = fsClose;
  s_fsDrv.read_cb = fsRead;
  s_fsDrv.seek_cb = fsSeek;
  s_fsDrv.tell_cb = fsTell;
  lv_fs_drv_register(&s_fsDrv);
  s_fsReg = true;
}

// LVGL drive "B:" = the microSD, read only. Every size of a pack is its own
// TinyTTF font, but they share one open file: the card has few file slots
// (deck_sd.cpp). Each handle keeps its own position. LVGL runs on one task.
struct SharedFile {
  String path;
  File file;
  int refs = 0;
  uint32_t at = UINT32_MAX;  // the file's position, UINT32_MAX = unknown
};
SharedFile s_shared[PACK_MAX + 1];  // + the install probe
struct SharedHandle {
  SharedFile* sf;
  uint32_t pos;
};

void* packOpen(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (mode != LV_FS_MODE_RD || !wallOnSd()) return nullptr;
  SharedFile* sf = nullptr;
  for (SharedFile& s : s_shared)
    if (s.refs && s.path == path) sf = &s;
  if (!sf) {
    for (SharedFile& s : s_shared)
      if (!s.refs) { sf = &s; break; }
    if (!sf) return nullptr;
    File f = wallFs().open(path, "r");
    if (!f) return nullptr;
    f.setBufferSize(128);
    sf->file = f;
    sf->path = path;
    sf->at = 0;
  }
  sf->refs++;
  return new SharedHandle{sf, 0};
}
lv_fs_res_t packClose(lv_fs_drv_t*, void* h) {
  SharedHandle* sh = static_cast<SharedHandle*>(h);
  if (--sh->sf->refs == 0) {
    sh->sf->file.close();
    sh->sf->path = "";
  }
  delete sh;
  return LV_FS_RES_OK;
}
lv_fs_res_t packRead(lv_fs_drv_t*, void* h, void* buf, uint32_t n, uint32_t* got) {
  SharedHandle* sh = static_cast<SharedHandle*>(h);
  SharedFile& sf = *sh->sf;
  *got = 0;
  if (sf.at != sh->pos && !sf.file.seek(sh->pos)) {
    sf.at = UINT32_MAX;
    return LV_FS_RES_UNKNOWN;
  }
  const int r = sf.file.read((uint8_t*)buf, n);
  if (r < 0) {
    sf.at = UINT32_MAX;
    return LV_FS_RES_UNKNOWN;
  }
  *got = r;
  sh->pos += r;
  sf.at = sh->pos;
  return LV_FS_RES_OK;
}
lv_fs_res_t packSeek(lv_fs_drv_t*, void* h, uint32_t pos, lv_fs_whence_t w) {
  SharedHandle* sh = static_cast<SharedHandle*>(h);
  sh->pos = w == LV_FS_SEEK_CUR ? sh->pos + pos : w == LV_FS_SEEK_END ? sh->sf->file.size() + pos : pos;
  return LV_FS_RES_OK;
}
lv_fs_res_t packTell(lv_fs_drv_t*, void* h, uint32_t* pos) {
  *pos = static_cast<SharedHandle*>(h)->pos;
  return LV_FS_RES_OK;
}

// TinyTTF does not close its file when the font fails to initialise: close
// it here (the leaked handle is never used again).
void sharedDrop(const char* path) {
  for (SharedFile& s : s_shared)
    if (s.refs && s.path == path) {
      s.file.close();
      s.path = "";
      s.refs = 0;
    }
}

void registerPackFs() {
  if (s_packReg) return;
  lv_fs_drv_init(&s_packDrv);
  s_packDrv.letter = 'B';
  s_packDrv.cache_size = 256;
  s_packDrv.open_cb = packOpen;
  s_packDrv.close_cb = packClose;
  s_packDrv.read_cb = packRead;
  s_packDrv.seek_cb = packSeek;
  s_packDrv.tell_cb = packTell;
  lv_fs_drv_register(&s_packDrv);
  s_packReg = true;
}

// Custom Brand Icons came first and keep their file names.
String packFile(const String& prefix, const char* ext) {
  return String(SD_ROOT "/iconos/") + (prefix == "phu" ? "cbi" : prefix.c_str()) + ext;
}

uint8_t* readPackFile(const char* path, size_t limit, size_t& size, String& err) {
  File f = wallFs().open(path, "r");
  size = f ? f.size() : 0;
  if (!size || size > limit) {
    err = "missing, empty or oversized icon pack file";
    return nullptr;
  }
  f.setBufferSize(128);
  uint8_t* data = (uint8_t*)heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
  if (!data) { err = "out of memory"; return nullptr; }
  size_t got = 0;
  while (got < size) {
    const int n = f.read(data + got, std::min<size_t>(32768, size - got));
    if (n <= 0) break;
    got += n;
    esp_task_wdt_reset();
  }
  if (got != size) {
    heap_caps_free(data);
    err = "short read of icon pack file";
    return nullptr;
  }
  data[size] = 0;
  return data;
}

bool packNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}

bool validPackName(const char* name) {
  const uint8_t* p = (const uint8_t*)name;
  while (*p) {
    if (*p < 0x80) {
      if (!packNameChar(*p++)) return false;
      continue;
    }
    const uint8_t lead = *p++;
    const unsigned bytes = lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 :
                           lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
    if (!bytes) return false;
    uint32_t cp = lead & (0x7F >> bytes);
    for (unsigned i = 1; i < bytes; i++) {
      if ((*p & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (*p++ & 0x3F);
    }
    if (cp < 0xA0 || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
  }
  return true;
}

bool readPackNames(const char* path, PackData& out, String& err) {
  size_t size;
  out.names = (char*)readPackFile(path, PACK_MAX_NAMES, size, err);
  if (!out.names) return false;
  if (memchr(out.names, 0, size) || out.names[0] != '#') return false;
  char* first = strchr(out.names, '\n');
  if (!first || first - out.names < 2 || first - out.names > 64) return false;
  *first++ = 0;
  for (const char* p = out.names + 1; *p; p++)
    if (!packNameChar(*p) && *p != '+') return false;
  out.version = out.names + 1;
  char* end = out.names + size;
  char* prev = nullptr;
  for (char* p = first; p < end;) {
    char* next = strchr(p, '\n');
    if (next) *next++ = 0;
    else next = end;
    const size_t len = strlen(p);
    if (!len || len > 127 || (prev && strcmp(prev, p) >= 0) || ++out.count > MAX_ICONS) return false;
    if (!validPackName(p)) return false;
    prev = p;
    p = next;
  }
  if (!out.count) return false;
  out.offsets = (uint32_t*)heap_caps_malloc(out.count * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  if (!out.offsets) { err = "out of memory"; return false; }
  char* p = first;
  for (size_t i = 0; i < out.count; i++) {
    out.offsets[i] = p - out.names;
    p += strlen(p) + 1;
  }
  return true;
}

bool readAt(File& f, uint32_t off, uint8_t* buf, size_t n) {
  return f.seek(off) && f.read(buf, n) == (int)n;
}

// The font the portal builds: CFF outlines, 512 units per em, every glyph 512
// wide, cmap format 12 mapping U+100000+i to glyph i+1. Only the checked
// tables are read, so a big pack needs no RAM for this.
bool validPackFont(File& f, size_t size, size_t count) {
  uint8_t hdr[12];
  if (size < 12 || !readAt(f, 0, hdr, 12) || memcmp(hdr, "OTTO", 4)) return false;
  const size_t tables = be16(hdr + 4);
  if (!tables || tables > 64 || 12 + 16 * tables > size) return false;
  std::vector<uint8_t> dir(16 * tables);
  if (!readAt(f, 12, dir.data(), dir.size())) return false;
  for (size_t i = 0; i < tables; i++) {
    const uint8_t* r = dir.data() + 16 * i;
    const uint32_t off = be32(r + 8), len = be32(r + 12);
    if (off > size || len > size - off) return false;
  }
  auto find = [&](const char* tag, uint32_t& off, uint32_t& len) {
    for (size_t i = 0; i < tables; i++) {
      const uint8_t* r = dir.data() + 16 * i;
      if (memcmp(r, tag, 4)) continue;
      off = be32(r + 8);
      len = be32(r + 12);
      return true;
    }
    return false;
  };
  uint32_t off, len;
  uint8_t t[54];
  if (!find("head", off, len) || len < 54 || !readAt(f, off, t, 54) || be16(t + 18) != 512) return false;
  if (!find("maxp", off, len) || len < 6 || !readAt(f, off, t, 6) || be16(t + 4) != count + 1) return false;
  if (!find("hhea", off, len) || len < 36 || !readAt(f, off, t, 36) || be16(t + 4) != 448 ||
      (int16_t)be16(t + 6) != -64 || be16(t + 8) != 0) return false;
  const size_t metrics = be16(t + 34);
  if (!metrics || metrics > count + 1 || !find("hmtx", off, len) ||
      len < 4 * metrics + 2 * (count + 1 - metrics)) return false;
  uint8_t chunk[256];
  for (size_t i = 0; i < metrics;) {
    const size_t n = std::min<size_t>(metrics - i, sizeof(chunk) / 4);
    if (!readAt(f, off + 4 * i, chunk, 4 * n)) return false;
    for (size_t k = 0; k < n; k++) if (be16(chunk + 4 * k) != 512) return false;
    i += n;
  }
  if (!find("CFF ", off, len) || len < 4 || !readAt(f, off, t, 4) || t[0] != 1 || t[2] < 4 ||
      t[2] > len || t[3] < 1 || t[3] > 4) return false;
  if (!find("cmap", off, len) || len < 4 || len > 65536) return false;
  uint8_t* cmap = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!cmap) return false;
  bool found = false, ok = readAt(f, off, cmap, len);
  const size_t subs = ok ? be16(cmap + 2) : 0;
  if (4 + 8 * subs > len) ok = false;
  for (size_t i = 0; ok && i < subs; i++) {
    const uint8_t* r = cmap + 4 + 8 * i;
    const uint32_t sub = be32(r + 4);
    if (sub > len || len - sub < 2) { ok = false; break; }
    if (be16(cmap + sub) != 12) continue;
    if (len - sub < 16) { ok = false; break; }
    const uint8_t* p = cmap + sub;
    const uint32_t bytes = be32(p + 4), groups = be32(p + 12);
    if (bytes > len - sub || bytes < 16 || groups > (bytes - 16) / 12) { ok = false; break; }
    uint32_t next = PACK_LOCAL;
    for (uint32_t g = 0; ok && g < groups; g++) {
      const uint8_t* gr = p + 16 + 12 * g;
      const uint32_t start = be32(gr), end = be32(gr + 4), gid = be32(gr + 8);
      if (g == 0 && start == 0 && end == 0 && gid == 0) continue;
      if (start != next || end < start || end >= PACK_LOCAL + count || gid != start - PACK_LOCAL + 1)
        ok = false;
      next = end + 1;
    }
    if (ok && next != PACK_LOCAL + count) ok = false;
    if (ok && (be16(r) == 0 || (be16(r) == 3 && be16(r + 2) == 10))) found = true;
  }
  heap_caps_free(cmap);
  return ok && found;
}

// probe = also open it with TinyTTF (install); boot skips that.
bool readPack(const char* font, const char* names, PackData& out, String& err, bool probe) {
  err = "";
  if (!wallOnSd()) { err = "icon packs need a microSD card"; return false; }
  if (!readPackNames(names, out, err)) {
    if (!err.length()) err = "invalid or unsorted icon names";
    return false;
  }
  File f = wallFs().open(font, "r");
  out.size = f ? f.size() : 0;
  if (!out.size || out.size > PACK_MAX_FILE) { err = "missing or oversized icon font"; return false; }
  const bool valid = validPackFont(f, out.size, out.count);
  f.close();
  if (!valid) { err = "icon font metrics or codepoints do not match the names"; return false; }
  registerPackFs();
  if (!probe) return true;
  lv_font_t* font24 = lv_tiny_ttf_create_file_ex((String("B:") + font).c_str(), 24, LV_FONT_KERNING_NONE, 1);
  if (!font24) {
    sharedDrop(font);
    err = "cannot open icon font";
    return false;
  }
  lv_tiny_ttf_destroy(font24);
  return true;
}

bool removeIfPresent(const String& path) {
  return !wallFs().exists(path) || wallFs().remove(path);
}

// A journal makes the two file renames recoverable as a pair after a restart.
bool recoverPack(const String& prefix, String& err) {
  const String journalPath = packFile(prefix, ".pending");
  if (!wallFs().exists(journalPath)) return true;
  File journal = wallFs().open(journalPath, "r");
  const int flags = journal ? journal.read() : -1;
  journal.close();
  if (flags < 0 || flags > 3) { err = "cannot read icon pack install journal"; return false; }
  const String paths[] = {packFile(prefix, ".otf"), packFile(prefix, ".names")};
  for (int i = 0; i < 2; i++) {
    const String backup = paths[i] + ".bak";
    bool ok = true;
    if (wallFs().exists(backup))
      ok = removeIfPresent(paths[i]) && wallFs().rename(backup, paths[i]);
    else if (!(flags & (1 << i))) ok = removeIfPresent(paths[i]);
    if (!ok) { err = "cannot restore previous icon pack files"; return false; }
  }
  if (!wallFs().remove(journalPath)) { err = "cannot clear icon pack install journal"; return false; }
  return true;
}

// Fonts closed only (they point at their pack).
void movePack(Pack& to, Pack& from) {
  to.prefix = from.prefix;
  to.data.swap(from.data);
  to.base = from.base;
}

void removePackAt(size_t at) {
  for (size_t i = at; i + 1 < s_packCount; i++) movePack(s_packs[i], s_packs[i + 1]);
  Pack& last = s_packs[--s_packCount];
  PackData empty;
  last.data.swap(empty);
  last.prefix = "";
  last.base = 0;
}

void insertPack(const String& prefix, PackData& data) {
  size_t at = 0;
  while (at < s_packCount && s_packs[at].prefix < prefix) at++;
  for (size_t i = s_packCount; i > at; i--) movePack(s_packs[i], s_packs[i - 1]);
  s_packs[at].prefix = prefix;
  s_packs[at].data.swap(data);
  s_packCount++;
}

void assignBases() {
  uint32_t base = PACK_BASE;
  for (size_t i = 0; i < s_packCount; i++) {
    s_packs[i].base = base;
    base += s_packs[i].data.count;
  }
}

size_t iconsBesides(int skip) {
  size_t total = 0;
  for (size_t i = 0; i < s_packCount; i++)
    if ((int)i != skip) total += s_packs[i].data.count;
  return total;
}

const Pack* packOf(uint32_t cp) {
  for (size_t i = 0; i < s_packCount; i++)
    if (cp >= s_packs[i].base && cp - s_packs[i].base < s_packs[i].data.count) return &s_packs[i];
  return nullptr;
}

// Copies the indexed names out of the font data, so the data can go.
uint8_t* packNames(const uint8_t* d, Entry* index, size_t count) {
  size_t total = 0;
  for (size_t i = 0; i < count; i++) total += 1 + d[index[i].name];
  uint8_t* pool = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
  if (!pool) return nullptr;
  size_t at = 0;
  for (size_t i = 0; i < count; i++) {
    const size_t n = 1 + d[index[i].name];
    memcpy(pool + at, d + index[i].name, n);
    index[i].name = at;
    at += n;
  }
  return pool;
}

}  // namespace

const char* iconsPath() { return "/mdi.ttf"; }

bool iconsLoad(const char* path, String& err) {
  if (!deckFsReady() || !LittleFS.exists(path)) {
    err = "no icon font on the device";
    return false;
  }
  File f = LittleFS.open(path, "r");
  const size_t size = f ? f.size() : 0;
  if (!size || size > MAX_FILE) {
    if (f) f.close();
    err = size ? "file too big" : "cannot read the file";
    return false;
  }
  uint8_t* data = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!data) {
    f.close();
    err = "out of memory";
    return false;
  }
  size_t got = 0;
  while (got < size) {
    const int r = f.read(data + got, std::min<size_t>(32768, size - got));
    if (r <= 0) break;
    got += r;
    esp_task_wdt_reset();
  }
  f.close();
  size_t count = 0;
  Entry* index = got == size ? buildIndex(data, size, count, err) : nullptr;
  if (got != size) err = "short read";
  uint8_t* names = index ? packNames(data, index, count) : nullptr;
  heap_caps_free(data);
  if (!names) {
    if (index && !err.length()) err = "out of memory";
    heap_caps_free(index);
    return false;
  }
  freeFonts();
  heap_caps_free(s_index);
  heap_caps_free(s_names);
  s_names = names;
  s_size = size;
  s_index = index;
  s_count = count;
  registerFs();
  Serial.printf("Deck: icon font %u bytes, %u icons\n", (unsigned)size, (unsigned)count);
  return true;
}

void iconsUnload() {
  freeFonts();
  heap_caps_free(s_index);
  heap_caps_free(s_names);
  s_index = nullptr;
  s_names = nullptr;
  s_count = 0;
  s_size = 0;
}

bool iconsReady() { return s_index != nullptr; }
size_t iconsCount() { return s_count; }
size_t iconsFileSize() { return s_size; }

bool packPrefixValid(const String& prefix) {
  const size_t n = prefix.length();
  if (!n || n > 24 || prefix == "mdi" || prefix == "cbi" || prefix[0] == '-') return false;
  for (size_t i = 0; i < n; i++) {
    const char c = prefix[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

String packFontPath(const String& prefix) { return packFile(prefix, ".otf"); }
String packNamesPath(const String& prefix) { return packFile(prefix, ".names"); }

int packFind(const String& prefix) {
  for (size_t i = 0; i < s_packCount; i++)
    if (s_packs[i].prefix == prefix) return i;
  return -1;
}

bool packInfo(size_t i, PackInfo& out) {
  if (i >= s_packCount) return false;
  const Pack& p = s_packs[i];
  out = {p.prefix, p.data.version, p.data.count, p.data.size};
  return true;
}

void packsLoad() {
  freeFonts();
  while (s_packCount) removePackAt(s_packCount - 1);
  if (!wallOnSd()) return;
  // Every pack named by a file on the card, half-installed ones too.
  std::vector<String> found;
  File dir = wallFs().open(SD_ROOT "/iconos");
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String name = f.name();
      f.close();
      name = name.substring(name.lastIndexOf('/') + 1);
      for (const char* ext : {".names", ".names.bak", ".pending"}) {
        if (!name.endsWith(ext)) continue;
        String prefix = name.substring(0, name.length() - strlen(ext));
        if (prefix == "cbi") prefix = "phu";
        if (packPrefixValid(prefix) && std::find(found.begin(), found.end(), prefix) == found.end())
          found.push_back(prefix);
      }
    }
  }
  dir.close();
  std::sort(found.begin(), found.end());
  for (const String& prefix : found) {
    String err;
    PackData next;
    if (s_packCount >= PACK_MAX) err = "too many icon packs";
    else if (recoverPack(prefix, err) && wallFs().exists(packFontPath(prefix)) &&
             readPack(packFontPath(prefix).c_str(), packNamesPath(prefix).c_str(), next, err, false) &&
             iconsBesides(-1) + next.count > PACK_END - PACK_BASE)
      err = "too many icons";
    if (!err.length() && next.count) {
      Serial.printf("Deck: icon pack %s: %u bytes, %u icons (%s)\n", prefix.c_str(), (unsigned)next.size,
                    (unsigned)next.count, next.version.c_str());
      insertPack(prefix, next);
    } else if (err.length()) {
      Serial.printf("Deck: icon pack %s: %s\n", prefix.c_str(), err.c_str());
    }
  }
  assignBases();
}

bool packInstall(const String& prefix, const char* font, const char* names, String& err) {
  if (!packPrefixValid(prefix)) { err = "invalid pack prefix"; return false; }
  PackData next;
  if (!readPack(font, names, next, err, true)) return false;
  const int at = packFind(prefix);
  if (at < 0 && s_packCount >= PACK_MAX) { err = "too many icon packs: remove one first"; return false; }
  if (iconsBesides(at) + next.count > PACK_END - PACK_BASE) {
    err = "too many icons on the device: remove a pack first";
    return false;
  }
  freeFonts();
  if (!recoverPack(prefix, err)) return false;
  const String paths[] = {packFontPath(prefix), packNamesPath(prefix)};
  const char* uploads[] = {font, names};
  const String journalPath = packFile(prefix, ".pending");
  uint8_t flags = 0;
  for (int i = 0; i < 2; i++) {
    if (!removeIfPresent(paths[i] + ".bak")) {
      err = "cannot clear old icon pack backup";
      return false;
    }
    if (wallFs().exists(paths[i])) flags |= 1 << i;
  }
  File journal = wallFs().open(journalPath, "w");
  const bool written = journal && journal.write(&flags, 1) == 1;
  journal.close();
  if (!written) {
    wallFs().remove(journalPath);
    err = "cannot create icon pack install journal";
    return false;
  }
  bool ok = true;
  for (int i = 0; i < 2 && ok; i++) {
    if (flags & (1 << i)) ok = wallFs().rename(paths[i], paths[i] + ".bak");
    if (ok) ok = wallFs().rename(uploads[i], paths[i]);
  }
  if (ok) ok = wallFs().remove(journalPath);
  if (!ok) {
    err = "cannot store icon pack files";
    String recovery;
    if (!recoverPack(prefix, recovery)) {
      if (at >= 0) removePackAt(at);
      assignBases();
      err += "; " + recovery;
    }
    return false;
  }
  if (at >= 0) s_packs[at].data.swap(next);
  else insertPack(prefix, next);
  assignBases();
  for (const String& path : paths) wallFs().remove(path + ".bak");
  return true;
}

bool packRemove(const String& prefix, String& err) {
  err = "";
  if (!wallOnSd()) { err = "icon packs need a microSD card"; return false; }
  if (!packPrefixValid(prefix)) { err = "invalid pack prefix"; return false; }
  freeFonts();
  if (!recoverPack(prefix, err)) return false;
  const bool font = removeIfPresent(packFontPath(prefix));
  const bool names = removeIfPresent(packNamesPath(prefix));
  const int at = packFind(prefix);
  if ((font || names) && at >= 0) {
    removePackAt(at);
    assignBases();
  }
  if (!font || !names) { err = "cannot remove icon pack files"; return false; }
  return true;
}

uint32_t iconCodepoint(const String& name) {
  const char* s = name.c_str();
  size_t n = name.length();
  const char* colon = strchr(s, ':');
  if (colon && strncmp(s, "mdi:", 4)) {
    const size_t plen = colon - s;
    for (size_t i = 0; i < s_packCount; i++) {
      const Pack& p = s_packs[i];
      if (p.prefix.length() != plen || memcmp(p.prefix.c_str(), s, plen)) continue;
      size_t lo = 0, hi = p.data.count;
      while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const int c = strcmp(p.data.names + p.data.offsets[mid], colon + 1);
        if (c == 0) return p.base + mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
      }
      return 0;
    }
    return 0;  // another icon set (hass:, custom) - not installed
  }
  if (!s_index) return 0;
  if (colon) {
    s += 4;
    n -= 4;
  }
  size_t lo = 0, hi = s_count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = cmpName(s_names, s_index[mid].name, s, n);
    if (c == 0) return s_index[mid].cp;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

// fresh = it was opened by this call.
static lv_font_t* sizedFont(Sized (&fonts)[MAX_FONTS], const char* path, int px, bool& fresh) {
  fresh = false;
  for (const Sized& s : fonts)
    if (s.font && s.px == px) return s.font;
  for (Sized& s : fonts) {
    if (s.font) continue;
    s.font = lv_tiny_ttf_create_file_ex(path, px, LV_FONT_KERNING_NONE, 64);
    s.px = px;
    fresh = s.font != nullptr;
    return s.font;
  }
  return nullptr;  // all size slots taken: a caller asked for too many sizes
}

static void chainLink(int px, lv_font_t* font) {
  for (Chain& c : s_chains)
    if (c.head && c.px == px) {
      c.tail->fallback = font;
      c.tail = font;
      return;
    }
  for (Chain& c : s_chains)
    if (!c.head) {
      c = {px, font, font};
      return;
    }
}

const lv_font_t* iconFont(int px, uint32_t cp) {
  bool fresh;
  lv_font_t* mdi = s_index ? sizedFont(s_fonts, "I:/mdi.ttf", px, fresh) : nullptr;
  if (mdi && fresh) {
    s_ttfGlyphDsc = mdi->get_glyph_dsc;
    mdi->get_glyph_dsc = mdiGlyphDsc;
    chainLink(px, mdi);
  }
  if (cp < PACK_BASE) return mdi;
  // Pack sizes open only where one of the pack's icons is shown.
  Pack* p = const_cast<Pack*>(packOf(cp));
  lv_font_t* font = p ? sizedFont(p->fonts, ("B:" + packFontPath(p->prefix)).c_str(), px, fresh) : nullptr;
  if (!font) return nullptr;
  if (fresh) {
    s_ttfGlyphDsc = font->get_glyph_dsc;
    font->get_glyph_dsc = packGlyphDsc;
    font->user_data = p;
    chainLink(px, font);
  }
  for (const Chain& c : s_chains)
    if (c.head && c.px == px) return c.head;
  return font;  // no chain slot left: this pack alone
}

void iconUtf8(uint32_t cp, char out[5]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    out[1] = 0;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | cp >> 6);
    out[1] = (char)(0x80 | (cp & 0x3F));
    out[2] = 0;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | cp >> 12);
    out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = 0;
  } else {
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = 0;
  }
}

}  // namespace deck

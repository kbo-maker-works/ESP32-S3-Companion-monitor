// Guition deck: MDI on LittleFS and optional icon packs on microSD.
// TinyTTF reads glyphs on demand; name -> codepoint indexes live in PSRAM.
#ifndef DECK_ICONS_H
#define DECK_ICONS_H

#include <Arduino.h>
#include <lvgl.h>

namespace deck {

// Where the font lives on LittleFS.
const char* iconsPath();
// Index the font at path; iconFont() then reads iconsPath(). The current
// index stays when the new font fails. Fonts from iconFont() die here: free
// their labels first.
bool iconsLoad(const char* path, String& err);
// Close the fonts (they hold iconsPath() open) and drop the index.
void iconsUnload();
bool iconsReady();
size_t iconsCount();
size_t iconsFileSize();

// Icon packs on microSD (Custom Brand Icons, Simple Icons, Phosphor, any
// fill-drawn Iconify set): a CFF font the portal builds from the set's SVGs,
// glyph i at U+100000+i, plus its sorted names. Each answers to its own prefix
// ("si:github") and gets its own codepoint range on the device.
constexpr size_t PACK_MAX = 8;
constexpr size_t PACK_MAX_FILE = 8 * 1024 * 1024;
constexpr size_t PACK_MAX_NAMES = 512 * 1024;
struct PackInfo {
  String prefix, version;
  size_t count, size;
};
// 1-24 of a-z, 0-9 and "-", starting with a letter or digit; not "mdi".
bool packPrefixValid(const String& prefix);
String packFontPath(const String& prefix);
String packNamesPath(const String& prefix);
// Call after SD mount and LVGL init: every pack on the card. Invalid ones are
// skipped (and logged).
void packsLoad();
// As with iconsLoad(), callers must release labels before changing packs.
bool packInstall(const String& prefix, const char* font, const char* names, String& err);
bool packRemove(const String& prefix, String& err);
// Index of the installed pack, -1 when there is none with that prefix.
int packFind(const String& prefix);
// Pack i, in prefix order; false past the last one.
bool packInfo(size_t i, PackInfo& out);

// "mdi:name", "name", or "<pack>:name" -> codepoint; 0 when unknown or unavailable.
uint32_t iconCodepoint(const String& name);
// TinyTTF font for this pixel size (cached); nullptr when no font is loaded.
// Pass the codepoint to draw: a pack icon brings its pack in as a fallback.
const lv_font_t* iconFont(int px, uint32_t cp = 0);
// UTF-8 text of one codepoint, for lv_label_set_text.
void iconUtf8(uint32_t cp, char out[5]);

}  // namespace deck

#endif  // DECK_ICONS_H

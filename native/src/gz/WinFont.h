// The font as the text-bearing window controls use it (cIGZFont vslots noted), and where they get fonts from.
#pragma once
#include <cstdint>

#include "gz/Font.h"
#include "gz/Win.h"

namespace gz {

class IWinFont : public cIGZUnknown {
public:
    // The full font interface (Font.h) of the same object, for the controls that take it; null when the font has none.
    virtual cIGZFont* AsFont() { return nullptr; }
    virtual std::uint32_t DefaultTextColor() = 0;                                   // 0x60
    virtual std::uint32_t DefaultShadowColor() = 0;                                 // 0x68
    virtual std::int16_t LineHeight(int a, int b) = 0;                              // 0x128
    virtual int CountLines(const char* text, std::uint32_t length, int width) = 0;  // 0x158
    virtual int WrappedWidth(const char* text, std::uint32_t length, int height) = 0;   // 0x160
    virtual void MeasureText(const char* text, std::uint32_t length, cRZRect& out) = 0;  // 0x178
    virtual int TextWidth(const char* text, std::uint32_t length) = 0;              // 0x180
    virtual void DrawText(IWinBuffer* target, int x, int y, const char* text, std::uint32_t length,
                          std::uint32_t color, std::uint32_t color2, const cRZRect* clip, int flags) = 0;   // 0x198
    // The list box asks the font for these; fonts that do not model them fall back to the plain measures.
    virtual int GlyphHeight() { return LineHeight(0, 0); }                                                          // 0x118
    virtual std::uint32_t FitLength(const char*, std::uint32_t length, int, int) { return length; }                 // 0x150: how many characters fit
    virtual std::uint32_t MeasureTextWidth(const char* text, std::uint32_t length, std::uint32_t*) { return static_cast<std::uint32_t>(TextWidth(text, length)); }   // 0x188
    virtual void DrawTextBox(IWinBuffer* target, const cRZRect& box, const char* text, std::uint32_t length,
                             std::uint32_t alignment, std::uint32_t color, std::uint32_t color2, const cRZRect* clip, int flags) = 0;   // 0x1a0
    // cIGZFont [41] 0x148: how many bytes of `text` fit on one line of `width` (mode 2 is the break cSC4WinText uses).
    // Defaults to the plain character fit when the font does not expose the full interface.
    virtual std::uint32_t WordsToFitInWidth(const char* text, std::uint32_t length, int width,
                                            std::uint32_t* fitted, int mode) {
        if (cIGZFont* font = AsFont())
            return font->CalculateWordsToFitInWidth(text, length, static_cast<std::uint32_t>(width), fitted,
                                                    static_cast<cIGZFont::WordWrapMode>(mode));
        return FitLength(text, length, width, 0);
    }
    // cIGZFont [50] 0x190: shortens `text` in place so it fits `area`, appending `ellipsis` (the second argument is
    // the ellipsis string, not the text: cGZFont::FormatForEllipsis(text, ellipsis, area, skip)). False when the font
    // has no full interface; the text is then left as it is.
    virtual bool FormatForEllipsis(cIGZString& text, const cIGZString& ellipsis, const cRZRect& area, bool skip) {
        if (cIGZFont* font = AsFont()) return font->FormatForEllipsis(text, ellipsis, area, skip);
        return false;
    }
};

// RZ::WCM() vslot 0x28: the font registered under a style id (borrowed pointer, null when unknown).
class IWinFontSource {
public:
    virtual ~IWinFontSource() = default;
    virtual IWinFont* Font(std::uint32_t styleId) = 0;
    virtual bool Available() = 0;
    // The style a script names ("font=GenBodyLight": the names of the [Font Styles] table); null when unknown.
    virtual IWinFont* FontByName(const char* name) { (void)name; return nullptr; }
    virtual std::uint32_t StyleId(const char* name) { (void)name; return 0; }   // the name's style id (0: unknown)
};
IWinFontSource* WinFontSource();
void SetWinFontSource(IWinFontSource* source);

// The window font of a cIGZFont. The original's control manager and controls hold the cIGZFont itself; the port's
// controls draw through IWinFont, whose surfaces are the platform's, so the platform supplies the maker. The view holds
// a reference on the font and comes back with one reference of its own; null without a maker or font.
using WinFontMaker = IWinFont* (*)(cIGZFont* font);
IWinFont* MakeWinFont(cIGZFont* font);
void SetWinFontMaker(WinFontMaker maker);

constexpr std::uint32_t kWinDefaultFontStyle = 0x68963c4c;
constexpr std::uint32_t kWinUnsetTextColor = 0xdeadbeef;

}  // namespace gz

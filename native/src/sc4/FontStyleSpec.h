// The parse half of CreateFontStyleFromSpecification (Mac i386 0x002ecc7c): a [Font Styles] value of FontStyle.ini,
// "<faces>, <size>, <param|param...>, <style id hex>", as what the function acquires and sets on the font.
#pragma once
#include <cstdint>
#include <functional>

#include "rz/cRZString.h"

namespace sc4 {

struct FontStyleSpec {
    cRZString faces;                 // field 0, handed to cIGZFontSys::FontAcquire as the name list
    std::uint32_t size = 0;          // field 1 (RZConvertToUint32A)
    std::uint32_t styleFlags = 0;    // bold 1, italic 2, under/underline 4, strike/strikethrough 8, shadow 0x10
    std::uint32_t styleId = 0;       // field 3 (RZAtoUint32HexA)
    int antialiasing = 0;            // aa=none 1, aa=color 2, aa=bg / aa=background 3; 0 = not given
    bool colorsGiven = false;        // color= or bgcolor= was given
    std::uint32_t textColor = 0;     // color=r,g,b as the display's native color
    std::uint32_t backgroundColor = 0;   // bgcolor=r,g,b
    float gamma = 0.0f;              // 0 = not given
    float sharpness = 0.0f;          // 0 = not given
    float scaleX = 1.0f, scaleY = 1.0f;  // xscale / yscale
    int charSpacing = 0, lineSpacing = 0;
    float advanceScaleX = 1.0f;      // xadvancescale
};

// Keys and values compare in lower case; a parameter without '=' is a bare key; unknown keys are ignored; a numeric
// key with an empty value keeps its default. `nativeColor` turns r, g, b into the display's color (RZ::GS() vslot 0x8c).
FontStyleSpec ParseFontStyleSpecification(const cRZString& specification,
                                          const std::function<std::uint32_t(std::uint8_t, std::uint8_t, std::uint8_t)>& nativeColor);

}  // namespace sc4

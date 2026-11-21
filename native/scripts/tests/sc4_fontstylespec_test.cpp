// The [Font Styles] specification parse of CreateFontStyleFromSpecification (sc4/FontStyleSpec.h), on lines of the
// shape the game's FontStyle.ini uses and on every parameter the original reads.
#include <cstdio>
#include <cstring>

#include "rz/cRZString.h"
#include "sc4/FontStyleSpec.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
std::uint32_t Pack(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return std::uint32_t(r) << 16 | std::uint32_t(g) << 8 | b; }
sc4::FontStyleSpec Parse(const char* text) { return sc4::ParseFontStyleSpecification(cRZString(text), &Pack); }
}  // namespace

int main() {
    // a quoted line as the shipped file writes it
    {
        const auto s = Parse("\"StocletITC TT\",   \"15\",    \"bold|aa=bg|linespacing=2|xscale=1.25|xadvancescale=0.99\",   0xea85d303");
        CHECK(std::strcmp(s.faces.ToChar(), "StocletITC TT") == 0);
        CHECK(s.size == 15);
        CHECK(s.styleFlags == 1);
        CHECK(s.styleId == 0xea85d303u);
        CHECK(s.antialiasing == 3);
        CHECK(s.lineSpacing == 2 && s.charSpacing == 0);
        CHECK(s.scaleX == 1.25f && s.scaleY == 1.0f);
        CHECK(s.advanceScaleX > 0.989f && s.advanceScaleX < 0.991f);
        CHECK(!s.colorsGiven && s.gamma == 0.0f && s.sharpness == 0.0f);
    }
    // style flags: both spellings of underline and strikethrough, shadow; keys compare without case
    {
        const auto s = Parse("\"Arta\", \"14\", \"Italic|UNDER|strike|Shadow\", 0x5a809915");
        CHECK(s.styleFlags == (2u | 4u | 8u | 0x10u));
        const auto t = Parse("\"Arta\", \"14\", \"underline|strikethrough\", 0x1");
        CHECK(t.styleFlags == (4u | 8u));
    }
    // antialiasing values; an unknown value leaves it unset
    {
        CHECK(Parse("\"Arta\",\"9\",\"aa=None\",0x1").antialiasing == 1);
        CHECK(Parse("\"Arta\",\"9\",\"aa=color\",0x1").antialiasing == 2);
        CHECK(Parse("\"Arta\",\"9\",\"AA=Background\",0x1").antialiasing == 3);
        CHECK(Parse("\"Arta\",\"9\",\"aa=sharp\",0x1").antialiasing == 0);
    }
    // colors go through the native color maker; text and background apart
    {
        const auto s = Parse("\"Arta\",\"12\",\"color=234,12,0|bgcolor=1,2,3\",0x2");
        CHECK(s.colorsGiven);
        CHECK(s.textColor == 0xea0c00u);
        CHECK(s.backgroundColor == 0x010203u);
        const auto t = Parse("\"Arta\",\"12\",\"bgcolor=9,9,9\",0x2");
        CHECK(t.colorsGiven && t.textColor == 0 && t.backgroundColor == 0x090909u);
    }
    // the numbers, an empty value keeps the default, an unknown key is ignored
    {
        const auto s = Parse("\"Arta\",\"12\",\"gamma=1.5|sharpness=0.25|yscale=0.5|charspacing=-1|linespacing=|wobble=3\",0x3");
        CHECK(s.gamma == 1.5f && s.sharpness == 0.25f);
        CHECK(s.scaleX == 1.0f && s.scaleY == 0.5f);
        CHECK(s.charSpacing == -1 && s.lineSpacing == 0);
        CHECK(s.styleFlags == 0 && s.antialiasing == 0);
    }
    // several faces stay one field (the quotes keep the commas)
    {
        const auto s = Parse("\"Arta,Arial\", \"20\", \"\", 0x28963c70");
        CHECK(std::strcmp(s.faces.ToChar(), "Arta,Arial") == 0);
        CHECK(s.size == 20 && s.styleId == 0x28963c70u);
    }
    if (failures == 0) std::printf("sc4_fontstylespec: all checks passed\n");
    return failures == 0 ? 0 : 1;
}

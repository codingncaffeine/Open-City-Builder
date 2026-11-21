#include "sc4/FontStyleSpec.h"

#include <cstdlib>
#include <string>

#include "rz/RZStringUtils.h"

namespace sc4 {

FontStyleSpec ParseFontStyleSpecification(const cRZString& specification,
                                          const std::function<std::uint32_t(std::uint8_t, std::uint8_t, std::uint8_t)>& nativeColor) {
    FontStyleSpec spec;
    cRZString fields[4];
    unsigned fieldCount = 4;
    RZParseDelimitedText(specification.ToChar(), specification.Strlen(), ',', fields, fieldCount);
    spec.faces = fields[0];
    spec.size = static_cast<std::uint32_t>(std::strtoul(fields[1].ToChar(), nullptr, 10));   // RZConvertToUint32A
    cRZString parameters[12];
    unsigned parameterCount = 12;
    RZParseDelimitedText(fields[2].ToChar(), fields[2].Strlen(), '|', parameters, parameterCount);
    for (unsigned i = 0; i < parameterCount; ++i) {
        cRZString key(parameters[i]);
        cRZString value;
        const std::size_t equals = key.FindChars("=", 0, true);
        if (equals != std::string::npos && equals > 0) {
            value = key.Mid(equals + 1, std::string::npos);
            key = key.Left(equals);
        }
        key.MakeLower();
        value.MakeLower();
        const std::string k(key.ToChar()), v(value.ToChar());
        if (k == "bold") spec.styleFlags |= 1;
        else if (k == "italic") spec.styleFlags |= 2;
        else if (k == "under" || k == "underline") spec.styleFlags |= 4;
        else if (k == "strikethrough" || k == "strike") spec.styleFlags |= 8;
        else if (k == "shadow") spec.styleFlags |= 0x10;
        else if (k == "aa") {
            if (v == "none") spec.antialiasing = 1;
            else if (v == "color") spec.antialiasing = 2;
            else if (v == "bg" || v == "background") spec.antialiasing = 3;
        } else if (k == "color" || k == "bgcolor") {
            cRZString rgb[3];
            unsigned rgbCount = 3;
            RZParseDelimitedText(value.ToChar(), value.Strlen(), ',', rgb, rgbCount);
            auto channel = [&](int i) { return static_cast<std::uint8_t>(rgb[i].Strlen() ? std::atoi(rgb[i].ToChar()) : 0); };
            const std::uint32_t color = nativeColor(channel(0), channel(1), channel(2));
            (k == "color" ? spec.textColor : spec.backgroundColor) = color;
            spec.colorsGiven = true;
        } else if (value.Strlen() == 0) {
            continue;
        } else if (k == "gamma") spec.gamma = static_cast<float>(RZAtofEnglishA(v.c_str()));
        else if (k == "sharpness") spec.sharpness = static_cast<float>(RZAtofEnglishA(v.c_str()));
        else if (k == "xscale") spec.scaleX = static_cast<float>(RZAtofEnglishA(v.c_str()));
        else if (k == "yscale") spec.scaleY = static_cast<float>(RZAtofEnglishA(v.c_str()));
        else if (k == "linespacing") spec.lineSpacing = std::atoi(v.c_str());
        else if (k == "charspacing") spec.charSpacing = std::atoi(v.c_str());
        else if (k == "xadvancescale") spec.advanceScaleX = static_cast<float>(RZAtofEnglishA(v.c_str()));
    }
    spec.styleId = RZAtoUint32HexA(fields[3].ToChar());
    return spec;
}

}  // namespace sc4

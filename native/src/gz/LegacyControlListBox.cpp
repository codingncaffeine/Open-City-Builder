// cGZLegacyControl's list box interpreter (apart from LegacyControl.cpp because gz/WinListBox.h declares its own types).
#include "gz/LegacyControl.h"

#include <cstdio>

#include "gz/WinFont.h"
#include "gz/WinListBox.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"

// InterpretPropertiesGZWinListBox (Mac i386 0x003b5126). The init pass sets the scroll bars, font, the three text colours,
// the highlight colour, outline drawing and colours, the sunken rect, the scroll bar and text gutters and the drop-down
// flag; the second pass adds the script's items (0x502 strings), sorts (0x503), selects (0x501), scrolls (0x505 / 0x506)
// and records the autofit parameter 0xdacf8f9c (with the resizes when the script asks for autofit, 0x304).
void cGZLegacyControl::InterpretPropertiesGZWinListBox(cIGZWin* window, tScriptPropertyArray& parameters, bool init) {
    if (!window) return;
    gz::cIGZWinListBox* list = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinListBox, reinterpret_cast<void**>(&list)) || !list) return;
    auto native = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return 0xff000000u | std::uint32_t(r) << 16 | std::uint32_t(g) << 8 | b;
    };
    if (init) {
        std::uint32_t value = 0;
        const bool hasVertical = GetPropertyUint32(parameters, 0x402, value);
        const bool vertical = !hasVertical || value == 1;
        const bool hasHorizontal = GetPropertyUint32(parameters, 0x401, value);
        list->EnableScrollBar(vertical, hasHorizontal && value == 1);
        std::uint32_t styleId = 0;
        cRZString fontName;
        if (GetPropertyUint32(parameters, 0xf000, styleId)) {
            if (gz::IWinFontSource* fonts = gz::WinFontSource()) list->SetFont(fonts->Font(styleId));
            SetGZWinFont(window, styleId, 0);
        } else if (GetPropertyString(parameters, 0xf000, fontName)) {
            SetGZWinFont(window, fontName, 0);
        }
        std::uint8_t r = 0, g = 0, b = 0;
        if (GetPropertyColor(parameters, 0xf00f, r, g, b)) list->SetFontColor(gz::cIGZWinListBox::kColorText, native(r, g, b));
        if (GetPropertyColor(parameters, 0xf010, r, g, b)) list->SetFontColor(gz::cIGZWinListBox::kColorDisabled, native(r, g, b));
        if (GetPropertyColor(parameters, 0xf011, r, g, b)) list->SetFontColor(gz::cIGZWinListBox::kColorSelected, native(r, g, b));
        if (GetPropertyColor(parameters, 0x408, r, g, b)) list->SetHighlightColor(native(r, g, b));
        if (GetPropertyUint32(parameters, 0xf019, value)) list->SetOutlineDrawing(value == 1);
        cRZString outline;
        int c[6] = {};
        if (GetPropertyString(parameters, 0x40b, outline) &&
            std::sscanf(outline.ToChar(), "(%d,%d,%d)(%d,%d,%d)", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 6) {
            list->SetOutlineColors(native(std::uint8_t(c[0]), std::uint8_t(c[1]), std::uint8_t(c[2])),
                                   native(std::uint8_t(c[3]), std::uint8_t(c[4]), std::uint8_t(c[5])));
        }
        if (GetPropertyUint32(parameters, 0xf00b, value)) list->SetDrawSunkenRect(value == 1);
        int x = 0, y = 0;
        if (GetPropertyCoords(parameters, 0x500, x, y)) list->SetScrollbarGutters(std::int8_t(x), std::int8_t(y));
        if (GetPropertyCoords(parameters, 0xf004, x, y)) list->SetTextGutters(std::int8_t(x), std::int8_t(y));
        if (GetPropertyUint32(parameters, 0x504, value)) list->SetDrop(value == 1);
    } else {
        for (const tScriptProperty& property : parameters) {
            if (property.id != 0x502 || !property.value) continue;
            if (property.value->GetType() != (cIGZVariant::kTypeRZChar | cIGZVariant::kTypeArrayFlag)) continue;
            cRZString item;
            property.value->GetValString(item);
            list->InsertString(item, -2, 0);
        }
        std::uint32_t value = 0;
        if (GetPropertyUint32(parameters, 0x503, value) && value == 1) {
            list->SetShouldSort();
            list->Sort();
        }
        if (GetPropertyUint32(parameters, 0x501, value)) list->SetSelection(int(value), true);
        if (GetPropertyUint32(parameters, 0x505, value)) list->PutIndexInView(int(value));
        if (GetPropertyUint32(parameters, 0x506, value)) list->SetIndexOfTopLine(int(value));
        bool autofit = false;
        for (const tScriptProperty& property : parameters)
            if (property.id == 0x304) { autofit = true; break; }
        auto* autofitParam = new cRZVariant();
        autofitParam->SetValBool(autofit);
        window->SetParam(0xdacf8f9cu, autofitParam);
        if (autofit) {
            list->ResizeWindowForExactLineHeights();
            list->ResizeBoxToFitContents();
            list->ResizeBoxToFitContentsHorizontal();
        }
    }
    list->Release();
}

// cGZLegacyControl's grid interpreter, apart from LegacyControl.cpp because gz/WinGrid.h declares its own gz::cIGZBuffer.
#include "gz/LegacyControl.h"

#include "gz/WinFont.h"
#include "gz/WinGrid.h"
#include "platform/Unwired.h"
#include "rz/cRZString.h"

// InterpretPropertiesGZWinGrid (Mac i386 0x003b6868): the grid's colours, font, alignment, wrapping, gutters, selection
// rules, outline, default sizes, headers and its 23 flag keywords (0x160c..0x1622: one flag bit each, fdefault = 0x19bff,
// fall = every bit). PARTIAL: the cell / row / column data lists (wingridcell / wingridrow / wingridcol: per-cell text,
// bitmaps and sizes kept as window parameter 0x287a7136) are not read yet.
void cGZLegacyControl::InterpretPropertiesGZWinGrid(cIGZWin* window, tScriptPropertyArray& parameters, bool init) {
    if (!window) return;
    gz::cIGZWinGrid* grid = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinGrid, reinterpret_cast<void**>(&grid)) || !grid) return;
    for (const tScriptProperty& property : parameters) {
        if (property.id >= 0x1625 && property.id <= 0x1627) {
            SC4_UNWIRED("cGZLegacyControl::InterpretPropertiesGZWinGrid: the wingridcell/row/col data lists (window parameter 0x287a7136) are not read");
            break;
        }
    }
    auto native = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return 0xff000000u | std::uint32_t(r) << 16 | std::uint32_t(g) << 8 | b;
    };
    std::uint8_t r = 0, g = 0, b = 0;
    if (!init) {
        std::uint8_t r2 = 0, g2 = 0, b2 = 0;
        GetPropertyColor(parameters, 0x1623, r, g, b);
        GetPropertyColor(parameters, 0x1624, r2, g2, b2);
        grid->SetGridColors(static_cast<int>(native(r, g, b)), static_cast<int>(native(r2, g2, b2)));
    }
    if (GetPropertyColor(parameters, 0x1601, r, g, b)) grid->SetDefaultFontColor(native(r, g, b), 0);
    std::uint32_t styleId = 0;
    cRZString fontName;
    gz::IWinFontSource* fonts = gz::WinFontSource();
    if (GetPropertyUint32(parameters, 0xf000, styleId)) {
        if (fonts) grid->SetDefaultFont(fonts->Font(styleId));
        SetGZWinFont(window, styleId, 0);
    } else if (GetPropertyString(parameters, 0xf000, fontName)) {
        if (fonts) if (gz::IWinFont* font = fonts->FontByName(fontName.ToChar())) grid->SetDefaultFont(font);
        SetGZWinFont(window, fontName, 0);
    } else if (fonts) {
        grid->SetDefaultFont(fonts->Font(0x68963c4c));
    }
    int alignment = 0;
    if (GetPropertySint32(parameters, 0x1602, alignment)) grid->SetDefaultTextAlignment(alignment);
    std::uint32_t wrap = 0;
    grid->SetDefaultTextWrapping(GetPropertyUint32(parameters, 0x1603, wrap) && wrap != 0);
    cRZRect gutters{0, 0, 0, 0};
    if (GetPropertyRect(parameters, 0xf004, gutters)) grid->SetGutters(gutters.left, gutters.top, gutters.right, gutters.bottom);
    else grid->SetGutters(0, 0, 0, 0);
    int selectionRule = 2, selectionLimit = 1;
    GetPropertySint32(parameters, 0x1605, selectionRule);
    GetPropertySint32(parameters, 0x1606, selectionLimit);
    grid->SetSelectionRules(selectionRule, selectionLimit);
    std::uint32_t outline = 0;
    GetPropertyUint32(parameters, 0x1616, outline);
    r = g = b = 0;
    GetPropertyColor(parameters, 0x1604, r, g, b);
    grid->SetOutlineUse(outline == 1, native(r, g, b));
    int columnWidth = 100, rowHeight = 0x14;
    GetPropertySint32(parameters, 0x1607, columnWidth);
    grid->SetDefaultColumnWidth(columnWidth);
    GetPropertySint32(parameters, 0x1608, rowHeight);
    grid->SetDefaultRowHeight(rowHeight);
    std::uint32_t headers = 0;
    GetPropertyUint32(parameters, 0x1609, headers);
    grid->EnableHeaders(headers == 1, headers == 2);
    int columnHeader = 0, rowHeader = 0;
    GetPropertySint32(parameters, 0x160a, columnHeader);
    GetPropertySint32(parameters, 0x160b, rowHeader);
    grid->SetHeaderSizes(columnHeader, rowHeader);
    static constexpr struct { std::uint32_t property; std::uint32_t flag; } kFlags[] = {
        {0x160c, 0x0}, {0x160d, 0x1}, {0x160e, 0x2}, {0x160f, 0x4}, {0x1610, 0x8}, {0x1611, 0x10}, {0x1612, 0x20},
        {0x1613, 0x40}, {0x1614, 0x80}, {0x1615, 0x100}, {0x1616, 0x200}, {0x1617, 0x400}, {0x1618, 0x800},
        {0x1619, 0x1000}, {0x161a, 0x2000}, {0x161b, 0x4000}, {0x161c, 0x8000}, {0x161d, 0x10000}, {0x161e, 0x20000},
        {0x161f, 0x40000}, {0x1620, 0x80000}, {0x1621, 0x19bff}, {0x1622, 0xffffffffu}};
    for (const auto& f : kFlags) {
        int value = 0;
        if (!GetPropertySint32(parameters, f.property, value)) continue;
        if (value == 0) grid->ClearFlag(static_cast<int>(f.flag));
        else grid->SetFlag(static_cast<int>(f.flag));
    }
    grid->Release();
}

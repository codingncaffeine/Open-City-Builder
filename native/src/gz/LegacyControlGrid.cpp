// cGZLegacyControl's grid interpreter, apart from LegacyControl.cpp because gz/WinGrid.h declares its own gz::cIGZBuffer.
#include "gz/LegacyControl.h"

#include "gz/WinFont.h"
#include "gz/WinGrid.h"
#include "gz/WinBufferOfGZBuffer.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinCtrlMgr.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "rz/cRZString.h"

// InterpretPropertiesGZWinGrid (Mac i386 0x003b6868): the grid's colours, font, alignment, wrapping, gutters, selection
// rules, outline, default sizes, headers and its 23 flag keywords (0x160c..0x1622: one flag bit each, fdefault = 0x19bff,
// fall = every bit), then the cell / column / row lists (wingridcell / wingridcol / wingridrow). The original also keeps
// each list as a window parameter (0x287a7136 / 0xc88ccb6d / 0xc88ccb6e) for unscript, the UI editor's writer; the game reads
// none of them, so the port applies the lists and keeps no copy.
void cGZLegacyControl::InterpretPropertiesGZWinGrid(cIGZWin* window, tScriptPropertyArray& parameters, bool init) {
    if (!window) return;
    gz::cIGZWinGrid* grid = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinGrid, reinterpret_cast<void**>(&grid)) || !grid) return;
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
    auto text = [](const tScriptProperty& property, cRZString& out) {
        return property.value && property.value->GetType() == (cIGZVariant::kTypeRZChar | cIGZVariant::kTypeArrayFlag) &&
               property.value->GetValString(out);
    };
    // wingridcell "column,row,flags,..." (i386 0x003b72d5): flags 0x40000 = the text itself in field 3, else a string or picture
    // key "group,instance" in fields 3/4 (0x2000 a picture, 0x10000 a localised string); 0x80000 makes the text editable;
    // 0x1000 adds a cell format from the next six fields (font name, colour, horizontal / vertical alignment, border, border
    // colour; 0x100000 sets its flag to 1, else 0xff).
    for (const tScriptProperty& property : parameters) {
        cRZString value;
        if (property.id != 0x1625 || !text(property, value)) continue;
        std::vector<std::string> fields;
        {
            std::string all(value.ToChar(), value.Strlen()), field;
            for (char c : all) {
                if (c == ',') { fields.push_back(field); field.clear(); if (fields.size() == 10) break; }
                else field += c;
            }
            if (fields.size() < 11) fields.push_back(field);
        }
        if (fields.size() < 4) continue;
        const int column = static_cast<int>(std::strtoul(fields[0].c_str(), nullptr, 10));
        const int row = static_cast<int>(std::strtoul(fields[1].c_str(), nullptr, 10));
        const std::uint32_t flags = static_cast<std::uint32_t>(std::strtoul(fields[2].c_str(), nullptr, 16));
        const bool literal = (flags & 0x40000) != 0;
        const std::size_t formatAt = literal ? 4 : 5;
        const bool editable = (flags & 0x80000) != 0;
        if (literal) {
            const std::string& t = fields[3];
            grid->SetCellTextValue(column, row, t.c_str(), static_cast<std::uint32_t>(t.size()), 1, editable);
        } else if (fields.size() > 4) {
            unsigned group = 0, instance = 0;
            const std::string key = fields[3] + "," + fields[4];
            if (std::sscanf(key.c_str(), "%08x,%08x", &group, &instance) == 2) {
                if ((flags & 0x2000) != 0) {
                    if (cIGZBuffer* image = LoadBuffer(group, instance)) {
                        auto* bitmap = new WinBufferOfGZBuffer(image);
                        grid->SetCellBitmap(column, row, bitmap, 1);
                        bitmap->Release();
                        image->Release();
                    }
                } else if ((flags & 0x10000) != 0) {
                    cRZString loaded;
                    if (LoadStringA(group, instance, loaded))
                        grid->SetCellTextValue(column, row, loaded.ToChar(), static_cast<std::uint32_t>(loaded.Strlen()), 1, editable);
                }
            }
        }
        if ((flags & 0x1000) != 0 && fields.size() >= formatAt + 6) {
            gz::cIGZWinGrid::CellFormat format;
            gz::cIGZWinCtrlMgr* controls = gz::WinCtrlMgr();
            const cRZString fontName(fields[formatAt].c_str());
            const int fontId = controls ? controls->GetFontMappingID(&fontName) : 0;
            if (fontId != 0 && controls) format.font = controls->SystemFont(fontId);
            format.colors[0] = static_cast<std::uint32_t>(std::strtoul(fields[formatAt + 1].c_str(), nullptr, 10));
            format.halign = static_cast<std::uint8_t>(std::strtoul(fields[formatAt + 2].c_str(), nullptr, 10));
            format.valign = static_cast<std::uint8_t>(std::strtoul(fields[formatAt + 3].c_str(), nullptr, 10));
            format.border = static_cast<std::uint8_t>(std::strtoul(fields[formatAt + 4].c_str(), nullptr, 10));
            format.borderColor = static_cast<std::uint32_t>(std::strtoul(fields[formatAt + 5].c_str(), nullptr, 10));
            format.flag = (flags & 0x100000) != 0 ? 1 : 0xff;
            grid->SetCellFormat(column, row, format);
        }
    }
    // wingridcol "first,last,width" (i386 0x003b7a81): SetColumnWidth(first, 1, width) - one column, whatever `last` says.
    for (const tScriptProperty& property : parameters) {
        cRZString value;
        if (property.id != 0x1626 || !text(property, value)) continue;
        int first = static_cast<int>(0xdeadbeef), last = static_cast<int>(0xdeadbeef), width = 0;
        if (std::sscanf(value.ToChar(), "%d,%d,%d", &first, &last, &width) == 3) {
            grid->SetColumnWidth(first, 1, width);
            if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "grid %08x: column %d width %d -> %d\n", window->GetID(), first, width, grid->GetColumnWidth(first, 1));
        }
    }
    // wingridrow "first,last,height" (i386 0x003b7c7f): SetRowHeight(first, last - first + 1, height).
    for (const tScriptProperty& property : parameters) {
        cRZString value;
        if (property.id != 0x1627 || !text(property, value)) continue;
        int first = static_cast<int>(0xdeadbeef), last = static_cast<int>(0xdeadbeef), height = 0;
        if (std::sscanf(value.ToChar(), "%d,%d,%d", &first, &last, &height) == 3) grid->SetRowHeight(first, last - first + 1, height);
    }
    grid->Release();
}

// gz::MakeWinFont: the platform's window font maker (gz/WinFont.h).
#include "gz/WinFont.h"

namespace gz {

namespace {
WinFontMaker g_fontMaker = nullptr;
}

IWinFont* MakeWinFont(cIGZFont* font) { return font && g_fontMaker ? g_fontMaker(font) : nullptr; }
void SetWinFontMaker(WinFontMaker maker) { g_fontMaker = maker; }

}  // namespace gz

// The glyph texture manager service on the native application (NativeGlyphTextures.cpp).
#pragma once
#include <cstdint>

class cIGZUnknown;
namespace sc4 { class cSC4GlyphTextureManager; }

namespace native {
cIGZUnknown* CreateGlyphTextureManager();              // the boot's service factory for "cSC4GlyphTextureManager"
sc4::cSC4GlyphTextureManager* GlyphTextureManager();   // null before the application created it
void TickGlyphTextures(std::uint32_t timeMs);          // the framework tick: the string cache's sweep
}  // namespace native

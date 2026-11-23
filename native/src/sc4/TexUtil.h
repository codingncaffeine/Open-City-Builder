// nsTexUtil — a GZ buffer turned into a driver texture (Mac i386 0x0023ae84..0x0023b1ee). Used by the dispatch indicators'
// call numbers and pictures, the dispatch call box and the renderer's background picture.
#pragma once
#include <cstdint>

class cIGZBuffer;
struct cRZRect;
namespace gd { class cIGZGDriver; }

namespace nsTexUtil {

int GetNearestPowerOfTwo(int value);                                      // i386 0x0023ae84: 1 for value <= 1
void ReleaseTexture(gd::cIGZGDriver* driver, std::uint32_t texture);      // i386 0x0023ae9a: DeleteTextures(1, &texture)
// i386 0x0023aec4. A 32-bit buffer whose area (or `rect` of it) is already a square power of two is loaded as is: the
// driver makes a texture of that size (internal format 4 with `alpha`, else 1; one level; hint 2) and level 0 is loaded
// from the buffer's bits (offset to the rect's corner), `width` x `width` with the buffer's width as the row length.
// Anything else is first copied into a new ARGB8888 buffer of the next square power of two (cleared to 0, the source
// blitted at the corner) and that one is set up instead. `outWidth` gets the used width, `outSize` the texture's size.
// False when the buffer cannot be locked for reading or the copy cannot be made.
bool SetupTexture(cIGZBuffer* buffer, gd::cIGZGDriver* driver, std::uint32_t& texture, bool alpha, const cRZRect* rect,
                  std::uint32_t* outWidth, std::uint32_t* outSize);

}  // namespace nsTexUtil

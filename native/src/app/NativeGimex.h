// The port's image codecs (PNG, BMP) behind the Gimex reader and writer classes; see NativeGimex.cpp.
#pragma once

namespace native {
// Installs the codecs as the Gimex library and registers the reader / writer classes (0x2a316c02 / 0xca316c55).
void InstallImageCodecs();
}  // namespace native

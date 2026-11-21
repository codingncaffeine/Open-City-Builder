// cSLCursor (Mac i386 0x0002c81e..0x0002caec): a game cursor from the resources. The cursor record (type 0xaa5c3144, group =
// the cursor bit depth, instance = the cursor id) is written to a temporary .cur file and loaded with cRZCursor.
#pragma once
#include "gz/WinInterfaces.h"
#include "gz/cGZPersistResourceKey.h"

namespace sc4 {

constexpr std::uint32_t kCursorResourceType = 0xaa5c3144;

// cSLCursor(key) + its cursor: one reference, or null when the record is missing or not a loadable cursor. A key of
// type 0 means the cursor type.
cIGZCursor* LoadSLCursor(const cGZPersistResourceKey& key);

}  // namespace sc4

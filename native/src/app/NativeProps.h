// The prop manager's and the props' native hosts (NativeProps.cpp).
#pragma once

namespace native {
void InstallPropHosts();   // before the city's components are created
}  // namespace native
#include "gz/cGZPersistResourceKey.h"
namespace native {
void* NativeCreateFadingDecal(const cGZPersistResourceKey& key, float size, float fadeIn, const float position[3]);   // app/NativeProps.cpp
void NativeRetireFadingDecal(void* decal, float fadeOut, int completionAction);
}  // namespace native

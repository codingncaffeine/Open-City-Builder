// The occupant animation coordinator and the model maker's animated models on the native application:
// NativeAnimationCoordinator.cpp.
#pragma once

#include "gz/cGZPersistResourceKey.h"

namespace s3d { class cIS3DModelInstanceAnimated; }
namespace sc4 { class IModelAnimationCoordinator; }

namespace native {

sc4::IModelAnimationCoordinator* NativeCreateAnimationCoordinator();                  // one reference, not initialised
s3d::cIS3DModelInstanceAnimated* NativeCreateAnimatedModel(const cGZPersistResourceKey& key);   // one reference or null
void NativeAnimationReport(int limit);   // the kit's `animated [N]`

}  // namespace native

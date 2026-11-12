// The disaster menu's tools in a native city (app/NativeDisasterTools.cpp) and what they ask the disaster classes
// (app/NativeDisasters.cpp; the two sides cannot share a translation unit: sc4/Disaster.h and sc4/VisualEffect.h both declare
// cISC4VisualEffect).
#pragma once

#include <cstdint>

class cIGZUnknown;
namespace sc4 { class cSC4View3DWin; }

namespace native {

// A visual effect as the disaster classes use it.
class IDisasterEffectFace {
public:
    virtual ~IDisasterEffectFace() = default;
    virtual void Retain() = 0;
    virtual void Drop() = 0;
    virtual void Start(int transition) = 0;
    virtual void Stop(int transition) = 0;
    virtual bool IsPlaying() = 0;
    virtual void SetCompletionMessage(std::uint32_t message, std::uint32_t data) = 0;
    virtual void SetIntParameter(int parameter, const void* data, int count) = 0;
    virtual void SetPointerParameter(int parameter, const void* data, int count) = 0;
};

// ---- app/NativeDisasters.cpp
bool NativeHasDisasterManager(std::uint32_t id);
void NativeAttemptDisasterStart(std::uint32_t id, int x, int z);
bool NativeTorchIgnite(int cellX, int cellZ, bool again, bool force, std::uint32_t value);
void NativeSetFireCause(std::uint32_t cause);
// The manager's StartDisasterEffect: the instance with one reference, null when the manager runs no effects. The face is
// deleted with the instance's last reference to the effect.
cIGZUnknown* NativeStartDisasterEffect(std::uint32_t managerId, IDisasterEffectFace* effect, const float position[3], std::uint32_t id);
void NativeStopDisasterEffect(cIGZUnknown* instance);

// ---- app/NativeDisasterTools.cpp
void InstallDisasterTools();
// A named effect placed at a world point and started: the effect with one reference (an opaque handle), null when there is none.
void* NativeCreateEffectAtPoint(const char* name, const float xyz[3]);
void NativeMoveEffectTo(void* effect, const float xyz[3]);
void NativeReleaseEffectHandle(void* effect);
// cSC4View3DWin::InvokeDisaster: the menu item's tool becomes the view's current one.
void InvokeDisasterTool(sc4::cSC4View3DWin& view, std::uint32_t id);
// Kit command `disasters`: every disaster exemplar with its effect and manager.
void NativeDisasterMenuReport();

}  // namespace native

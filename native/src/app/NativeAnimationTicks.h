// The animation tick manager (SL::ATM()) on the native application: NativeAnimationTicks.cpp.
#pragma once

#include <cstdint>

class cIGZMessageServer2;
namespace sc4 { class cSC4AnimationTickManager; }

namespace native {

sc4::cSC4AnimationTickManager* AnimationTickManager();   // created on first use (GZCOM 0xa9c73857), not initialised
bool StartAnimationTicks();                               // Init: joins the framework (SL::ATM()), creates the tick message
void StopAnimationTicks();                                // Shutdown: releases every tickable object
void NoteFramePresented();                               // one frame went to the screen (the SC4_SIMDEBUG pace line counts them)
std::uint32_t TickAnimation();                            // DoAnimationTick (the region screen's GZPaint, the city frame)
cIGZMessageServer2* AnimationTickServerView();            // the manager seen as a message server (AddNotification = AddTickableObject)

}  // namespace native

// cSC4AdvisorUIImageWinProc — the advisor's animated head in an advisor page (the popup's image window, child 4, and the
// advisor window's pictures). 16 functions in the Mac i386 build, 0x0021eaf8..0x0021fb56 (decompiles in
// scratch/s50/ghidra/imageproc.c); object 0xec bytes in i386.
//
// It is the window procedure of a gen window. On attach (Init) it takes the window's absolute area and the main window's
// size and builds an orthographic camera over the window (0..w, 0..h, -400..400, turned half a circle about Y so the head
// faces the viewer). Enabled, it registers with the animation tick manager (priority 0xc0) and loads two animated head
// models of its advisor for the current mood:
//   A = advisor->GetHeadAnimID(kMoodToModelA[mood]) (the talking loop), B = GetHeadAnimID(kMoodToModelB[mood]),
//   resource keys {0x5ad0e817, 0xbadb57f1, id} (id + 0x100 with the llama cheat on, when that model exists), both
//   self-illuminated; A plays first.
// Every animation tick advances the playing model; each time it wraps to frame 0 the head either restarts A and picks a
// random number of loops in [1, 3) (the first wrap after a B), or counts a loop and switches to B when the count is reached.
// After the window paints (kWinProcPaintAfter) the head is drawn over it: the renderer's draw context with this camera,
// the driver viewport on the window (minus the right-hand offset), a cleared depth buffer, the head placed at
// (-w/2, -offset, 0) and scaled to 0.85 of the window height, then the window asks to be painted again.
//
// Moods (ForceMood): the advice's mood is mapped 0 -> 3, 1 -> 2, 3 -> 0, 5 -> 1, anything else unchanged; a new mood
// restarts the loop count and reloads the models.
#pragma once
#include <cstdint>

#include "gz/WinProc.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZRandom.h"
#include "s3d/Camera.h"
#include "sc4/AdvisorPopupTracker.h"

struct cGZPersistResourceKey;
class SC4DrawContext;
class cISC43DRender;
namespace gd { class cIGZGDriver; }
namespace s3d { class cIS3DModelInstanceAnimated; }

namespace sc4 {

class cISC4Advisor;

constexpr std::uint32_t kAdvisorHeadModelType = 0x5ad0e817;
constexpr std::uint32_t kAdvisorHeadModelGroup = 0xbadb57f1;
constexpr std::uint32_t kAdvisorHeadLlamaOffset = 0x100;      // the llama cheat's heads
constexpr std::uint32_t kAdvisorHeadTickPriority = 0xc0;
constexpr std::uint32_t kAdvisorHeadTickMessage = 0x86ad10ee;  // the animation tick (float milliseconds in data3)
constexpr float kAdvisorHeadScale = 0.85f;                    // of the window height
constexpr std::uint32_t kAdvisorHeadClearMask = 0x1000;       // the driver's depth-buffer clear bit
constexpr int kAdvisorHeadLoopsMin = 1, kAdvisorHeadLoopsMax = 3;   // RandomSint32RangeUniform(1, 3)

// Everything the head reaches outside itself (one method per original global / engine call).
class IAdvisorUIImageHost {
public:
    virtual ~IAdvisorUIImageHost() = default;
    // SL::spCity vslot 0x144 (the advisor system) -> vslot 0x1c GetAdvisor(id); borrowed, null without a city.
    virtual cISC4Advisor* Advisor(int advisorId) = 0;
    // cSC4AdvisorSystem::mbLlamaCheatActive.
    virtual bool LlamaCheatActive() = 0;
    // SL::spRM GetResource(key, 0x9ad0e7e3) -> resource slot 5 (CreateAnimatedModelInstance): one reference, null when the
    // resource is missing.
    virtual s3d::cIS3DModelInstanceAnimated* CreateModel(const cGZPersistResourceKey& key) = 0;
    // SL::ATM() vslots 0x14 / 0x18 (AddTickableObject / RemoveTickableObject).
    virtual void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;
    virtual void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;
    // RZ::WM()->GetMainWindow() GetW / GetH.
    virtual bool MainWindowSize(std::uint16_t& width, std::uint16_t& height) = 0;
    // SL::spRenderer (borrowed; null in a headless run) and SL::spGDriver.
    virtual cISC43DRender* Renderer() = 0;
    virtual gd::cIGZGDriver* Driver() = 0;
};
IAdvisorUIImageHost* AdvisorUIImageHost();
void SetAdvisorUIImageHost(IAdvisorUIImageHost* host);

class cSC4AdvisorUIImageWinProc : public IAdvisorUIImageWinProc, public cIGZMessageTarget2 {
public:
    explicit cSC4AdvisorUIImageWinProc(int advisorId);   // @ 0x0021f65e: reference count 0
    virtual ~cSC4AdvisorUIImageWinProc();                // @ 0x0021f97a

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x0021eaf8
    std::uint32_t AddRef() override;                               // @ 0x0021eb3a
    std::uint32_t Release() override;                              // @ 0x0021eb4c
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // @ 0x0021eec0
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x0021ec84

    void Enable(bool enable) override;                             // @ 0x0021ec00
    void ForceMood(int mood) override;                             // @ 0x0021ece8
    virtual void SetMood(int mood) { ForceMood(mood); }            // @ 0x0021ecda
    virtual void SetOffset(int offset) { offset_ = offset; }       // @ 0x0021ecc8

    bool Init(cIGZWin* window);                                    // @ 0x0021ed48
    bool Shutdown();                                               // @ 0x0021eb72
    void PlaceHead(float x, float y, float z, float scale);        // @ 0x0021ef54
    void LoadModels(bool load);                                    // @ 0x0021f2f8
    void DoPaint();                                                // @ 0x0021effc
    void DoAnimationTick(float milliseconds);                      // @ 0x0021f1d2

    // Harness views.
    int Mood() const { return mood_; }
    bool Enabled() const { return enabled_; }
    std::uint32_t ModelIdA() const { return modelIdA_; }
    std::uint32_t ModelIdB() const { return modelIdB_; }
    s3d::cIS3DModelInstanceAnimated* Current() const { return current_.get(); }
    s3d::cIS3DModelInstanceAnimated* ModelA() const { return modelA_.get(); }
    s3d::cIS3DModelInstanceAnimated* ModelB() const { return modelB_.get(); }
    int LoopCount() const { return loopCount_; }
    int LoopTarget() const { return loopTarget_; }
    s3d::cS3DCamera* Camera() const { return camera_; }

private:
    void DropCamera();
    std::uint32_t refCount_ = 0;                                   // +0x08
    bool initialized_ = false;                                     // +0x0c
    bool enabled_ = false;                                         // +0x0d
    int mood_ = 0;                                                 // +0x10
    std::uint32_t modelIdA_ = 0;                                   // +0x14
    cRZAutoRefCount<s3d::cIS3DModelInstanceAnimated> modelA_;     // +0x18
    std::uint32_t modelIdB_ = 0;                                   // +0x1c
    cRZAutoRefCount<s3d::cIS3DModelInstanceAnimated> modelB_;     // +0x20
    cRZAutoRefCount<s3d::cIS3DModelInstanceAnimated> current_;    // +0x24
    cRZAutoRefCount<cIGZWin> window_;                              // +0x28
    cRZRect area_{};                                               // +0x2c (absolute)
    std::uint16_t mainWidth_ = 0, mainHeight_ = 0;                 // +0x3c / +0x3e
    s3d::cS3DCamera* camera_ = nullptr;                            // +0x40 (counted; its Release does not delete)
    int advisorId_;                                                // +0x44
    float projection_[4][4] = {};                                  // +0x48 the camera's projection, transposed (never read)
    [[maybe_unused]] float unused88_[16] = {};                     // +0x88 (zeroed, never read)
    int lastFrame_ = 0;                                            // +0xc8
    int loopCount_ = 0;                                            // +0xcc
    int loopTarget_ = 0;                                           // +0xd0
    cRZRandom random_{0xffffffffu};                                // +0xd4
    int offset_ = 0;                                               // +0xe8 pixels left free at the right / top
};

}  // namespace sc4

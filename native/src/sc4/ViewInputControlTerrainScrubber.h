// cSC4ViewInputControlTerrainScrubber — the god-mode terrain brush tool (raise, lower, smooth, level, the terrain
// texture and erosion brushes; Mac i386 0x002b4454..0x002b5aba, 22 functions). Holding the left button applies the
// brush along the drag: every mouse move and every third animation tick queue a delayed action (from the last point
// to the current one), and ProcessDelayedActions applies the brush once for all the actions that are due, its
// strength growing with the time since the last application (up to twice the base). A brush with a cost withdraws
// it from the budget per application and shows the running total in the cost indicator; with no money it stops.
//
// Object layout of the original (i386):
//   +0x2c brush type   +0x30 the brush's UI elements (GetUIElementsForBrush)   +0x58 tick count since the last push
//   +0x5c terrain (referenced)   +0x60 terrain view (referenced)   +0x64 scrubbing   +0x65 cursor over the view
//   +0x68 last point (x, z)   +0x70 window manager   +0x74 tBrushParameters (0x38 bytes)   +0xac moved since the tick
//   +0xb0 applications since the texture flush   +0xb4 free brush (no cost)   +0xb8 running cost (int64)
//   +0xc0 cRZTimer(us) since the last application   +0xd4 first application   +0xdc / +0xe0 strength / width
//   +0xe4 / +0xe8 the cursor echo effects   +0xec echo position   +0x100 cRZTimer(ms) of the drag
//   +0x114 action delay (ms)   +0x118 anchor altitude   +0x11c the delayed actions
//
// Everything outside the object goes through ITerrainScrubberHost.
#pragma once
#include <cstdint>
#include <list>

#include "gz/cIGZMessage2.h"
#include "rz/cRZTimer.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControl.h"
#include "sc4/cSTETerrain.h"

namespace sc4 {

constexpr std::uint32_t kTerrainScrubberToolId = 0x68e29401;
constexpr std::uint32_t kTerrainScrubberCursorId = 0x071e5703;
constexpr std::uint32_t kTerrainScrubberMsgTick = 0x86ad10ee;       // the animation timer, every 0xc0 ms while scrubbing
constexpr std::uint32_t kTerrainScrubberTickInterval = 0xc0;
constexpr std::uint32_t kTerrainScrubberMsgViewChanged[2] = {0xa6b79602, 0xa6b79612};   // the camera moved or zoomed

class ITerrainScrubberHost {
public:
    virtual ~ITerrainScrubberHost() = default;
    virtual cISC4View3DWin* View() = 0;                                   // SL::spView3D
    virtual cSTETerrain* Terrain() = 0;                                   // SL::spTerrain
    virtual bool BudgetPresent() = 0;                                     // SL::spBudgetSim != null
    virtual std::int64_t Funds() = 0;                                     // budget vslot 0x18 GetTotalFunds
    virtual bool WithdrawFunds(std::int64_t amount) = 0;                  // budget vslot 0x20
    virtual cIGZMessageServer2* MessageServer() = 0;                      // SL::spMS2
    virtual void StartTick(cIGZMessageTarget2* target, std::uint32_t interval) = 0;   // SL::ATM() vslot 0x14
    virtual void StopTick(cIGZMessageTarget2* target, std::uint32_t interval) = 0;    // SL::ATM() vslot 0x18
    virtual bool CursorPosition(int& x, int& y) = 0;                      // window manager vslot 0x84
    virtual bool CapsLockOn() = 0;                                        // the keyboard's key 0x14 toggle
    // SC4ViewUtilities: the ray under the viewport point meeting the horizontal plane at `altitude`, clipped to the city.
    virtual bool ViewportPointOnPlane(int x, int y, float altitude, float out[3]) = 0;
    // The tuning exemplar 0x6534284a/0x88cd66e9/1 (ReadTuningParameters); null when it is missing.
    virtual cISCPropertyHolder* TuningExemplar() = 0;
};
ITerrainScrubberHost* TerrainScrubberHost();
void SetTerrainScrubberHost(ITerrainScrubberHost* host);

class cSC4ViewInputControlTerrainScrubber final : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    cSC4ViewInputControlTerrainScrubber(std::uint32_t brushType, bool freeBrush);   // 0x002b5846
    ~cSC4ViewInputControlTerrainScrubber() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cSC4ViewInputControl::AddRef(); }
    std::uint32_t Release() override { return cSC4ViewInputControl::Release(); }
    bool Init() override;                                                  // 0x002b52b0
    bool Shutdown() override;                                              // 0x002b47f0
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;   // 0x002b4bea
    bool OnKeyUp(std::uint32_t key, std::uint32_t modifiers) override;     // 0x002b4d58
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;     // 0x002b54d6
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;       // 0x002b51a2
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;      // 0x002b5404
    bool OnMouseExit() override;                                           // 0x002b4bb2
    bool DoMessage(cIGZMessage2* message) override;                        // 0x002b573e

    std::uint32_t BrushType() const { return brushType_; }
    bool Scrubbing() const { return scrubbing_; }

private:
    struct DelayedAction { std::uint32_t due; float fromX, fromZ, toX, toZ; };

    static void StaticInit();                                              // 0x002b4630
    static void ReadTuningParameters();                                    // 0x002b4500
    bool SetCursorAnchor(int x, int y);                                    // 0x002b4454
    bool GetCursorPositionInWorld(int x, int y, float* out);               // 0x002b46da
    void PushDelayedAction(float fromX, float fromZ, float toX, float toZ);   // 0x002b53a8
    void ProcessDelayedActions(bool all);                                  // 0x002b4de4
    void RecordBrushApplication();                                         // 0x002b4674
    void ApplyModifierKeys(std::uint32_t modifiers);                       // the strength / width pick shared by the key and mouse handlers

    std::uint32_t brushType_;                     // +0x2c
    TerrainBrushUIElements ui_{};                 // +0x30
    int ticksSincePush_ = 0;                      // +0x58
    cSTETerrain* terrain_ = nullptr;              // +0x5c
    bool scrubbing_ = false;                      // +0x64
    bool cursorOverView_ = false;                 // +0x65
    float lastX_ = 0.0f, lastZ_ = 0.0f;           // +0x68
    TerrainBrushParameters params_;               // +0x74
    bool movedSinceTick_ = false;                 // +0xac
    std::uint32_t applicationsSinceFlush_ = 0;    // +0xb0
    bool freeBrush_;                              // +0xb4
    std::int64_t runningCost_ = 0;                // +0xb8
    cRZTimer sinceApplication_{cRZTimer::kMicroseconds};   // +0xc0
    bool firstApplication_ = true;                // +0xd4
    float baseStrength_ = 1.0f, baseWidth_ = 1.0f;   // +0xdc / +0xe0 (+0xd8 1.0 unused)
    cRZTimer dragTimer_{cRZTimer::kMilliseconds};  // +0x100
    std::uint32_t actionDelay_ = 0;               // +0x114
    float anchorAltitude_ = 0.0f;                 // +0x118
    std::list<DelayedAction> actions_;            // +0x11c
};

}  // namespace sc4

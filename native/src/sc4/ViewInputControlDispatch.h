// cSC4ViewInputControlDispatch — the emergency dispatch tool (Mac i386 0x002da570..0x002db4b8, 20 functions; dispatch.md 4.5).
// Opening it lays out one call box ("pylon") per free unit of every station of its type (cISC4DispatchManager::BeginDispatchTool);
// a click on the ground sends the nearest free unit there, a click on a call box picks it up and the next click drops it: on its
// own station's lot it goes home (RevokeDispatchByPylon), anywhere else the dispatch manager sends that unit (or re-targets it when
// it is out). A second click within 500 ms on a call box that is out follows its vehicle with the camera. The cursor text counts
// the free units.
//
// Original layout (i386): +0x1c the 3D view (base), +0x28 cIGZMessageTarget2, +0x2c dispatch type, +0x30 "the location is OK",
// +0x34/+0x38 the drag offset (pylon screen position - mouse), +0x3c cRZTimer (units 4), +0x50 the tool's cursor id, +0x54 the
// hovered pylon (counted), +0x58 the cursor text, +0x64 the tool is on top (cursor text shown), +0x68 cPylonFilter (counted),
// +0x6c the picked-up pylon's position, +0x78 its dispatch state (both restored when the tool loses the top).
//
// Quirks kept: OnKeyDown passes no key but Tab (9) on to the view... and answers true for every other key the base would pass
// (0x002da570 returns 1 unless the key is passed, 9 is never passed). The 0x99ef1143 handler dereferences the hovered pylon without
// a null check in the original; the port checks.
#pragma once
#include <cstdint>

#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"
#include "sc4/DispatchManager.h"
#include "sc4/ViewInputControl.h"

namespace sc4 {

constexpr std::uint32_t kDispatchToolCursorTextId = 0x49d02c4d;
constexpr std::uint32_t kDispatchToolHandCursor = 0x13b138d0;
constexpr std::uint32_t kDispatchToolCursorFire = 0x02e43537;     // types 1 and 5
constexpr std::uint32_t kDispatchToolCursorPolice = 0xc2e4353d;   // types 0 and 6
constexpr std::uint32_t kDispatchToolId = 0x68e1900a;
constexpr std::uint32_t kDispatchToolMsgTopChanged = 0x09c8b8ea;  // Void1 = the tool that left the top, Void2 = the new one
constexpr std::uint32_t kDispatchToolMsgFollow = 0x49f10111;      // the camera follows a dispatched vehicle (IGZUnknown = pylon)
constexpr std::uint32_t kDispatchToolMsgPickUp = 0xc9f1011a;      // a call box was picked up

// The tool's services: SL::spDispatchManager, the renderer pick with cPylonFilter, the view's terrain pick and cursor text, the
// city's terrain and cells, SC4ViewUtilities and the vehicle manager's camera follow.
class IDispatchToolHost {
public:
    virtual ~IDispatchToolHost() = default;
    virtual cISC4DispatchManager* DispatchManager() = 0;
    // spRenderer vslot 0x100(x, y, cPylonFilter, &model) -> model owner QI 0xc9b6d7d6: the call box under the mouse, one reference.
    virtual IPylonOccupant* PickPylon(int x, int y) = 0;
    virtual bool PickTerrain(int x, int y, float out[3]) = 0;                         // view vslot 0x90(x, y, out, false)
    virtual float Altitude(float x, float z) = 0;                                    // city 0x150 terrain -> vslot 0x30
    virtual void CellToWorld(int cellX, int cellZ, float& x, float& z) = 0;          // city vslot 0x180
    virtual bool ProjectToViewport(const float world[3], float& x, float& y) = 0;   // SC4ViewUtilities ProjectPointToViewportCoordinates2D
    virtual void SetCursorText(std::uint32_t id, const cRZString& text) = 0;         // view vslot 0xa0(id, 0, 0, text, 0)
    virtual void ClearCursorText(std::uint32_t id) = 0;                              // view vslot 0xa4
    virtual void CursorTextFormat(cRZString& out) = 0;                               // cSLString {0x6a231eaa, 0x4a54ed13}
    virtual void FollowAutomaton(IDispatchAutomaton* automaton) = 0;                 // city 0xac (vehicle manager) vslot 0x88
    virtual cIGZMessageServer2* MessageServer() = 0;                                  // SL::spMS2
};
IDispatchToolHost* DispatchToolHost();
void SetDispatchToolHost(IDispatchToolHost* host);

class cSC4ViewInputControlDispatch final : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    explicit cSC4ViewInputControlDispatch(std::uint32_t type);                     // 0x002db4b8
    ~cSC4ViewInputControlDispatch() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;                  // 0x002da83c
    std::uint32_t AddRef() override { return ++refCount_; }                       // 0x002da870
    std::uint32_t Release() override;                                             // 0x002da882
    bool DoMessage(cIGZMessage2* message) override;                               // 0x002da994

    bool Init() override;                                                         // 0x002db222
    bool Shutdown() override;                                                     // 0x002db36e
    bool ShouldStack() override { return false; }                                 // 0x002da98c
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;          // 0x002da570
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;            // 0x002daeb6
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;             // 0x002dab42

    std::uint32_t DispatchType() const { return type_; }
    bool LocationOK() const { return locationOK_; }
    IPylonOccupant* HoverPylon() const { return hover_; }
    const cRZString& CursorText() const { return cursorText_; }

private:
    bool PickPylonOccupant(int x, int y, IPylonOccupant*& out);                   // 0x002da5a4
    bool InitiateDispatch(std::uint32_t type, const float position[3], bool ok);   // 0x002da648
    void SetHoverPylon(IPylonOccupant* pylon);                                     // 0x002da69a
    void CalculateDragPylonOffset(IPylonOccupant* pylon, int x, int y);            // 0x002da706
    void UpdateCursorText();                                                      // 0x002da772
    void SendDispatchMessage(std::uint32_t type, IPylonOccupant* pylon);           // 0x002da8ca (data 0, 0, 0, 0)
    bool LocationIsOKForDispatch(std::uint32_t type, const float position[3], bool ok);   // 0x002daafc
    bool DropActivePylon();                                                       // 0x002dad52
    void RestorePickedUpPylon(cISC4DispatchManager* manager);                      // Shutdown / losing the top

    std::uint32_t type_;                         // +0x2c
    bool locationOK_ = false;                    // +0x30
    int dragOffsetX_ = 0, dragOffsetY_ = 0;      // +0x34, +0x38
    cRZTimer timer_{cRZTimer::kMilliseconds};    // +0x3c (units 4)
    std::uint32_t toolCursor_ = 0;               // +0x50
    IPylonOccupant* hover_ = nullptr;            // +0x54 (counted)
    cRZString cursorText_;                       // +0x58
    bool onTop_ = false;                         // +0x64
    float pickedUpPosition_[3] = {0, 0, 0};      // +0x6c
    std::uint32_t pickedUpState_ = 0;            // +0x78
};

}  // namespace sc4

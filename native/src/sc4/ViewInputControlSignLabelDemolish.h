// cSC4ViewInputControlSignLabelDemolish — the bulldozer for signs and labels: Mac i386 0x004933c4..0x00493884 (the x64 build's
// constructor is 0x1006c9a8c), spec OpenSC4 view-input-controls.md section 6. Tool id 0xcbcc987e, cursor 0x8bd4b428.
// Layout: +0x28 the filter (cSC4SignOrLabelFilter, counted), +0x2c the prop under the cursor (cISC4PropOccupant, counted).
// Hovering highlights the sign or label under the cursor (a label at alpha 0.7, a sign made dynamic); a click removes it.
#pragma once
#include <cstdint>

#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropOccupantInterface.h"
#include "sc4/ViewInputControl.h"

namespace sc4 {

constexpr std::uint32_t kSignLabelDemolishId = 0xcbcc987e;
constexpr std::uint32_t kSignLabelDemolishCursor = 0x8bd4b428;
constexpr std::uint32_t kSignLabelDemolishSound = 0x8a5ec047;   // group 0xea451903

// SL::spRenderer, spOccupantManager, spPropManager, spPropDeveloper, spSS.
class ISignLabelDemolishHost {
public:
    virtual ~ISignLabelDemolishHost() = default;
    // renderer slot 0x100 Pick(x, y, filter, &instance): the picked model's occupant (one reference) or null
    virtual cISC4Occupant* PickOccupant(int x, int y, cISC4OccupantFilter* filter) = 0;
    virtual bool PickTerrain(int x, int y, float out[3]) = 0;                                          // renderer slot 0xf4 (.., 0, 1)
    virtual cISC4Occupant* FirstOccupantAt(float x, float z, cISC4OccupantFilter* filter) = 0;        // occupant manager slot 0x74, one reference
    virtual void RemoveProp(cISC4Occupant* occupant) = 0;                                              // prop manager slot 0x74
    virtual void ShowSignsAndLabels() = 0;                                                             // prop developer slot 0x20 (1)
    virtual void PlaySound(std::uint32_t group, std::uint32_t id) = 0;                                 // spSS slot 0x28
};
ISignLabelDemolishHost* SignLabelDemolishHost();
void SetSignLabelDemolishHost(ISignLabelDemolishHost* host);

class cSC4ViewInputControlSignLabelDemolish : public cSC4ViewInputControl {
public:
    cSC4ViewInputControlSignLabelDemolish() = default;                  // 0x004936b0
    ~cSC4ViewInputControlSignLabelDemolish() override;

    bool Init() override;                                               // 0x004933c4
    bool Shutdown() override;                                           // 0x00493448
    bool OnMouseUpL(int x, int y, std::uint32_t mods) override;         // 0x0049347e
    bool OnMouseMove(int x, int y, std::uint32_t mods) override;        // 0x00493884

    cISC4PropOccupant* PickSignOrLabel(int x, int y);                   // 0x004936dc (no reference: the city keeps it)
    void SetSelection(cISC4PropOccupant* prop);                         // 0x0049350c
    cISC4PropOccupant* Selection() const { return selection_; }

private:
    cISC4OccupantFilter* filter_ = nullptr;                             // +0x28
    cISC4PropOccupant* selection_ = nullptr;                            // +0x2c
};

}  // namespace sc4

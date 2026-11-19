// cSC4MissingPluginDataReporter (Mac i386 0x001e336c..0x001e3c8e): after a city is set up (message 0xea8ae29a) it looks for the
// occupants that name a plugin pack (property 0x6a871b82) and have no model in the game's data, and lists those packs in a dialog
// (cSC4MissingPluginPacksDialog, UI script 0xea89b6c3) when pop-up dialogs are on.
#pragma once
#include <cstdint>
#include <set>

#include "gz/WinInterfaces.h"
#include "gz/cIGZMessage2.h"
#include "sc4/OccupantManager.h"

namespace sc4 {

// An occupant is included when it names a plugin pack and its model is missing at the view's zoom and rotation, or, when the
// occupant has no model there, at the first other zoom (4 down to 0, rotation 0) that has one.
class cSC4MissingPluginPackOccupantFilter final : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;                           // @ 0x001e3a4e
    // 0 = the model is there, 1 = it is missing, 2 = no model for that zoom and rotation.
    int AreModelsAvailable(cISC4Occupant* occupant);                                      // @ 0x001e3978
    int AreModelsAvailableForZoomAndRotation(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t orientation,
                                             int state, bool lit);                       // @ 0x001e3840
    int zoom = 0;       // +0xc (the view utilities' zoom)
    int rotation = 0;   // +0x10
};

// The dialog that lists the missing packs; it is the dialog window's message filter (its OK button ends it).
class cSC4MissingPluginPacksDialog final : public cIGZWinMessageFilter {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;                         // @ 0x001e34f6
    std::uint32_t AddRef() override { return ++refCount_; }                               // @ 0x001e34e4
    std::uint32_t Release() override;                                                     // @ 0x001e3a90
    bool DoMessage(cIGZWin* window, cGZMessage& message) override;                        // @ 0x001e352e
    // True when the player pressed OK.
    bool DoDialog(const std::set<std::uint32_t>& packs);                                  // @ 0x001e3612
private:
    std::uint32_t refCount_ = 0;
};

class cSC4MissingPluginDataReporter final : public cIGZMessageTarget2 {
public:
    cSC4MissingPluginDataReporter() = default;                                            // @ 0x001e3abc
    ~cSC4MissingPluginDataReporter();                                                     // @ 0x001e33c4

    bool QueryInterface(std::uint32_t iid, void** out) override;                         // @ 0x001e3490
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool Init();                                                                          // @ 0x001e359a
    bool Shutdown();                                                                      // @ 0x001e336c
    bool DoMessage(cIGZMessage2* message) override;                                       // @ 0x001e3c8e
    bool DoPostCityInit();                                                                // @ 0x001e3af8

private:
    std::uint32_t refCount_ = 0;
    bool initialized_ = false;                                // +0xc
    cSC4MissingPluginPackOccupantFilter* filter_ = nullptr;   // +0x10 (held)
};

// SC4GetPopupModalDialogsEnabled (@ 0x00024db8): SL::spSC4's pop-up setting (vslot 0x5c).
bool SC4GetPopupModalDialogsEnabled();

}  // namespace sc4

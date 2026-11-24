// The region screen's region dialogs: load another region (a list of the nation's regions) and delete the current one.
// Recovered from the Mac i386 build with symbols (cSC4LoadRegionDialog 0x006fd776.., cSC4DeleteRegionDialog 0x006fd736..,
// SC4DisplayLoadRegionDialog 0x002ebbae, SC4DisplayDeleteRegionDialog 0x002ebb32) and the new-region dialog
// (cSC4NewRegionDialog 0x002eb4e2.., SC4DisplayNewRegionDialog 0x002ebc24).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sc4/DialogWinProcs.h"

namespace sc4 {

// The nation as the dialogs read it (cISC4Nation vslots 0x18 count, 0x1c active index, 0x2c region -> vslot 0xc name).
struct RegionDialogNation {
    std::vector<std::string> names;
    int active = 0;
};

class cSC4LoadRegionDialog final : public cSC4DialogWinProcBase {
public:
    explicit cSC4LoadRegionDialog(const RegionDialogNation& nation)
        : cSC4DialogWinProcBase(0x8a5ab1cc, 0x4a5ba0e7, 0x1000), nation_(nation) {}   // @ 0x006fd776
    void GZOnAttach() override;                                                       // @ 0x006fd940
    void GZOnOK() override;                                                           // @ 0x006fda98
    void GZOnCancel() override { EndDialog(0xffffffffu); }                            // @ 0x006fd2a4

private:
    RegionDialogNation nation_;
};

class cSC4DeleteRegionDialog final : public cSC4DialogWinProcBase {
public:
    cSC4DeleteRegionDialog() : cSC4DialogWinProcBase(0x8a5ab1ce, 0x6a5ba20c, 0) {}    // @ 0x006fd736
    void GZOnAttach() override { TranslateTokensInCaption(0x1000); }                  // @ 0x006fd924
};

// The delete-city confirmation (script 0x8a5ab1d0).
class cSC4DeleteCityDialog final : public cSC4DialogWinProcBase {
public:
    cSC4DeleteCityDialog() : cSC4DialogWinProcBase(0x8a5ab1d0, 0x8a5ab1d0, 0) {}      // @ 0x006fd6f6
};

// What the new-region dialog does with the nation: the regions' names (cISC4Nation vslot 0x2c region -> vslot 0xc) and the
// new region itself (nation vslot 0x34 AddNewRegion(name) -> its index; the region's Init (0x20), SetBaseTerrainType (0x4c)
// and Shutdown (0x24)).
class INewRegionNation {
public:
    virtual ~INewRegionNation() = default;
    virtual std::vector<std::string> RegionNames() = 0;
    virtual int AddNewRegion(const cIGZString& name, std::uint32_t baseTerrainType) = 0;
    // nSC4UI::GenericNotificationDialog(SC4LoadString(0x6a231eaa, 0x4a78b7e8), SC4LoadString(0x6a231eaa, 0x8a78b80e)):
    // the region name is taken
    virtual void NotifyNameInUse() = 0;
};

// The new-region dialog (script 0x8a5ab1cb / 0xea5ba0d1): the name (text 0x2000, made unique as "<name> <n>"), flat (radio
// 0x1000) or mountains (radio 0x1001); OK (0x8a5b9959) is enabled while the name is a valid file name. A taken name brings
// the notification "region name in use" and the dialog stays.
class cSC4NewRegionDialog final : public cSC4DialogWinProcBase {
public:
    explicit cSC4NewRegionDialog(INewRegionNation* nation)
        : cSC4DialogWinProcBase(0x8a5ab1cb, 0xea5ba0d1, 0x2000), nation_(nation) {}   // @ 0x006fd7b6
    void GZOnAttach() override;                                                       // @ 0x002ec1a6
    void GZOnOK() override;                                                           // @ 0x002ebf1c
    void GZOnCancel() override { EndDialog(0xffffffffu); }                            // @ 0x002eb4e2
    bool GZOnButtonActivate(std::uint32_t id) override;                               // @ 0x002ebe66
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;             // @ 0x002ec16a
    bool NameConflicts(const cIGZString& name);                                       // @ 0x002eb51a (case-insensitive)

private:
    void EnableControls();                                                            // @ 0x002ec0be
    INewRegionNation* nation_;
};

// A scripted message dialog (the app's quit / save questions): shows the window of script `instance` (id `windowId`)
// modally and answers the id of the button that closed it.
std::uint32_t SC4RunScriptedButtonDialog(cIGZWin* parent, std::uint32_t instance, std::uint32_t windowId);

// The selected region's index, or -1 (cancelled).
int SC4DisplayLoadRegionDialog(cIGZWin* parent, const RegionDialogNation& nation);
// True when the player confirmed.
bool SC4DisplayDeleteRegionDialog(cIGZWin* parent);
// True when the player confirmed (SC4DisplayDeleteCityDialog @ 0x002eb9fa).
bool SC4DisplayDeleteCityDialog(cIGZWin* parent);
// The new region's index, or -1 (cancelled).
int SC4DisplayNewRegionDialog(cIGZWin* parent, INewRegionNation* nation);

}  // namespace sc4

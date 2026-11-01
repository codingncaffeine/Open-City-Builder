// The region screen's region dialogs: load another region (a list of the nation's regions) and delete the current one.
// Recovered from the Mac i386 build with symbols (cSC4LoadRegionDialog 0x006fd776.., cSC4DeleteRegionDialog 0x006fd736..,
// SC4DisplayLoadRegionDialog 0x002ebbae, SC4DisplayDeleteRegionDialog 0x002ebb32). The new-region dialog
// (cSC4NewRegionDialog, 0x002eb4e2..) is not ported yet.
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

// The selected region's index, or -1 (cancelled).
int SC4DisplayLoadRegionDialog(cIGZWin* parent, const RegionDialogNation& nation);
// True when the player confirmed.
bool SC4DisplayDeleteRegionDialog(cIGZWin* parent);

}  // namespace sc4

// cSC4UndergroundCustomView — the custom view of the pipe and subway data views (Mac i386 0x0013a8e6..0x0013b5aa):
// while the view is up, every surface network piece is translucent so the pieces below show through it. Built by
// cSC4WinMapView::SetDataView from the view exemplar's class id (0x4a0b47eb) and started with InitCustomView; the next
// SetDataView shuts it down.
//
// cSC4PipeCustomView (class id 0x6a19bdd8, ctor 0x00678e44) and cSC4SubwayCustomView (ctor 0x00678e0e) are the class
// with the network interface id of their pieces; neither adds anything.
//
// Layout (i386): +0 cRZRefCount, +8 cISC4CustomView, +0xc cIGZMessageTarget2, +0x10 std::list<cISC4NetworkOccupant*>
// (the pieces made translucent, one reference each), +0x18 the network interface id.
#pragma once
#include <cstdint>
#include <list>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZUnknown.h"

namespace sc4 {

class cISC4Occupant;
class cSC4NetworkOccupant;

constexpr std::uint32_t kGZIID_cISC4CustomView = 0x8a175d6b;
constexpr std::uint32_t kGZCLSID_cSC4PipeCustomView = 0x6a19bdd8;
constexpr std::uint32_t kMessageUndergroundOccupantInserted = 0x99ef1142;   // the view listens while it is up
constexpr std::uint32_t kMessageOccupantTranslucencyChanged = 0x99ef1145;  // posted to each piece it changes

// cISC4CustomView: slot 3 (+0xc) InitCustomView, slot 4 (+0x10) ShutdownCustomView, slot 5 RefreshCustomView.
class cISC4CustomView : public cIGZUnknown {
public:
    virtual bool InitCustomView() = 0;
    virtual bool ShutdownCustomView() = 0;
    virtual void RefreshCustomView() = 0;
};

class IUndergroundCustomViewHost {
public:
    virtual ~IUndergroundCustomViewHost() = default;
    virtual int CityCellCountX() = 0;                       // spCity vslot 0x174
    virtual int CityCellCountZ() = 0;                       // spCity vslot 0x178
    // spOccupantManager GetOccupantsByStandardCityCells(cell, cell, all, -1): one reference each.
    virtual void CellOccupants(int x, int z, std::list<cISC4Occupant*>& out) = 0;
    virtual cIGZMessageServer2* MessageServer() = 0;        // SL::spMS2
};
void SetUndergroundCustomViewHost(IUndergroundCustomViewHost* host);

class cSC4UndergroundCustomView : public cISC4CustomView, public cIGZMessageTarget2 {
public:
    explicit cSC4UndergroundCustomView(std::uint32_t networkGziid);   // 0x0013b5aa
    virtual ~cSC4UndergroundCustomView();                             // 0x0013b4fe

    bool QueryInterface(std::uint32_t iid, void** out) override;     // 0x0013a8fa
    std::uint32_t AddRef() override { return ++refs_; }               // 0x0013a8e6
    std::uint32_t Release() override;                                 // 0x0013a8f0
    bool DoMessage(cIGZMessage2* message) override;                  // 0x0013ab1c

    bool InitCustomView() override;                                   // 0x0013ae90
    bool ShutdownCustomView() override;                               // 0x0013b474
    void RefreshCustomView() override {}                              // 0x0067880c (empty)

    void SetRoadsTranslucent(int x, int z);                           // 0x0013ad46
    void DoOccupantInserted(cISC4Occupant* occupant);                 // 0x0013aa7a

    std::size_t TranslucentCount() const { return pieces_.size(); }
    std::uint32_t NetworkGziid() const { return networkGziid_; }

private:
    std::uint32_t refs_ = 0;
    std::list<cSC4NetworkOccupant*> pieces_;   // +0x10
    std::uint32_t networkGziid_;               // +0x18
};

// The data views' custom view by class id (GZCOM CreateInstance(clsid, 0x8a175d6b)): the pipe view, or null for a class
// the port does not have. One reference.
cISC4CustomView* CreateCustomView(std::uint32_t classId);

}  // namespace sc4

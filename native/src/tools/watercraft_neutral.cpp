// The watercraft manager's inherited half (a real cSC4AutomataManager behind a handle) and a neutral host, so the manager can read the
// record of a city file. The host answers "nothing there" for everything outside the record; the references of the record are consumed.
#include <cmath>

#include "gz/cIGZIStream.h"
#include "sc4/DBSegment.h"
#include "sc4/WatercraftManager.h"
#include "tools/automata_base_handle.h"

namespace sc4 {
namespace {

class NeutralBase final : public IWatercraftManagerBase {
public:
    NeutralBase() : handle_(AutomataBaseCreate()) {}
    ~NeutralBase() override { AutomataBaseDestroy(handle_); }
    bool Initialized() const override { return false; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool DoMessage(cIGZMessage2*) override { return false; }
    bool Write(cIGZOStream& stream) const override { return AutomataBaseWrite(handle_, stream); }
    bool Read(cIGZIStream& stream) override { return AutomataBaseRead(handle_, stream); }
    bool ReadInternal(cIGZIStream& stream, std::uint16_t version) override { return AutomataBaseReadInternal(handle_, stream, version); }
    void DoAnimationTick(std::uint32_t, float) override {}
    bool CalculateSimSpeedModifier() override { return false; }
    bool SimSpeedPaused() const override { return false; }
    bool IsDrivingMode() const override { return false; }
    bool CanCreateGroup(std::uint32_t) const override { return true; }
    void SetMaxCount(int) override {}
    void SetMinZoom(int) override {}
    void DestroyAutomata() override {}
    bool PostCityInitPending() const override { return false; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    cRZRandomSuper& Random() override { static cRZRandomSuper random(0xffffffffu); return random; }
    float TuningMaxCountField() const override { return 0.0f; }
    float RetireOpacityDuration() const override { return 0.0f; }

private:
    void* handle_;
    std::uint32_t refs_ = 0;
};

class NeutralHost final : public IWatercraftManagerHost {
public:
    bool AddMessageNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveMessageNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    void WorldToCell(float x, float z, int& cellX, int& cellZ) override { cellX = static_cast<int>(std::floor(x / 16.0f)); cellZ = static_cast<int>(std::floor(z / 16.0f)); }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override { x = static_cast<float>(cellX) * 16.0f; z = static_cast<float>(cellZ) * 16.0f; }
    bool CellInCity(int, int) override { return false; }
    bool PointInCity(float, float) override { return false; }
    void CityCellCount(int& sizeX, int& sizeZ) override { sizeX = sizeZ = 0; }
    float CellSize() override { return 16.0f; }
    cSC4Lot* LotAtPosition(const float*) override { return nullptr; }
    cSC4Lot* LotAtCell(int, int) override { return nullptr; }
    float TerrainHeight(float, float) override { return 0.0f; }
    IWatercraftBodyOfWater* BodyOfWater() override { return nullptr; }
    bool IsWaterCell(int, int, int) override { return false; }
    bool PropOccupantFacing(cISC4Occupant*, int&) override { return false; }
    void VisibleCellRect(int rect[4]) override { rect[0] = rect[1] = rect[2] = rect[3] = 0; }
    cISCResExemplar* GetAutomataTuningExemplar() override { return nullptr; }
    bool WriteSerializable(cIGZOStream& stream, cIGZUnknown* object) override { return SC4WriteGZSerializable(stream, object); }
    cIGZUnknown* ReadSerializable(cIGZIStream& stream, std::uint32_t iid) override {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, iid, &raw, false);
        return static_cast<cIGZUnknown*>(raw);
    }
    bool WriteLot(cIGZOStream& stream, cSC4Lot*) override { return SC4WriteGZSerializable(stream, nullptr); }
    cSC4Lot* ReadLot(cIGZIStream& stream) override {   // the lot is an object reference the lot manager resolves later
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, 0xe818fced, &raw, false);
        if (raw) static_cast<cIGZUnknown*>(raw)->Release();
        return nullptr;
    }
    cSC4Watercraft* CreateWatercraft(std::uint32_t, const cS3DVector3&, std::uint32_t, const cGZPersistResourceKey&, cIGZMessageTarget2*, std::uint8_t,
                                     const cS3DVector3*) override { return nullptr; }
    void FerryRoutesInUse(std::list<FerryRouteInfo>&) override {}
    bool GetWaterRoute(int, int, int, int, std::vector<std::uint8_t>&) override { return false; }
    float SeaLevel() override { return 0.0f; }
    void NeighborConnectionCells(std::uint32_t, std::vector<std::pair<int, int>>&) override {}
};

}  // namespace

void ConnectNeutralWatercraftServices(cSC4WatercraftManager& manager) {
    static NeutralHost host;
    manager.SetHost(&host);
    manager.SetBase(new NeutralBase());
}

}  // namespace sc4

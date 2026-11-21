#include <cstdio>

#include "sc4/BuildingOccupant.h"
#include "sc4/MySimFilters.h"
#include "sc4/PylonOccupant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct Link final : cIGZUnknown {
    bool answers = true;
    int refs = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override { if (answers && iid == kIidPylonLink) { *out = this; ++refs; return true; } return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};
// cISC4BuildingOccupant as the building filter reads it: completion (slot 9) and the profile's purpose (slot 11).
struct Building final : cISC4BuildingOccupant {
    float completion = 1.0f;
    BuildingProfile profile;
    int refs = 1;
    cISC4Occupant* occupant = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cISC4Occupant* AsOccupant() override { return occupant; }
    std::uint32_t GetBuildingType() const override { return 0; }
    void SetBuildingType(std::uint32_t) override {}
    std::int32_t GetBuildingAge() const override { return 0; }
    void SetBuildingAge(std::int32_t) override {}
    void SetBoundingBox(const float*, const float*) override {}
    const float& GetCompletionPercent() override { return completion; }
    void SetCompletionPercent(float) override {}
    const BuildingProfile& GetBuildingProfile() const override { return profile; }
    const cRZString& GetBuildingName() const override { return name; }
    const cRZString& GetExemplarName() const override { return name; }
    std::int32_t GetOrientation() const override { return 0; }
    bool SetOrientation(std::int32_t) override { return false; }
    bool IsLit() const override { return false; }
    void SetLit(bool) override {}
    void SetName(const cRZString&) override {}
    bool GetName(cRZString&) override { return false; }
    cRZString name;
};
struct Occ final : cSC4Occupant {
    Building* building;
    Occ(Building* b, std::uint32_t t) : building(b) { type_ = t; b->occupant = this; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kIidMySimBuilding) { *out = static_cast<cISC4BuildingOccupant*>(building); building->AddRef(); return true; }
        return cSC4Occupant::QueryInterface(iid, out);
    }
};
// cISC4PylonOccupant as the pylon filter reads it: UI state (slot 7), drag state (slot 16), owner (slot 20).
struct Pylon final : cISC4PylonOccupant {
    int uiState = 3; bool dragging = false; Link* owner = nullptr; int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cISC4Occupant* AsOccupant() override { return nullptr; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void SetUIState(std::int32_t) override {}
    std::int32_t GetUIState() const override { return uiState; }
    void SetBitmapInfo(std::int32_t, const cIGZString&) override {}
    void SetDispatchType(std::int32_t) override {}
    std::int32_t GetDispatchType() const override { return 0; }
    void SetDispatchState(std::int32_t) override {}
    std::int32_t GetDispatchState() const override { return 0; }
    void SetCallNumber(std::uint16_t) override {}
    std::uint16_t GetCallNumber() const override { return 0; }
    void SetDragState(bool) override {}
    bool GetDragState() const override { return dragging; }
    void SetSourceOccupant(cISC4Occupant*) override {}
    cISC4Occupant* GetSourceOccupant() const override { return nullptr; }
    void SetPylonOwner(cIGZUnknown*) override {}
    cIGZUnknown* GetPylonOwner() const override { return owner; }
};
struct PylonOcc final : cSC4Occupant {
    Pylon* pylon;
    PylonOcc(Pylon* p, std::uint32_t t) : pylon(p) { type_ = t; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kIidMySimPylon) { *out = static_cast<cISC4PylonOccupant*>(pylon); ++pylon->refs; return true; }
        return cSC4Occupant::QueryInterface(iid, out);
    }
};
struct Host final : IMySimFilterHost {
    std::uint8_t condition = 0;
    cISC4BuildingOccupant* asked = nullptr;
    std::uint8_t BuildingCondition(cISC4BuildingOccupant* building) override { asked = building; return condition; }
};
}  // namespace

int main() {
    Host host;
    SetMySimFilterHost(&host);
    // building
    Building facts;
    facts.profile.purpose = 1;
    auto* b = new Occ(&facts, kOccupantTypeBuilding);
    b->AddRef();
    cMySimBuildingFilter any, kindOne(true);
    CHECK(any.IsOccupantIncluded(b) && kindOne.IsOccupantIncluded(b));
    CHECK(host.asked == &facts);                       // the condition is asked of the building itself
    CHECK(facts.refs == 1);                            // the building reference is returned
    facts.profile.purpose = 2;
    CHECK(any.IsOccupantIncluded(b) && !kindOne.IsOccupantIncluded(b));
    facts.completion = 0.99f;
    CHECK(!any.IsOccupantIncluded(b));
    facts.completion = 1.0f;
    host.condition = 3;
    CHECK(any.IsOccupantIncluded(b));
    host.condition = 4;
    CHECK(!any.IsOccupantIncluded(b));
    host.condition = 0;
    CHECK(facts.refs == 1);
    Building other;
    auto* wrongType = new Occ(&other, 0x1234);
    wrongType->AddRef();
    CHECK(!any.IsOccupantIncluded(wrongType));
    auto* noInterface = new cSC4Occupant();
    noInterface->AddRef();
    CHECK(!any.IsOccupantIncluded(noInterface));
    // pylon
    Pylon p;
    Link link;
    p.owner = &link;
    auto* po = new PylonOcc(&p, kOccupantTypePylon);
    po->AddRef();
    cMySimPylonFilter pf;
    CHECK(pf.IsOccupantIncluded(po) && p.refs == 1 && link.refs == 1);   // all references returned
    link.answers = false;
    CHECK(!pf.IsOccupantIncluded(po));
    link.answers = true;
    p.dragging = true;
    CHECK(!pf.IsOccupantIncluded(po));
    p.dragging = false; p.uiState = 0;
    CHECK(!pf.IsOccupantIncluded(po));
    p.uiState = 1; p.owner = nullptr;
    CHECK(!pf.IsOccupantIncluded(po));
    auto* notPylon = new PylonOcc(&p, kOccupantTypeBuilding);
    notPylon->AddRef();
    CHECK(!pf.IsOccupantIncluded(notPylon));
    b->Release(); wrongType->Release(); noInterface->Release(); po->Release(); notPylon->Release();
    std::printf(failures ? "FAILED\n" : "sc4_mysimfilters OK\n");
    return failures ? 1 : 0;
}

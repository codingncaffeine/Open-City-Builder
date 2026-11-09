// cSC4ViewInputControlQuery: lifecycle / message plumbing, the query state machine, the dialog control helpers,
// picking, and the SetOrGet rows that do not need an unrecovered simulator.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fake_occupant.h"
#include "sc4/BuildingOccupant.h"
#include "gz/Win.h"
#include "gz/WinBMP.h"
#include "gz/WinSlider.h"
#include "gz/WinText.h"
#include "gz/WinTextEdit.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/SC4Utilities.h"
#include "sc4/SCProperty.h"
#include "sc4/ViewInputControlQuery.h"
#include "vic_fakes.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;
using gz::cIGZWinText;
using gz::cIGZWinTextEdit;
using gz::cIGZWinSlider;
using gz::cIGZWinBMP;

namespace {

// ---- the language formatter SC4MakeMoneyString goes through -------------------------------------------------
struct Formatter final : ILanguageFormatter {
    void FormatMoney(std::int64_t value, cIGZString& out, const cRZString&) override {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%s", std::to_string(value).c_str());
        out.AssignFromCString(buffer);
    }
    void FormatMoney(double value, cIGZString& out, const cIGZString&, const cRZString&) override {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.2f", value);
        out.AssignFromCString(buffer);
    }
    void FormatNumber(std::int64_t value, cRZString& out) override { out.AssignFromCString(std::to_string(value).c_str()); }
};

// ---- window manager: DestroyWindow really unparents (ChildDeleteAll loops until the child is gone) -------------
struct Mgr final : cIGZWinMgr {
    vic_fakes::CursorMgr cursors;
    cIGZWin* capture = nullptr;
    cIGZWin* setCapture = nullptr;
    cIGZWin* released = nullptr;
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZWin* GetMainWindow() override { return nullptr; }
    void AppendToPlotStrategy(cIGZWin*) override {}
    void Plot() override {}
    void PostMessageToTarget(cIGZWin*, const cGZMessage&) override {}
    void AddWindowToValidList(cIGZWin*) override {}
    void RemoveWindowFromValidList(cIGZWin*) override {}
    void CleanUpWindowReferences(cIGZWin*) override {}
    bool DestroyWindow(cIGZWin* w) override {
        if (!w) return true;
        if (cIGZWin* parent = w->GetParentWin()) parent->ChildRemove(w);
        return true;
    }
    bool IsWindowValid(cIGZWin*) override { return true; }
    cIGZWin* GetWindowFromPoint(int, int) override { return nullptr; }
    void SetCurrentMouseWin(cIGZWin*) override {}
    cIGZWin* GZGetCapture() override { return capture; }
    void GZSetCapture(cIGZWin* w) override { setCapture = capture = w; }
    void GZReleaseCapture(cIGZWin* w) override { released = w; capture = nullptr; }
    cIGZCursorManager* GetCursorManager() override { return &cursors; }
    cIGZWin* GZGetFocus() override { return nullptr; }
    void GZSetFocus(cIGZWin*) override {}
    cIGZKeyboard* GetKeyboard() override { return nullptr; }
};

// ---- a property holder that answers one bool property -----------------------------------------------------
struct Holder final : cISCPropertyHolder {
    cSCProperty property;
    std::uint32_t id = 0;
    bool value = false;
    std::vector<std::uint32_t> values;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool HasProperty(std::uint32_t propertyId) const override { return propertyId == id; }
    bool GetPropertyList(cIGZUnknownList**) const override { return false; }
    cISCProperty* GetProperty(std::uint32_t propertyId) const override {
        if (propertyId != id) return nullptr;
        auto* self = const_cast<Holder*>(this);
        cRZVariant v;
        if (!values.empty()) v.RefUint32(values.data(), static_cast<std::uint32_t>(values.size()));
        else v.SetValBool(value);
        self->property.SetPropertyID(id);
        self->property.SetPropertyValue(v);
        return &self->property;
    }
    bool GetProperty(std::uint32_t, std::uint32_t&) const override { return false; }
    bool GetProperty(std::uint32_t, cIGZString&) const override { return false; }
    bool GetProperty(std::uint32_t, std::uint32_t, void**) const override { return false; }
    bool GetProperty(std::uint32_t, void*, std::uint32_t&) const override { return false; }
    void AddProperty(cISCProperty*, bool) override {}
    void AddProperty(std::uint32_t, const cIGZVariant*, bool) override {}
    void AddProperty(std::uint32_t, std::uint32_t, bool) override {}
    void AddProperty(std::uint32_t, const cIGZString&, bool) override {}
    void AddProperty(std::uint32_t, cIGZUnknown*, bool) override {}
    void AddProperty(std::uint32_t, const void*, std::uint32_t, bool) override {}
    void CopyAddProperty(cISCProperty*, bool) override {}
    bool RemoveProperty(std::uint32_t, bool) override { return false; }
    void RemoveAllProperties(bool) override {}
    void EnumProperties(void (*)(cISCProperty*, void*), void*) const override {}
    void EnumProperties(bool (*)(cISCProperty*, void*), void (*)(cISCProperty*, void*), void*) const override {}
    void CompactProperties() override {}
};

struct TestOccupant final : cISC4Occupant, cISC4BuildingOccupant {
    std::uint32_t type = 0x278128a0;
    int highlight = 0;
    int highlightSets = 0;
    int names = 0;
    std::string name = "Test Building";
    std::string lastSetName;
    Holder* holder = nullptr;
    std::uint16_t profileCapacity = 0;
    mutable BuildingProfile profile;
    const BuildingProfile& GetBuildingProfile() const override { profile.capacity = profileCapacity; return profile; }
    IQueryNetworkOccupant* network = nullptr;   // answered to iid 0xa821ef94 (one reference taken by the tool, released by it)

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0x87dda3a9) { *out = static_cast<cISC4BuildingOccupant*>(this); return true; }
        if (iid == 0xa821ef94 && network) { *out = network; return true; }
        if (iid == 0x26bb767e) { *out = static_cast<cISC4Occupant*>(this); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return holder; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { xyz[0] = 64.0f; xyz[1] = 0.0f; xyz[2] = 128.0f; }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
    SC4CellRect cityCells{3, 4, 5, 6};
    void GetBoundingCityCells(SC4CellRect& r) override { r = cityCells; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return static_cast<std::uint32_t>(highlight); }
    void SetHighlight(int h, bool) override { highlight = h; ++highlightSets; }
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return true; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }

    void SetName(const cRZString& n) override { ++names; lastSetName = n.ToChar(); }
    bool GetName(cRZString& n) override { n.AssignFromCString(name.c_str()); return true; }
    // the rest of cISC4BuildingOccupant (the tool never reaches them)
    cISC4Occupant* AsOccupant() override { return this; }
    std::uint32_t GetBuildingType() const override { return 0; }
    void SetBuildingType(std::uint32_t) override {}
    std::int32_t GetBuildingAge() const override { return 0; }
    void SetBuildingAge(std::int32_t) override {}
    void SetBoundingBox(const float*, const float*) override {}
    const float& GetCompletionPercent() override { static const float done = 1.0f; return done; }
    void SetCompletionPercent(float) override {}
    const cRZString& GetBuildingName() const override { static const cRZString none; return none; }
    const cRZString& GetExemplarName() const override { static const cRZString none; return none; }
    std::int32_t GetOrientation() const override { return 0; }
    bool SetOrientation(std::int32_t) override { return true; }
    bool IsLit() const override { return false; }
    void SetLit(bool) override {}
};

struct Dummy final : cIGZUnknown {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0x26bb767e || iid == 0xa9b40f05 || iid == 0xe9793a65) { *out = this; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
};

// ---- the view ---------------------------------------------------------------------------------------------
struct View final : IQueryView {
    int refs = 1;
    cIGZWin* window = nullptr;
    bool showCoordinates = false;
    bool pickOk = true;
    float terrain[3] = {10.0f, 1.0f, 20.0f};
    int tips = 0, clears = 0, coverage = 0, ended = -1;
    std::string lastTitle, lastText;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void EndInput(int result) override { ended = result; }
    cSC4ViewInputControl* TopControl() override { return nullptr; }
    cIGZWin* ViewWindow() override { return window; }
    bool PickTerrain(int, int, float out[3], bool) override {
        out[0] = terrain[0]; out[1] = terrain[1]; out[2] = terrain[2];
        return pickOk;
    }
    bool ShowCoordinates() override { return showCoordinates; }
    void SetTip(std::uint32_t, int, const cIGZString* title, const cIGZString* text, int) override {
        ++tips;
        lastTitle = title ? title->ToChar() : "";
        lastText = text ? text->ToChar() : "";
    }
    void ClearTip(std::uint32_t) override { ++clears; }
    void RecalcCoverage() override { ++coverage; }
};

// ---- the callout box --------------------------------------------------------------------------------------
struct Callout final : cISC4WinCalloutBox {
    int refs = 1;
    cIGZWin* win = nullptr;
    std::string title, text;
    std::uint32_t textStyle = 0, titleStyle = 0;
    cRZRect avoid{};
    cRZPoint point{};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZWin* AsIGZWin() override { return win; }
    void SetBackgroundImage(cIGZBuffer*, int, int) override {}
    cIGZBuffer* GetBackgroundImage() const override { return nullptr; }
    void SetTitle(const cIGZString& t) override { title = t.ToChar(); }
    const cIGZString& GetTitle() const override { return title_; }
    void SetText(const cIGZString& t) override { text = t.ToChar(); }
    const cIGZString& GetText() const override { return text_; }
    void SetTitleFontStyle(std::uint32_t style) override { titleStyle = style; }
    void SetTextFontStyle(std::uint32_t style) override { textStyle = style; }
    void SetIcon(cIGZBuffer*) override {}
    cIGZBuffer* GetIcon() const override { return nullptr; }
    void SetIconMeter(cIGZBuffer*, std::uint32_t, float) override {}
    void SetPointOfInterest(const cRZPoint& p) override { point = p; }
    const cRZPoint& GetPointOfInterest() const override { return point; }
    void SetAvoidanceRect(const cRZRect& r) override { avoid = r; }
    const cRZRect& GetAvoidanceRect() const override { return avoid; }
    void SetConstraintRect(const cRZRect&) override {}
    const cRZRect& GetConstraintRect() const override { return avoid; }

private:
    cRZString title_, text_;
};

// ---- picking ----------------------------------------------------------------------------------------------
struct Filter final : IOccupantFilter {
    int refs = 1;
    int kind = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool IsValid(const cISC4Occupant*) override { return true; }
};

struct PickResult final : IPickResult {
    int refs = 1;
    cIGZUnknown* object = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZUnknown* GetObject() override { return object; }
};

struct FakeRenderer final : IQueryRenderer {
    cIGZUnknown* nextObject = nullptr;
    int picks = 0, filters = 0, layersAdded = 0, layersRemoved = 0;
    Filter* lastFilter = nullptr;
    int lastX = 0, lastY = 0;
    IOccupantFilter* CreateFilter(int kind) override {
        ++filters;
        auto* f = new Filter();
        f->kind = kind;
        lastFilter = f;
        return f;
    }
    IPickResult* Pick(int x, int y, IOccupantFilter*) override {
        ++picks;
        lastX = x;
        lastY = y;
        if (!nextObject) return nullptr;
        auto* r = new PickResult();
        r->object = nextObject;
        return r;
    }
    void AddLayer(cIGZUnknown*) override { ++layersAdded; }
    void RemoveLayer(cIGZUnknown*) override { ++layersRemoved; }
};

// ---- lots -------------------------------------------------------------------------------------------------
struct Lot final : IQueryLot {
    int refs = 1;
    SC4CellRect rect{};
    int state = 0;
    int reason = 0;
    bool historical = false;
    bool needsPower = false, hasPower = true;
    bool GetNeedsPower() override { return needsPower; }
    bool GetHasPower() override { return hasPower; }
    Holder* lotHolder = nullptr;
    cISCPropertyHolder* AsPropertyHolder() override { return lotHolder; }
    int wealth = 0;
    std::uint16_t capacity = 0;
    std::uint32_t buildingType = 1;
    float occupancy = 0.0f;
    std::vector<cISC4Occupant*> occupants;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void GetOccupants(std::vector<cISC4Occupant*>& out) override { out = occupants; }
    void GetCellRect(SC4CellRect& out) override { out = rect; }
    int GetState() override { return state; }
    bool IsHistorical() override { return historical; }
    void SetHistorical(bool on) override { historical = on; }
    int GetAbandonReason() override { return reason; }
    int GetOccupantWealth() override { return wealth; }
    std::uint16_t GetCapacity(int) override { return capacity; }
    std::uint32_t GetBuildingType(bool) override { return buildingType; }
    float GetOccupancy() override { return occupancy; }
};

struct LotMgr final : IQueryLotManager {
    Lot* byOccupant = nullptr;
    Lot* byCell = nullptr;
    int occupantLookups = 0, cellLookups = 0;
    IQueryLot* GetLotForOccupant(cISC4Occupant*) override { ++occupantLookups; return byOccupant; }
    IQueryLot* GetLotAtCell(int, int, int) override { ++cellLookups; return byCell; }
};

struct PortDeveloperFake final : IQueryPortDeveloper {
    std::uint32_t usage = 0, capacity = 0;
    bool answers = true;
    std::int64_t maintenance = 0;
    IQueryLot* lastLot = nullptr;
    bool GetUsageAndCapacity(IQueryLot* lot, std::uint32_t& u, std::uint32_t& c) override { lastLot = lot; u = usage; c = capacity; return answers; }
    std::int64_t GetMaintenanceCost(IQueryLot* lot) override { lastLot = lot; return maintenance; }
};

// ---- the simulators the ported rows use -------------------------------------------------------------------
struct Power final : IQueryPowerSim {
    bool powered = false;
    bool IsPowered(cISC4Occupant*) override { return powered; }
    std::uint32_t capacity = 0, used = 0, maxProduced = 0;
    float efficiency = 0.0f, condition = 0.0f, funding = 0.0f, setFunding = -1.0f;
    std::int64_t monthlyCost = 0;
    bool fundingOk = true, costOk = true;
    int setCalls = 0;
    void GetOccupantPowerProduced(cISC4Occupant*, std::uint32_t& c, std::uint32_t& u) override { c = capacity; u = used; }
    void GetOccupantMaxPowerProduced(cISC4Occupant*, std::uint32_t& out) override { out = maxProduced; }
    float GetCurrentPlantEfficiencyPercent(cISC4Occupant*) override { return efficiency; }
    float GetCurrentPlantConditionPercent(cISC4Occupant*) override { return condition; }
    bool GetMonthlyPowerGenerationCost(cISC4Occupant*, std::int64_t& cost) override { cost = monthlyCost; return costOk; }
    bool GetPowerPlantFundingPercentage(cISC4Occupant*, float& f) override { f = funding; return fundingOk; }
    void SetPowerPlantFundingPercentage(cISC4Occupant*, float f) override { setFunding = f; ++setCalls; }
};
struct PollutionFake final : IQueryPollutionSim {
    std::int64_t produced = 0, recycled = 0, landfilled = 0;
    int centres = 0;
    float scale = 1.0f, maxCapacity = 0.0f, currentCapacity = 0.0f, efficiency = 0.0f;
    std::uint32_t incUsed = 0, incCurrent = 0, wteUsed = 0;
    std::int64_t GarbageProducedPrevMonth() override { return produced; }
    std::int64_t GarbageRecycledPrevMonth() override { return recycled; }
    std::int64_t GarbageLandfilledPrevMonth() override { return landfilled; }
    int RecyclingCenterCount() override { return centres; }
    float GarbageScalingFactor() override { return scale; }
    void RecyclingCenterMaxCapacity(cISC4Occupant*, float& o) override { o = maxCapacity; }
    void RecyclingCenterCurrentCapacity(cISC4Occupant*, float& o) override { o = currentCapacity; }
    bool CurrentOccupantEfficiency(cISC4Occupant*, float& o) override { o = efficiency; return true; }
    void IncineratorUsedCapacity(cISC4Occupant*, std::uint32_t& o) override { o = incUsed; }
    void WasteToEnergyUsedCapacity(cISC4Occupant*, std::uint32_t& o) override { o = wteUsed; }
    void IncineratorCurrentCapacity(cISC4Occupant*, std::uint32_t& o) override { o = incCurrent; }
};
struct LandfillFake final : IQueryLandfillDeveloper {
    std::uint32_t usage = 0, capacity = 0;
    std::int64_t budget = 0;
    void GetLandfillUsage(std::uint32_t& u, std::uint32_t& c) override { u = usage; c = capacity; }
    std::int64_t GetCurrentBudget() override { return budget; }
};
struct Plumbing final : IQueryPlumbingSim {
    bool watered = false;
    bool IsWatered(cISC4Occupant*) override { return watered; }
    bool infoOk = true, conditionOk = true, upkeepOk = true;
    int pipes = 0, distressedPipes = 0;
    int GetNumPipes() override { return pipes; }
    int GetNumDistressedPipes() override { return distressedPipes; }
    std::uint32_t current = 0, maximum = 0, used = 0;
    float efficiency = 0.0f, condition = 0.0f;
    bool pump = false;
    std::int64_t upkeep = 0;
    bool GetWaterProducerInfo(cISC4Occupant*, std::uint32_t* c, std::uint32_t* m, float* e, bool* p, std::uint32_t* u) override {
        if (c) *c = current;
        if (m) *m = maximum;
        if (e) *e = efficiency;
        if (p) *p = pump;
        if (u) *u = used;
        return infoOk;
    }
    bool GetWaterProducerConditionPercent(cISC4Occupant*, float& f) override { f = condition; return conditionOk; }
    bool GetOccupantMonthlyUpkeepCost(cISC4Occupant*, std::int64_t& c) override { c = upkeep; return upkeepOk; }
};
struct Aura final : IQueryAuraSim {
    std::int8_t value = 0, mayorRating = 0;
    std::int8_t GetAura(int, int) override { return value; }
    std::int8_t GetMayorRating() override { return mayorRating; }
};
struct GrowthDeveloperFake final : IQueryGrowthDeveloper {
    cSC4ResponseCurve curves[9];   // 0..3 pollution, 4 school, 5 hospital, 6 crime, 7 trip length, 8 traffic
    bool present[9] = {};
    int lows[8] = {}, highs[8] = {};
    void GetRatingThresholds(int kind, int& low, int& high) override { low = lows[kind]; high = highs[kind]; }
    const cSC4ResponseCurve* Get(int i) const { return present[i] ? &curves[i] : nullptr; }
    const cSC4ResponseCurve* PollutionCurve(int index) override { return Get(index); }
    const cSC4ResponseCurve* SchoolCurve() override { return Get(4); }
    const cSC4ResponseCurve* HospitalCurve() override { return Get(5); }
    const cSC4ResponseCurve* CrimeCurve() override { return Get(6); }
    const cSC4ResponseCurve* TripLengthCurve() override { return Get(7); }
    const cSC4ResponseCurve* TrafficCurve() override { return Get(8); }
    GrowthDeveloperFake() {
        for (int i = 0; i < 9; ++i) curves[i].SetResponseCurveData(std::vector<SC4Point>{SC4Point{0.0f, 0.0f}, SC4Point{300.0f, 300.0f}});   // identity
    }
};
struct NetworkFake final : IQueryNetworkOccupant {
    std::uint32_t types = 0;   // bit per network type
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool IsOfType(std::uint32_t type) const override { return (types >> type) & 1u; }
};
struct LandValueFake final : IQueryLandValueSim {
    int type = 0, x = -1, z = -1;
    int GetLandValueType(int cx, int cz) override { x = cx; z = cz; return type; }
};
struct Sim final : IQuerySimulator { std::uint32_t frame = 0; std::uint32_t FrameCount() override { return frame; } };
struct ResSim final : IQueryResidentialSim {
    int calls = 0, x = 0, z = 0;
    void SetQueryCell(int cx, int cz) override { ++calls; x = cx; z = cz; }
    QueryEducationRecord school, hospital;
    bool schoolAnswers = true, hospitalAnswers = true;
    std::uint32_t first = 0, second = 0;
    int capacityCalls = 0, fundingCalls = 0;
    float lastStaff = -1.0f, lastSecond = -1.0f;
    bool lastEducation = false;
    bool GetSchoolQueryData(cISC4Occupant*, QueryEducationRecord& r) override { r = school; return schoolAnswers; }
    bool GetHospitalQueryData(cISC4Occupant*, QueryEducationRecord& r) override { r = hospital; return hospitalAnswers; }
    void EstimateCurrentOccupantCapacity(cISC4Occupant*, std::uint32_t& a, std::uint32_t& b) override { ++capacityCalls; a = first; b = second; }
    void SetOccupantFundingPercentages(cISC4Occupant*, float staff, float other, bool education) override {
        ++fundingCalls; lastStaff = staff; lastSecond = other; lastEducation = education;
    }
};

// ---- the fire / police / budget / neighbour / traffic seams the new rows read ------------------------------
struct FireSimFake final : IQueryFireSim {
    float effectiveness = 0.5f;
    int fundingCost = 0;
    float fundingFraction = 0.0f;
    float setFraction = -1.0f;
    int setFundingCalls = 0;
    std::uint32_t availability = 0, baseCall = 0, dispatchCount = 0;
    bool dispatchOk = true;
    float StationEffectiveness(cISC4Occupant*) override { return effectiveness; }
    bool StationFunding(cISC4Occupant*, int* cost, float* fraction) override {
        if (cost) *cost = fundingCost;
        if (fraction) *fraction = fundingFraction;
        return true;
    }
    void SetStationFunding(cISC4Occupant*, float fraction) override {
        ++setFundingCalls;
        setFraction = fraction;
    }
    bool DispatchInfo(cISC4Occupant*, std::uint32_t* a, std::uint32_t* b, std::uint32_t* c) override {
        if (!dispatchOk) return false;
        if (a) *a = availability;
        if (b) *b = baseCall;
        if (c) *c = dispatchCount;
        return true;
    }
};

struct PoliceSimFake final : IQueryPoliceSim {
    float effectiveness = 0.25f;
    int arrests = 0, crimes = 0;
    int fundingCost = 0;
    float fundingFraction = 0.0f;
    float setFraction = -1.0f;
    int setFundingCalls = 0;
    std::uint32_t dispatchCount = 0;
    bool dispatchOk = true;
    float jailEfficiency = 0.0f;
    int jailCapacity = 0;
    std::int64_t jailUpkeep = 0;
    float StationEffectiveness(cISC4Occupant*) override { return effectiveness; }
    int ArrestCount(cISC4Occupant*) override { return arrests; }
    int CrimeCount(cISC4Occupant*) override { return crimes; }
    bool StationFunding(cISC4Occupant*, int* cost, float* fraction) override {
        if (cost) *cost = fundingCost;
        if (fraction) *fraction = fundingFraction;
        return true;
    }
    void SetStationFunding(cISC4Occupant*, float fraction) override {
        ++setFundingCalls;
        setFraction = fraction;
    }
    bool DispatchInfo(cISC4Occupant*, std::uint32_t* a, std::uint32_t* b, std::uint32_t* c) override {
        if (!dispatchOk) return false;
        if (a) *a = 0;
        if (b) *b = 0;
        if (c) *c = dispatchCount;
        return true;
    }
    float JailEfficiency() override { return jailEfficiency; }
    int JailCapacity(cISC4Occupant*) override { return jailCapacity; }
    std::int64_t JailMaintenance(cISC4Occupant*) override { return jailUpkeep; }
};

struct BudgetSimFake final : IQueryBudgetSim {
    std::int64_t cost = 0;
    bool ok = true;
    int calls = 0;
    bool OccupantValueCost(cISCPropertyHolder*, std::uint32_t, std::int64_t& out) override {
        ++calls;
        if (!ok) return false;
        out = cost;
        return true;
    }
    Item item;
    float departmentFraction = 0.0f;
    bool departmentOk = true;
    std::uint32_t lastPurpose = 0, lastDepartment = 0;
    bool GetBudgetItemForPurpose(cISCPropertyHolder*, std::uint32_t purpose, Item& out) override { lastPurpose = purpose; out = item; return true; }
    bool DepartmentFundingFraction(std::uint32_t department, float& f) override { lastDepartment = department; f = departmentFraction; return departmentOk; }
};

struct NeighborCityFake final : IQueryNeighborCity {
    int refs = 1;
    std::string cityName, mayorName;
    int cityNameCalls = 0, mayorNameCalls = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void GetCityName(cIGZString& out) const override {
        ++const_cast<NeighborCityFake*>(this)->cityNameCalls;
        out.AssignFromCString(cityName.c_str());
    }
    void GetMayorName(cIGZString& out) const override {
        ++const_cast<NeighborCityFake*>(this)->mayorNameCalls;
        out.AssignFromCString(mayorName.c_str());
    }
};

struct NeighborsSimFake final : IQueryNeighborsSim {
    bool validCell = true;
    bool hasConnection = true;
    NeighborCityFake* city = nullptr;
    int lastType = -1, lastX = 0, lastZ = 0;
    bool IsValidConnectionCell(int, int) override { return validCell; }
    bool FindConnection(std::uint32_t type, int x, int z) override {
        lastType = static_cast<int>(type);
        lastX = x;
        lastZ = z;
        return hasConnection;
    }
    IQueryNeighborCity* FindCityOnOtherSide(int, int) override {
        if (city) city->AddRef();   // the caller releases (the original's returned pointer is owned)
        return city;
    }
};

struct TrafficSimFake final : IQueryTrafficSim {
    std::uint8_t cell = 0;
    int x = -1, z = -1;
    TransitData transit;
    bool transitOk = true;
    int transitX = -1, transitZ = -1;
    bool GetTransitSwitchQueryData(int cx, int cz, TransitData& out) override { transitX = cx; transitZ = cz; out = transit; return transitOk; }
    std::uint8_t CongestionCell(int cx, int cz) override {
        x = cx;
        z = cz;
        return cell;
    }
};

// A cIGZWinMessageFilter the filter rows can install and remove.
struct WinFilterFake final : cIGZWinMessageFilter {
    int refs = 1;
    int messages = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cIGZWin*, cGZMessage&) override {
        ++messages;
        return false;
    }
};

// ---- messages ---------------------------------------------------------------------------------------------
struct MsgServer final : cIGZMessageServer2 {
    int refs = 1;
    std::vector<std::uint32_t> added, removed;
    std::vector<std::uint32_t> sent, posted, targeted;
    std::size_t queue = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool MessageSend(cIGZMessage2* m) override { sent.push_back(m->GetType()); return true; }
    bool MessagePost(cIGZMessage2* m, bool) override { posted.push_back(m->GetType()); return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t type) override { added.push_back(type); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override { removed.push_back(type); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2* m, cIGZMessageTarget2*) override {
        targeted.push_back(m->GetType());
        return true;
    }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return static_cast<std::uint32_t>(queue); }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// ---- host -------------------------------------------------------------------------------------------------
struct Host final : IViewInputHost, IQueryHost {
    View view;
    Mgr mgr;
    vic_fakes::CursorMgr cursors;
    Callout* callout = nullptr;
    FakeRenderer renderer;
    LotMgr lots;
    PortDeveloperFake airport, seaport;
    IQueryPortDeveloper* AirportDeveloper() override { return &airport; }
    IQueryPortDeveloper* SeaportDeveloper() override { return &seaport; }
    Power power;
    PollutionFake pollution;
    LandfillFake landfill;
    IQueryPollutionSim* PollutionSim() override { return &pollution; }
    IQueryLandfillDeveloper* LandfillDeveloper() override { return &landfill; }
    Plumbing plumbing;
    Aura aura;
    LandValueFake landValue;
    bool water = false;
    std::uint32_t zone = 0xff;
    bool TerrainCellHasWater(int, int) override { return water; }
    std::uint32_t ZoneTypeAt(int, int) override { return zone; }
    bool exemplarOk = false;
    std::uint32_t exemplarProperty = 0;
    bool LoadExemplarString(cIGZString& out, cISCPropertyHolder*, std::uint32_t property) override {
        exemplarProperty = property;
        if (exemplarOk) out.AssignFromCString("exemplar name");
        return exemplarOk;
    }
    GrowthDeveloperFake developer;
    bool hasDeveloper = true;
    std::uint32_t typeWealth = 0;
    std::uint32_t BuildingTypeWealth(std::uint32_t) override { return typeWealth; }
    std::uint16_t pollutionMaps[4] = {};
    float school = 0.0f, hospitalGrade = 0.0f;
    std::uint8_t crime = 0, traffic = 0, tripLength = 0;
    std::uint16_t PollutionMapValue(int map, int, int) override { return pollutionMaps[map]; }
    float SchoolGradeValue(int, int) override { return school; }
    float HospitalGradeValue(int, int) override { return hospitalGrade; }
    std::uint8_t CrimeValue(int, int) override { return crime; }
    std::uint8_t TrafficValue(int, int) override { return traffic; }
    std::uint8_t TripLengthValue(int, int) override { return tripLength; }
    IQueryGrowthDeveloper* FindBestDeveloper(IQueryLot*) override { return hasDeveloper ? &developer : nullptr; }
    Holder* tuning = nullptr;
    std::uint32_t tuningType = 0, tuningGroup = 0, tuningInstance = 0;
    IQueryLandValueSim* LandValueSim() override { return &landValue; }
    cISCPropertyHolder* GetTuningProperties(const cGZPersistResourceKey& key) override { tuningType = key.type; tuningGroup = key.group; tuningInstance = key.instance; return tuning; }
    Sim sim;
    ResSim residential;
    FireSimFake fireSim;
    PoliceSimFake policeSim;
    BudgetSimFake budgetSim;
    NeighborsSimFake neighborsSim;
    TrafficSimFake trafficSim;
    MsgServer messages;
    cIGZWin* legendParent = nullptr;
    int attached = 0, detached = 0, sounds = 0;
    std::uint32_t soundId = 0;
    bool debug = false;
    std::vector<std::uint32_t> keys;
    std::string money = "1,234.56";
    std::vector<std::uint32_t> strings;   // instances the tool asked for
    std::string yes = "Yes", no = "No";

    IViewInputView* View3D() override { return &view; }
    cIGZWinMgr* WindowManager() override { return &mgr; }
    void PlaySound(int, std::uint32_t id) override { ++sounds; soundId = id; }

    cIGZMessageServer2* MessageServer() override { return &messages; }
    IQueryView* QueryView() override { return &view; }
    IQueryRenderer* Renderer() override { return &renderer; }
    IQueryCity* City() override { return &city; }
    IQueryLotManager* LotManager() override { return &lots; }
    IQueryPowerSim* PowerSim() override { return &power; }
    IQueryPlumbingSim* PlumbingSim() override { return &plumbing; }
    IQueryAuraSim* AuraSim() override { return &aura; }
    IQuerySimulator* Simulator() override { return &sim; }
    IQueryResidentialSim* ResidentialSim() override { return &residential; }
    IQueryFireSim* FireSim() override { return &fireSim; }
    IQueryPoliceSim* PoliceSim() override { return &policeSim; }
    IQueryBudgetSim* BudgetSim() override { return &budgetSim; }
    IQueryNeighborsSim* NeighborsSim() override { return &neighborsSim; }
    IQueryTrafficSim* TrafficSim() override { return &trafficSim; }
    IQueryPathView* CreatePathView() override { return nullptr; }
    cISC4WinCalloutBox* CreateCalloutBox() override { return callout; }
    cIGZWin* CreateWindowFromScript(std::uint32_t instance, cIGZWin*, std::uint32_t) override {
        legendInstances.push_back(instance);
        return legendParent;
    }
    void AttachLegendWinProc(cIGZWin*) override { ++attached; }
    void DetachWinProc(cIGZWin*) override { ++detached; }
    bool IsKeyDown(std::uint32_t key) override {
        for (std::uint32_t k : keys) if (k == key) return true;
        return false;
    }
    bool IsDebugMode() override { return debug; }
    void FormatMoneyFloat(double value, cRZString& out) override {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%s%.2f", money.c_str(), value);
        out.AssignFromCString(buffer);
    }
    void LoadString(std::uint32_t, std::uint32_t instance, cIGZString& out) override {
        strings.push_back(instance);
        if (instance == 0xca5d4f33) out.AssignFromCString(yes.c_str());
        else if (instance == 0x6a5d4f37) out.AssignFromCString(no.c_str());
        else if (instance == 0xcc06a5e0) out.AssignFromCString("Abandoned: commute");
        else out.AssignFromCString("Loaded");
    }

    // cISC4City stand-in
    struct City final : IQueryCity {
        int follows = 0, props = 0, toCell = 0;
        int cellX = 0, cellZ = 0;
        int cellsX = 64, cellsZ = 64;
        bool QueryInterface(std::uint32_t, void**) { return false; }
        void PositionToCell(float, float, int& x, int& z) override { ++toCell; x = cellX; z = cellZ; }
        int CellCountX() override { return cellsX; }
        int CellCountZ() override { return cellsZ; }
        void FollowAutomaton(cIGZUnknown*) override { ++follows; }
        void SelectStoplightProp(cIGZUnknown*) override { ++props; }
    } city;

    std::vector<std::uint32_t> legendInstances;
};

}  // namespace

int main() {
    Formatter formatter;
    SetLanguageFormatter(&formatter);
    Host host;
    auto* viewWin = new cGZWin(&host.mgr);
    host.view.window = viewWin;
    auto* calloutWin = new cGZWin(&host.mgr);
    auto callout = new Callout();
    callout->win = calloutWin;
    host.callout = callout;
    SetViewInputHost(&host);
    SetQueryHost(&host);

    // --- A: construction, ids, refcount, QueryInterface -----------------------------------------------------
    auto* tool = new cSC4ViewInputControlQuery(cSC4ViewInputControlQuery::kQueryBuilding, 250);
    CHECK(tool->QueryType() == cSC4ViewInputControlQuery::kQueryBuilding);
    CHECK(tool->TipPeriod() == 250 && tool->TipEnabled() && !tool->HoverValid() && !tool->MouseDown());
    CHECK(tool->ShouldStack() == false && tool->OnKeyDown(1, 2) == false);

    CHECK(tool->AddRef() == 1);
    {
        void* out = nullptr;
        CHECK(tool->QueryInterface(kGZIID_cSC4ViewInputControlQuery, &out) &&
              out == static_cast<cIGZMessageTarget2*>(tool));
        CHECK(tool->QueryInterface(1, &out));
        CHECK(tool->QueryInterface(kGZIID_cISC4ViewInputControl, &out) && out == static_cast<cSC4ViewInputControl*>(tool));
        CHECK(!tool->QueryInterface(0x1234, &out));
        CHECK(tool->Release() >= 1);   // the two QueryInterface references are still held
        CHECK(tool->AddRefFromMessageTarget() >= 2);
        CHECK(tool->ReleaseFromMessageTarget() >= 1);
        CHECK(tool->QueryInterfaceFromMessageTarget(1, &out));
        CHECK(tool->Release() >= 1);
    }

    // --- B: Init -------------------------------------------------------------------------------------------
    tool->SetWindow(viewWin);
    CHECK(tool->Init());
    CHECK(tool->GetID() == 0xc7af928e);
    CHECK(tool->QueryType() == cSC4ViewInputControlQuery::kQueryBuilding);
    CHECK(host.messages.added.size() == 6);
    CHECK(host.messages.added[0] == 0xa456d95 && host.messages.added[5] == 0x69247dc7);
    CHECK(host.callout == callout && callout->refs == 1);
    CHECK(callout->titleStyle == 0x0a52f5c6 && callout->textStyle == 0xa85f1a83);
    CHECK(callout->avoid.left == -40 && callout->avoid.right == 40);
    CHECK(!calloutWin->IsVisible());

    // --- C: SetTip -----------------------------------------------------------------------------------------
    cRZString title, text;
    title.AssignFromCString("Title");
    text.AssignFromCString("Body");
    callout->title.clear();
    callout->text.clear();
    tool->SetTip(&title, &text);
    CHECK(callout->title == "Title" && callout->text == "Body" && calloutWin->IsVisible());
    const int clearsBefore = host.view.clears;
    tool->SetTip(nullptr, nullptr);
    CHECK(host.view.clears == clearsBefore + 1 && !calloutWin->IsVisible());

    // --- D: DoMessage --------------------------------------------------------------------------------------
    {
        // the tick with tips turned off does nothing
        cRZMessage2Standard off;
        off.SetType(0x26d31ec2);
        CHECK(!tool->DoMessage(&off) && !tool->TipEnabled());
        const int tips = host.view.tips;
        cRZMessage2Standard tick;
        tick.SetType(0xa456d95);
        tool->DoMessage(&tick);
        CHECK(host.view.tips == tips);
        CHECK(tool->DoMessageFromMessageTarget(&tick) == false);

        cRZMessage2Standard coverage;
        coverage.SetType(0xa456d96);
        tool->DoMessage(&coverage);
        CHECK(host.view.coverage == 1);

        // dialog refresh is gated to every 10th frame and needs the dialog + callback
        cRZMessage2Standard dialog;
        dialog.SetType(0x66956814);
        host.sim.frame = 5;
        tool->DoMessage(&dialog);
        CHECK(cSC4ViewInputControlQuery::QueryDialogWindow() == nullptr);

        // mouse-move message: the window manager's position reaches OnMouseMove
        cRZMessage2Standard move;
        move.SetType(0xa6b79602);
        tool->DoMessage(&move);
        CHECK(host.renderer.picks >= 1);

        // path update without a path view is a no-op
        cRZMessage2Standard paths;
        paths.SetType(0x69247dc7);
        CHECK(!tool->DoMessage(&paths));
    }

    // --- E: hover state machine ----------------------------------------------------------------------------
    {
        TestOccupant occupant;
        Holder holder;
        holder.id = 0x6a95e503;
        holder.value = true;
        occupant.holder = &holder;
        host.renderer.nextObject = static_cast<cISC4Occupant*>(&occupant);
        auto* lot = new Lot();
        lot->occupants.push_back(static_cast<cISC4Occupant*>(&occupant));
        host.lots.byOccupant = lot;
        host.lots.byCell = nullptr;

        host.view.pickOk = true;
        host.view.terrain[0] = 64.0f;
        host.view.terrain[2] = 128.0f;
        CHECK(tool->OnMouseMove(5, 6, 0));
        CHECK(tool->HoverValid() && tool->HoveredOccupant() == static_cast<cISC4Occupant*>(&occupant));
        CHECK(occupant.highlight == 7 && occupant.highlightSets >= 1);
        // SetHighlightState sweeps the lot: the occupant carries property 0x6a95e503, so it is set to 7 again
        CHECK(occupant.highlightSets >= 2);
        CHECK(tool->TipTimer().IsRunning());

        CHECK(tool->OnMouseExit());
        CHECK(!tool->HoverValid() && tool->HoveredOccupant() == nullptr);
        CHECK(occupant.highlight == 0);
        CHECK(!tool->TipTimer().IsRunning());
        delete lot;
        host.lots.byOccupant = nullptr;
    }

    // --- F: clicks -----------------------------------------------------------------------------------------
    {
        host.renderer.nextObject = nullptr;   // misses
        host.messages.sent.clear();
        host.messages.targeted.clear();
        const std::size_t adds = host.messages.targeted.size();
        host.view.terrain[0] = 40.0f;
        host.view.terrain[2] = 80.0f;
        host.city.cellX = 40;
        host.city.cellZ = 80;
        CHECK(tool->OnMouseDownL(3, 4, 1));
        CHECK(host.renderer.picks >= 1);   // automaton + stoplight picks
        CHECK(host.messages.sent.size() == 1 && host.messages.sent[0] == 0x2acd2cf3);
        CHECK(host.messages.targeted.size() == adds);   // the tip refresh was not requested here
        CHECK(!tool->MouseDown());   // the building flavour clears it after DescribePick
        CHECK(host.mgr.setCapture == viewWin);
        CHECK(tool->OnMouseUpL(3, 4, 1) && !tool->MouseDown());

        host.sim.frame = 0;
        host.residential.calls = 0;
        CHECK(tool->OnMouseDownL(3, 4, 2));
        CHECK(host.residential.calls == 1 && host.residential.x == 40 >> 2 && host.residential.z == 80 >> 2);
    }

    // --- G: dialog controls --------------------------------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1200);
        auto* edit = new gz::cGZWinTextEdit(&host.mgr);
        edit->SetID(0x1210);
        auto* slider = new gz::cGZWinSlider(&host.mgr);
        slider->SetID(0x1220);
        auto* meter = new gz::cGZWinBMP(&host.mgr);
        meter->SetID(0x1230);
        auto* hidden = new gz::cGZWinText(&host.mgr);
        hidden->SetID(0x1240);
        hidden->ShowWindow();
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(edit) && dialog->ChildAdd(slider));
        CHECK(dialog->ChildAdd(meter) && dialog->ChildAdd(hidden));

        cRZString value;
        value.AssignFromCString("hello");
        CHECK(tool->SetEditControlText(dialog, 0x1210, value));
        CHECK(tool->SetStaticControlText(dialog, 0x1200, value));
        CHECK(tool->SetStaticControlNumeric(dialog, 0x1200, 42));
        CHECK(tool->SetStaticControlNumericPercent(dialog, 0x1200, 37));
        CHECK(tool->SetStaticControlSimoleons(dialog, 0x1200, 1234567));
        CHECK(tool->SetStaticControlSimoleonsFloat(dialog, 0x1200, 12.5));
        CHECK(tool->SetPerformanceMeterValue(dialog, 0x1230, 0.25f));
        CHECK(!tool->SetEditControlText(dialog, 0x9999, value));
        CHECK(!tool->SetStaticControlText(dialog, 0x9999, value));

        cRZString readBack;
        std::uint32_t length = 0;
        CHECK(tool->GetEditControlText(dialog, 0x1210, readBack, length));
        CHECK(std::strcmp(readBack.ToChar(), "hello") == 0);

        int sliderValue = 0;
        // without the money row at id + 0x100 the call still drives the slider but answers false
        CHECK(!tool->SetSliderControlValue(dialog, 0x1220, 10, 90, 55, 1000, nullptr, nullptr, nullptr, 0));
        CHECK(tool->GetSliderControlValue(dialog, 0x1220, sliderValue));
        CHECK(sliderValue == 55);
        CHECK(slider->GetMinimumValue() == 10 && slider->GetMaximumValue() == 90);
        // the money row of the slider is the static control at id + 0x100
        auto* moneyRow = new gz::cGZWinText(&host.mgr);
        moneyRow->SetID(0x1220 + 0x100);
        CHECK(dialog->ChildAdd(moneyRow));
        CHECK(tool->SetSliderControlValue(dialog, 0x1220, 0, 100, 50, 2000, nullptr, nullptr, nullptr, 0));
        CHECK(std::string(moneyRow->GetCaption().ToChar()) == "1000");   // 2000 * 50% + 0.5, truncated

        dialog->Release();
    }

    // --- H: SetOrGet rows ----------------------------------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* edit = new gz::cGZWinTextEdit(&host.mgr);
        edit->SetID(0x1300);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1310);
        label->HideWindow();
        CHECK(dialog->ChildAdd(edit) && dialog->ChildAdd(label));

        TestOccupant occupant;
        occupant.name = "Rutherford";
        CHECK(tool->SetOrGetName(dialog, 0x1300, true, &occupant));
        cRZString shown;
        shown.AssignFromCString(edit->GetText());
        CHECK(std::strcmp(shown.ToChar(), "Rutherford") == 0);

        // the edited name differs: the building occupant is renamed
        cRZString changed;
        changed.AssignFromCString("New Name");
        tool->SetEditControlText(dialog, 0x1300, changed);
        CHECK(tool->SetOrGetName(dialog, 0x1300, false, &occupant));
        CHECK(occupant.names == 1 && occupant.lastSetName == "New Name");

        // the name back to the occupant's: no rename
        cRZString same;
        same.AssignFromCString("Rutherford");
        tool->SetEditControlText(dialog, 0x1300, same);
        CHECK(tool->SetOrGetName(dialog, 0x1300, false, &occupant));
        CHECK(occupant.names == 1);

        host.power.powered = true;
        host.strings.clear();
        CHECK(tool->SetOrGetOccupantIsPowered(dialog, 0x1310, true, &occupant));
        CHECK(host.strings.size() == 1 && host.strings[0] == 0xca5d4f33);   // "Yes"
        host.plumbing.watered = false;
        host.strings.clear();
        CHECK(tool->SetOrGetOccupantIsWatered(dialog, 0x1310, true, &occupant));
        CHECK(host.strings.size() == 1 && host.strings[0] == 0x6a5d4f37);   // "No"
        CHECK(tool->SetOrGetOccupantIsPowered(dialog, 0x1310, false, &occupant));
        CHECK(tool->SetOrGetOccupantIsWatered(dialog, 0x1310, false, &occupant));

        // the abandoned-reason row uses the lot state and hides the control when there is no reason
        auto* lot = new Lot();
        host.lots.byOccupant = lot;
        lot->state = 4;
        lot->reason = 3;
        CHECK(tool->SetOrGetWhyOccupantAbandoned(dialog, 0x1310, true, &occupant));
        CHECK(label->IsVisible());
        CHECK(std::strcmp(label->GetCaption().ToChar(), "Abandoned: commute") == 0);
        lot->reason = 5;   // no string for reason 5 (quirk: case 5 falls through)
        CHECK(tool->SetOrGetWhyOccupantAbandoned(dialog, 0x1310, true, &occupant));
        CHECK(!label->IsVisible());
        host.lots.byOccupant = nullptr;
        CHECK(!tool->SetOrGetWhyOccupantAbandoned(dialog, 0x1310, false, &occupant));
        delete lot;

        // rating thresholds and the mayor approval row
        cRZString rating;
        host.strings.clear();
        tool->GetRatingIn3DegreesFromPercent(rating, 10.0f);
        CHECK(host.strings.back() == 0x4a5fe266);
        tool->GetRatingIn3DegreesFromPercent(rating, 50.0f);
        CHECK(host.strings.back() == 0xa5fe260);
        tool->GetRatingIn3DegreesFromPercent(rating, 90.0f);
        CHECK(host.strings.back() == 0xaa5fe249);

        auto* mayorLot = new Lot();
        mayorLot->rect.x0 = 0; mayorLot->rect.z0 = 0;
        mayorLot->rect.x1 = 1; mayorLot->rect.z1 = 1;
        host.lots.byCell = mayorLot;
        host.city.cellX = 1; host.city.cellZ = 1;
        host.aura.value = 0;
        const float percent = tool->GetMayorRatingPercent(&occupant);
        CHECK(percent > 49.0f && percent < 51.0f);    // average aura 0 + 0x80 over 4 cells = 50.2%
        host.aura.value = -128;                        // clamps nothing: (0 + 0x80 - 128) = 0
        CHECK(tool->GetMayorRatingPercent(&occupant) == 0.0f);
        host.strings.clear();
        CHECK(tool->SetOrGetResidentialBuildingMayorApproval(dialog, 0x1310, true, &occupant));
        CHECK(host.strings.size() == 1);   // the rating string (the row text itself is a static control)
        host.lots.byCell = nullptr;
        delete mayorLot;

        dialog->Release();
    }

    // --- I: picking and the debug query --------------------------------------------------------------------
    {
        TestOccupant occupant;
        host.renderer.nextObject = static_cast<cISC4Occupant*>(&occupant);
        cISC4Occupant* picked = nullptr;
        CHECK(tool->PickQueriableOccupant(10, 20, &picked, nullptr) && picked == static_cast<cISC4Occupant*>(&occupant));

        Dummy dummy;
        host.renderer.nextObject = &dummy;
        cIGZUnknown* automaton = nullptr;
        CHECK(tool->PickAutomaton(10, 20, &automaton) && automaton == &dummy);
        cIGZUnknown* prop = nullptr;
        CHECK(tool->PickStoplightProp(10, 20, &prop) && prop == &dummy);

        host.renderer.nextObject = nullptr;
        CHECK(!tool->PickQueriableOccupant(10, 20, &picked, nullptr) && picked == nullptr);

        host.debug = false;
        host.keys = {0x10, 0x11, 0x12};
        CHECK(!tool->IsDebugQueryEnabled());
        host.debug = true;
        CHECK(tool->IsDebugQueryEnabled());
        host.keys = {0x10, 0x11};
        CHECK(!tool->IsDebugQueryEnabled());
    }

    // --- J: audio feedback --------------------------------------------------------------------------------
    {
        TestOccupant occupant;
        Holder holder;
        holder.id = 0xaa1dd397;   // the default query sound
        holder.value = true;
        occupant.holder = &holder;
        host.sounds = 0;
        host.soundId = 0;
        tool->PlayOccupantQuerySound(static_cast<cISC4Occupant*>(&occupant));
        CHECK(host.sounds == 1);
        tool->PlayOccupantQuerySound(nullptr);
        CHECK(host.sounds == 1);
    }

    // --- K: Shutdown --------------------------------------------------------------------------------------
    CHECK(tool->Shutdown());
    CHECK(host.messages.removed.size() == 6);
    CHECK(host.view.refs == 1 && host.mgr.refs == 0);
    CHECK(tool->GetCursor() == nullptr);
    CHECK(tool->Shutdown() && host.messages.removed.size() == 6);   // already shut down: no second removal
    tool->Release();

    // --- L: the network flavour -----------------------------------------------------------------------------
    {
        auto* viewWin2 = new cGZWin(&host.mgr);
        host.view.window = viewWin2;
        // the previous tool's Shutdown deleted its callout window, so build a fresh one
        callout = new Callout();
        callout->win = new cGZWin(&host.mgr);
        host.callout = callout;
        auto* legendParent = new cGZWin(&host.mgr);
        auto* minimised = new cGZWin(&host.mgr);
        minimised->SetID(cSC4ViewInputControlQuery::kLegendMinimizedChild);
        auto* expanded = new cGZWin(&host.mgr);
        expanded->SetID(cSC4ViewInputControlQuery::kLegendExpandedChild);
        minimised->SetArea(0, 0, 100, 20);
        expanded->SetArea(0, 0, 200, 40);
        CHECK(legendParent->ChildAdd(minimised) && legendParent->ChildAdd(expanded));
        legendParent->AddRef();   // the tool takes and releases its own reference to the script window
        host.legendParent = legendParent;
        host.legendInstances.clear();
        auto* net = new cSC4ViewInputControlQuery(cSC4ViewInputControlQuery::kQueryNetwork, 100);
        CHECK(net->Init());
        CHECK(host.legendInstances.size() == 1);   // instance 0xabb0120f
        CHECK(host.attached == 2 && minimised->IsVisible() && !expanded->IsVisible());
        net->SetLegendMinimized(true);
        CHECK(!minimised->IsVisible() && !expanded->IsVisible());
        net->SetLegendMinimized(false);
        CHECK(minimised->IsVisible() && expanded->IsVisible());
        CHECK(net->ShouldStack() == false);
        net->Release();
        host.legendParent = nullptr;
        delete legendParent;   // owns both legend children
    }

    // --- M: fire / police / jail / park / occupant-cost rows ------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        auto* meter = new gz::cGZWinBMP(&host.mgr);
        meter->SetID(0x1001);
        auto* slider = new gz::cGZWinSlider(&host.mgr);
        slider->SetID(0x1002);
        slider->SetMinimumValue(0);   // the dialog script gives the funding slider its 0..100 range
        slider->SetMaximumValue(100);
        auto* moneyRow = new gz::cGZWinText(&host.mgr);
        moneyRow->SetID(0x1102);   // the slider's amount row is id + 0x100
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(meter) && dialog->ChildAdd(slider));
        CHECK(dialog->ChildAdd(moneyRow));

        TestOccupant occupant;

        // effectiveness goes through the performance-meter parameter
        host.fireSim.effectiveness = 0.75f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetFireStationEffectiveness(dialog, 0x1001, true, &occupant));
        cIGZVariant* meterValue = nullptr;
        CHECK(meter->AsIGZWin()->GetParam(0xea563ab0, &meterValue) && meterValue);
        CHECK(std::fabs(meterValue->GetValFloat32() - 0.75f) < 1e-6f);
        if (meterValue) meterValue->Release();
        // every row is set-only: clearing leaves the control alone and answers true
        CHECK(cSC4ViewInputControlQuery::SetOrGetFireStationEffectiveness(dialog, 0x1001, false, &occupant));

        // engine count is the dispatch interface's third out value
        host.fireSim.dispatchCount = 4;
        host.fireSim.dispatchOk = true;
        CHECK(cSC4ViewInputControlQuery::SetOrGetFireStationEngineCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "4");
        host.fireSim.dispatchOk = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetFireStationEngineCount(dialog, 0x1007, true, &occupant));

        // local funding: the slider is the percentage, the amount row the cost
        host.fireSim.fundingCost = 500;
        host.fireSim.fundingFraction = 0.4f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetFireStationLocalFunding(dialog, 0x1002, true, &occupant));
        int sliderValue = 0;
        CHECK(cSC4ViewInputControlQuery::GetSliderControlValue(dialog, 0x1002, sliderValue) && sliderValue == 40);
        CHECK(std::string(moneyRow->GetCaption().ToChar()) == "200");   // 40% of 500
        // ... and writing it back scales the slider by 0.01
        host.fireSim.setFundingCalls = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetFireStationLocalFunding(dialog, 0x1002, false, &occupant));
        CHECK(host.fireSim.setFundingCalls == 1 && std::fabs(host.fireSim.setFraction - 0.4f) < 1e-6f);

        // maintenance cost is the same funding call's cost out value
        host.fireSim.fundingCost = 1234;
        CHECK(cSC4ViewInputControlQuery::SetOrGetFireStationMaintenanceCost(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "1234");

        // police effectiveness / arrests / crimes
        host.policeSim.effectiveness = 0.5f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPoliceStationEffectiveness(dialog, 0x1001, true, &occupant));
        meterValue = nullptr;
        CHECK(meter->AsIGZWin()->GetParam(0xea563ab0, &meterValue) && meterValue);
        CHECK(std::fabs(meterValue->GetValFloat32() - 0.5f) < 1e-6f);
        if (meterValue) meterValue->Release();
        host.policeSim.arrests = 12;
        host.policeSim.crimes = 34;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPoliceStationArrestCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "12");
        CHECK(cSC4ViewInputControlQuery::SetOrGetPoliceStationCrimeCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "34");
        host.policeSim.dispatchCount = 3;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPoliceStationPatrolCarCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "3");
        host.policeSim.fundingCost = 800;
        host.policeSim.fundingFraction = 0.25f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPoliceStationLocalFunding(dialog, 0x1002, true, &occupant));
        CHECK(cSC4ViewInputControlQuery::GetSliderControlValue(dialog, 0x1002, sliderValue) && sliderValue == 25);
        CHECK(std::string(moneyRow->GetCaption().ToChar()) == "200");   // 25% of 800
        host.policeSim.setFundingCalls = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPoliceStationLocalFunding(dialog, 0x1002, false, &occupant));
        CHECK(host.policeSim.setFundingCalls == 1 && std::fabs(host.policeSim.setFraction - 0.25f) < 1e-6f);
        host.policeSim.fundingCost = 77;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPoliceStationMaintenanceCost(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "77");

        // the jail: cells are 0.8 * capacity, inmates capacity * efficiency, and the meter fills the band above 0.8
        host.policeSim.jailCapacity = 10;
        host.policeSim.jailEfficiency = 0.5f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetJailCellCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "8");
        CHECK(cSC4ViewInputControlQuery::SetOrGetJailInmateCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "5");
        CHECK(cSC4ViewInputControlQuery::SetOrGetJailCondition(dialog, 0x1001, true, &occupant));
        meterValue = nullptr;
        CHECK(meter->AsIGZWin()->GetParam(0xea563ab0, &meterValue) && meterValue);
        CHECK(std::fabs(meterValue->GetValFloat32() - 1.0f) < 1e-6f);   // at or below the design capacity
        if (meterValue) meterValue->Release();
        host.policeSim.jailEfficiency = 0.9f;    // 9 inmates, one free cell: 1 / (10 * 0.2)
        CHECK(cSC4ViewInputControlQuery::SetOrGetJailCondition(dialog, 0x1001, true, &occupant));
        meterValue = nullptr;
        CHECK(meter->AsIGZWin()->GetParam(0xea563ab0, &meterValue) && meterValue);
        CHECK(std::fabs(meterValue->GetValFloat32() - 0.5f) < 1e-5f);
        if (meterValue) meterValue->Release();
        host.policeSim.jailEfficiency = 1.0f;    // completely full: no free cell, meter 0
        CHECK(cSC4ViewInputControlQuery::SetOrGetJailCondition(dialog, 0x1001, true, &occupant));
        meterValue = nullptr;
        CHECK(meter->AsIGZWin()->GetParam(0xea563ab0, &meterValue) && meterValue);
        CHECK(meterValue->GetValFloat32() == 0.0f);
        if (meterValue) meterValue->Release();
        host.policeSim.jailCapacity = 0;         // no jail at all
        CHECK(cSC4ViewInputControlQuery::SetOrGetJailCondition(dialog, 0x1001, true, &occupant));
        meterValue = nullptr;
        CHECK(meter->AsIGZWin()->GetParam(0xea563ab0, &meterValue) && meterValue);
        CHECK(meterValue->GetValFloat32() == 0.0f);
        if (meterValue) meterValue->Release();
        host.policeSim.jailUpkeep = 750;
        CHECK(cSC4ViewInputControlQuery::SetOrGetJailMaintenanceCost(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "750");

        // the park row is the generic occupant-cost row: one budget lookup per property value
        Holder holder;
        holder.id = 0xea54d285;
        holder.values = {1, 2};
        occupant.holder = &holder;
        host.budgetSim.calls = 0;
        host.budgetSim.ok = true;
        host.budgetSim.cost = 250;
        CHECK(cSC4ViewInputControlQuery::SetOrGetParkMaintenanceCost(dialog, 0x1007, true, &occupant));
        CHECK(host.budgetSim.calls == 2);
        CHECK(std::string(label->GetCaption().ToChar()) == "500");
        // a failed lookup contributes nothing
        host.budgetSim.ok = false;
        CHECK(cSC4ViewInputControlQuery::SetOrGetOccupantCost(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "0");
        // no property vector at all: the row declines
        holder.id = 0;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetOccupantCost(dialog, 0x1007, true, &occupant));
        CHECK(cSC4ViewInputControlQuery::SetOrGetOccupantCost(dialog, 0x1007, false, &occupant));   // set-only

        dialog->Release();
    }

    // --- M2: airport / seaport rows -----------------------------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        auto* meter = new gz::cGZWinBMP(&host.mgr);
        meter->SetID(0x1001);
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(meter));
        TestOccupant occupant;   // its first city cell is (3, 4)
        Lot lot;
        host.lots.byCell = &lot;
        auto meterOf = [&]() {
            cIGZVariant* value = nullptr;
            float result = -1.0f;
            if (meter->AsIGZWin()->GetParam(0xea563ab0, &value) && value) { result = value->GetValFloat32(); value->Release(); }
            return result;
        };
        // usage and capacity print the developer's numbers; reading (set = false) does nothing and answers true
        host.airport.usage = 30; host.airport.capacity = 120;
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportOccupantUsage(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "30" && host.airport.lastLot == &lot);
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportOccupantCapacity(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "120");
        host.seaport.usage = 7; host.seaport.capacity = 9;
        CHECK(cSC4ViewInputControlQuery::SetOrGetSeaportOccupantUsage(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "7");
        CHECK(cSC4ViewInputControlQuery::SetOrGetSeaportCapacity(dialog, 0x1007, true, 3, 4));
        CHECK(std::string(label->GetCaption().ToChar()) == "9");
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportUsage(dialog, 0x1007, false, 3, 4));
        // no lot under the cell, or no occupant: false
        host.lots.byCell = nullptr;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetAirportUsage(dialog, 0x1007, true, 3, 4));
        CHECK(!cSC4ViewInputControlQuery::SetOrGetAirportOccupantUsage(dialog, 0x1007, true, nullptr));
        host.lots.byCell = &lot;
        // maintenance is a simoleon amount
        host.airport.maintenance = 1500;
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportOccupantMaintenanceCost(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "1500");   // the fake host formats money as a bare number
        host.seaport.maintenance = 42;
        CHECK(cSC4ViewInputControlQuery::SetOrGetSeaportMaintenanceCost(dialog, 0x1007, true, 3, 4));
        CHECK(std::string(label->GetCaption().ToChar()).find("42") != std::string::npos);
        // traffic meter: full up to 80 % use, then (capacity - usage) / (capacity * 0.2), empty at or over capacity
        host.airport.capacity = 100;
        host.airport.usage = 80;
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportOccupantAirTrafficVolume(dialog, 0x1001, true, &occupant));
        CHECK(std::fabs(meterOf() - 1.0f) < 1e-6f);
        host.airport.usage = 90;
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportAirTrafficVolume(dialog, 0x1001, true, 3, 4));
        CHECK(std::fabs(meterOf() - 10.0f / (100.0f * 0.19999999f)) < 1e-6f);
        host.airport.usage = 150;
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportAirTrafficVolume(dialog, 0x1001, true, 3, 4));
        CHECK(std::fabs(meterOf()) < 1e-6f);
        host.seaport.capacity = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetSeaportSeaTrafficVolume(dialog, 0x1001, true, 3, 4));
        CHECK(std::fabs(meterOf()) < 1e-6f);
        host.airport.answers = false;   // a developer that has nothing for the lot leaves the meter empty
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportAirTrafficVolume(dialog, 0x1001, true, 3, 4));
        CHECK(std::fabs(meterOf()) < 1e-6f);
        host.airport.answers = true;
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportAirTrafficVolume(dialog, 0x1001, false, 3, 4));   // reading answers true
        // the dispatcher routes the cell rows by control id and always answers false
        cSC4ViewInputControlQuery::tCellInfo info;
        info.x = 3; info.z = 4;
        host.airport.usage = 12; host.airport.capacity = 50;
        auto* usageLabel = new gz::cGZWinText(&host.mgr);
        usageLabel->SetID(0x107b);
        CHECK(dialog->ChildAdd(usageLabel));
        CHECK(!cSC4ViewInputControlQuery::SetOrGetControlValue(dialog, 0x107b, true, &info));
        CHECK(std::string(usageLabel->GetCaption().ToChar()) == "12");
        host.lots.byCell = nullptr;
        delete dialog;
    }

    // --- M3: school / hospital / library rows --------------------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        auto* meter = new gz::cGZWinBMP(&host.mgr);
        meter->SetID(0x1001);
        auto* staffSlider = new gz::cGZWinSlider(&host.mgr);
        staffSlider->SetID(0x1002);
        auto* busSlider = new gz::cGZWinSlider(&host.mgr);
        busSlider->SetID(0x1003);
        staffSlider->SetMinimumValue(0);   // the dialog script gives the sliders their 0..100 range
        staffSlider->SetMaximumValue(100);
        busSlider->SetMinimumValue(0);
        busSlider->SetMaximumValue(100);
        auto* staffMoney = new gz::cGZWinText(&host.mgr);
        staffMoney->SetID(0x1102);
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(meter) && dialog->ChildAdd(staffSlider) && dialog->ChildAdd(busSlider));
        CHECK(dialog->ChildAdd(staffMoney));
        TestOccupant occupant;
        auto meterOf = [&]() {
            cIGZVariant* value = nullptr;
            float result = -1.0f;
            if (meter->AsIGZWin()->GetParam(0xea563ab0, &value) && value) { result = value->GetValFloat32(); value->Release(); }
            return result;
        };
        // grade meters read the record's grade; clearing answers true; no data answers false
        host.residential.school.grade = 0.6f;
        host.residential.hospital.grade = 0.3f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolGrade(dialog, 0x1001, true, &occupant));
        CHECK(std::fabs(meterOf() - 0.6f) < 1e-6f);
        CHECK(cSC4ViewInputControlQuery::SetOrGetHospitalGrade(dialog, 0x1001, true, &occupant));
        CHECK(std::fabs(meterOf() - 0.3f) < 1e-6f);
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolGrade(dialog, 0x1001, false, &occupant));
        host.residential.schoolAnswers = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetSchoolGrade(dialog, 0x1001, true, &occupant));
        CHECK(!cSC4ViewInputControlQuery::SetOrGetSchoolStudentCount(dialog, 0x1007, true, &occupant));
        host.residential.schoolAnswers = true;
        // counts come from the record; the library reads the school record, the hospital its own
        host.residential.school.count = 321;
        host.residential.hospital.count = 77;
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolStudentCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "321");
        CHECK(cSC4ViewInputControlQuery::SetOrGetLibraryBooksBorrowedCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "321");
        CHECK(cSC4ViewInputControlQuery::SetOrGetHospitalPatientCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "77");
        // capacities: the first value is desks / beds / books / tickets, the second teachers / doctors; the simulator is asked even when clearing
        host.residential.first = 40;
        host.residential.second = 6;
        host.residential.capacityCalls = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolDeskCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "40");
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolTeacherCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "6");
        CHECK(cSC4ViewInputControlQuery::SetOrGetMuseumExhibitTicketsAvailable(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "40");
        CHECK(cSC4ViewInputControlQuery::SetOrGetLibraryBooksInCollectionCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "40");
        CHECK(cSC4ViewInputControlQuery::SetOrGetHospitalBedCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "40");
        CHECK(cSC4ViewInputControlQuery::SetOrGetHospitalDoctorCount(dialog, 0x1007, true, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "6");
        const int callsBefore = host.residential.capacityCalls;
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolDeskCount(dialog, 0x1007, false, &occupant));
        CHECK(host.residential.capacityCalls == callsBefore + 1);
        // funding sliders: the fraction is rounded to the nearest percent (0.755 -> 76, not 75), the amount row follows
        host.residential.school.staffFunding = 0.755f;
        host.residential.school.staffCost = 1000;
        host.residential.school.secondFunding = 0.5f;
        host.residential.school.secondCost = 400;
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolLocalStaffFunding(dialog, 0x1002, true, &occupant));
        int sliderValue = 0;
        CHECK(cSC4ViewInputControlQuery::GetSliderControlValue(dialog, 0x1002, sliderValue) && sliderValue == 76);
        CHECK(std::string(staffMoney->GetCaption().ToChar()) == "760");
        // moving the staff slider keeps the bus fraction from the record; moving the bus slider keeps the staff one
        busSlider->SetValue(30);
        staffSlider->SetValue(20);
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolLocalStaffFunding(dialog, 0x1002, false, &occupant));
        CHECK(host.residential.fundingCalls == 1 && host.residential.lastEducation);
        CHECK(std::fabs(host.residential.lastStaff - 0.2f) < 1e-6f && std::fabs(host.residential.lastSecond - 0.5f) < 1e-6f);
        CHECK(cSC4ViewInputControlQuery::SetOrGetSchoolLocalBusFunding(dialog, 0x1003, false, &occupant));
        CHECK(host.residential.fundingCalls == 2);
        CHECK(std::fabs(host.residential.lastStaff - 0.755f) < 1e-6f && std::fabs(host.residential.lastSecond - 0.3f) < 1e-6f);
        // the hospital stores with the education flag off
        host.residential.hospital.staffFunding = 0.25f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetHospitalLocalAmbulanceFunding(dialog, 0x1003, false, &occupant));
        CHECK(host.residential.fundingCalls == 3 && !host.residential.lastEducation);
        CHECK(std::fabs(host.residential.lastStaff - 0.25f) < 1e-6f);
        // no data for the occupant: false and nothing stored
        host.residential.hospitalAnswers = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetHospitalLocalStaffFunding(dialog, 0x1002, false, &occupant));
        CHECK(host.residential.fundingCalls == 3);
        host.residential.hospitalAnswers = true;
        delete dialog;
    }

    // --- M4: landfill / incinerator / recycling / power rows -------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        auto* meter = new gz::cGZWinBMP(&host.mgr);
        meter->SetID(0x1001);
        auto* slider = new gz::cGZWinSlider(&host.mgr);
        slider->SetID(0x1002);
        slider->SetMinimumValue(0);
        slider->SetMaximumValue(100);
        auto* money = new gz::cGZWinText(&host.mgr);
        money->SetID(0x1102);
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(meter) && dialog->ChildAdd(slider) && dialog->ChildAdd(money));
        TestOccupant occupant;
        auto meterOf = [&]() {
            cIGZVariant* value = nullptr;
            float result = -1.0f;
            if (meter->AsIGZWin()->GetParam(0xea563ab0, &value) && value) { result = value->GetValFloat32(); value->Release(); }
            return result;
        };
        auto text = [&]() { return std::string(label->GetCaption().ToChar()); };
        // landfill: usage meter (full to 80 %, empty at capacity), the month's trash, the developer's budget through the dispatcher
        host.landfill.capacity = 1000;
        host.landfill.usage = 800;
        CHECK(cSC4ViewInputControlQuery::SetOrGetLandfillUsage(dialog, 0x1001, true, 0, 0) && std::fabs(meterOf() - 1.0f) < 1e-6f);
        host.landfill.usage = 900;
        CHECK(cSC4ViewInputControlQuery::SetOrGetLandfillUsage(dialog, 0x1001, true, 0, 0));
        CHECK(std::fabs(meterOf() - 100.0f / (1000.0f * 0.19999999f)) < 1e-6f);
        host.landfill.capacity = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetLandfillUsage(dialog, 0x1001, true, 0, 0) && std::fabs(meterOf()) < 1e-6f);
        host.pollution.landfilled = 250;
        host.pollution.scale = 2.5f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetLandfillNetTrashThisMonth(dialog, 0x1007, true, 0, 0) && text() == "625");
        cSC4ViewInputControlQuery::tCellInfo info;
        host.landfill.budget = 321;
        auto* budgetLabel = new gz::cGZWinText(&host.mgr);
        budgetLabel->SetID(0x1028);
        CHECK(dialog->ChildAdd(budgetLabel));
        CHECK(!cSC4ViewInputControlQuery::SetOrGetControlValue(dialog, 0x1028, true, &info));
        CHECK(std::string(budgetLabel->GetCaption().ToChar()) == "321");
        // incinerator: efficiency as whole percent on the meter, used over capacity, trash scaled by the garbage factor
        host.pollution.efficiency = 0.876f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetIncineratorEfficiency(dialog, 0x1001, true, &occupant) && std::fabs(meterOf() - 87.0f) < 1e-6f);
        host.pollution.incUsed = 30;
        host.pollution.incCurrent = 120;
        CHECK(cSC4ViewInputControlQuery::SetOrGetIncineratorPercentCapacity(dialog, 0x1007, true, &occupant) && text() == "25%");
        host.pollution.incCurrent = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetIncineratorPercentCapacity(dialog, 0x1007, true, &occupant) && text() == "0%");
        host.pollution.scale = 1.5f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetIncineratorTrashIncinerated(dialog, 0x1007, true, &occupant) && text() == "45");
        host.pollution.wteUsed = 10;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWasteToEnergyTrashIncinerated(dialog, 0x1007, true, &occupant) && text() == "15");
        // recycling: current over maximum (at most 1), the recycled share over the current capacity, the centres' share
        host.pollution.maxCapacity = 200.0f;
        host.pollution.currentCapacity = 50.0f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRecyclingCenterEfficiency(dialog, 0x1001, true, &occupant) && std::fabs(meterOf() - 0.25f) < 1e-6f);
        host.pollution.currentCapacity = 400.0f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRecyclingCenterEfficiency(dialog, 0x1001, true, &occupant) && std::fabs(meterOf() - 1.0f) < 1e-6f);
        host.pollution.currentCapacity = 0.0f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRecyclingCenterEfficiency(dialog, 0x1001, true, &occupant) && std::fabs(meterOf()) < 1e-6f);
        host.pollution.produced = 1000;
        host.pollution.recycled = 100;
        host.pollution.currentCapacity = 0.5f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRecyclingCenterPercentCapacity(dialog, 0x1007, true, &occupant) && text() == "20%");   // 0.1 / 0.5
        host.pollution.currentCapacity = 0.05f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRecyclingCenterPercentCapacity(dialog, 0x1007, true, &occupant) && text() == "100%");   // clamped
        host.pollution.centres = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRecyclingCenterTrashRecycled(dialog, 0x1007, true, &occupant) && text() == "0");
        host.pollution.centres = 4;
        host.pollution.scale = 2.0f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRecyclingCenterTrashRecycled(dialog, 0x1007, true, &occupant) && text() == "50");   // 2 * 100 / 4
        // power plants and their waste-to-energy twins
        host.power.efficiency = 0.6f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantEfficiency(dialog, 0x1001, true, &occupant) && std::fabs(meterOf() - 0.6f) < 1e-6f);
        host.power.efficiency = 0.9f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWasteToEnergyEfficiency(dialog, 0x1001, true, &occupant) && std::fabs(meterOf() - 0.9f) < 1e-6f);
        host.power.capacity = 400;
        host.power.used = 100;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantCurrentPowerOutputCapacity(dialog, 0x1007, true, &occupant) && text() == "400");
        CHECK(cSC4ViewInputControlQuery::SetOrGetWasteToEnergyPowerUsed(dialog, 0x1007, true, &occupant) && text() == "100");
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantPercentCapacity(dialog, 0x1007, true, &occupant) && text() == "25%");
        host.power.capacity = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWasteToEnergyPercentCapacity(dialog, 0x1007, true, &occupant) && text() == "0%");
        host.power.maxProduced = 750;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantMaxPowerOutputCapacity(dialog, 0x1007, true, &occupant) && text() == "750");
        host.power.condition = 0.876f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantPercentCondition(dialog, 0x1007, true, &occupant) && text() == "87%");
        host.power.capacity = 400;
        host.power.monthlyCost = 1000;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantCostPerUnit(dialog, 0x1007, true, &occupant));   // 1000 / 400 = 2.5
        CHECK(text().find("2.50") != std::string::npos);   // the fake host's float money formatter appends the amount to a fixed prefix
        // the funding slider: the plant's fraction rounded to a percent, its monthly cost; moving it stores the fraction
        host.power.funding = 0.755f;
        host.power.monthlyCost = 2000;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantLocalFunding(dialog, 0x1002, true, &occupant));
        int sliderValue = 0;
        CHECK(cSC4ViewInputControlQuery::GetSliderControlValue(dialog, 0x1002, sliderValue) && sliderValue == 76);
        host.power.fundingOk = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetPowerPlantLocalFunding(dialog, 0x1002, true, &occupant));
        host.power.fundingOk = true;
        host.power.costOk = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetPowerPlantLocalFunding(dialog, 0x1002, true, &occupant));
        host.power.costOk = true;
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantLocalFunding(dialog, 0x1002, false, &occupant));
        CHECK(host.power.setCalls == 1 && std::fabs(host.power.setFunding - 0.76f) < 1e-6f);
        // every row is set-only: clearing answers true
        CHECK(cSC4ViewInputControlQuery::SetOrGetPowerPlantEfficiency(dialog, 0x1001, false, &occupant));
        CHECK(cSC4ViewInputControlQuery::SetOrGetRecyclingCenterTrashRecycled(dialog, 0x1007, false, &occupant));
        delete dialog;
    }

    // --- M5: water building rows ---------------------------------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        auto* meter = new gz::cGZWinBMP(&host.mgr);
        meter->SetID(0x1001);
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(meter));
        TestOccupant occupant;
        auto text = [&]() { return std::string(label->GetCaption().ToChar()); };
        host.plumbing.current = 200;
        host.plumbing.maximum = 500;
        host.plumbing.used = 50;
        host.plumbing.efficiency = 0.8f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingCurrentCapacity(dialog, 0x1007, true, &occupant) && text() == "200");
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingMaxCapacity(dialog, 0x1007, true, &occupant) && text() == "500");
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingWaterUsed(dialog, 0x1007, true, &occupant) && text() == "50");
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingUsagePercent(dialog, 0x1007, true, &occupant) && text() == "25%");
        host.plumbing.current = 0;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingUsagePercent(dialog, 0x1007, true, &occupant) && text() == "0%");
        host.plumbing.current = 200;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingEfficiency(dialog, 0x1001, true, &occupant));
        cIGZVariant* value = nullptr;
        CHECK(meter->AsIGZWin()->GetParam(0xea563ab0, &value) && value && std::fabs(value->GetValFloat32() - 0.8f) < 1e-6f);
        if (value) value->Release();
        host.plumbing.efficiency = 0.5f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterTreatmentPlantEfficiency(dialog, 0x1001, true, &occupant));   // forwards to the building row
        value = nullptr;
        CHECK(meter->AsIGZWin()->GetParam(0xea563ab0, &value) && value && std::fabs(value->GetValFloat32() - 0.5f) < 1e-6f);
        if (value) value->Release();
        // cost per unit: the monthly upkeep over the current capacity
        host.plumbing.upkeep = 1000;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingCostPerUnit(dialog, 0x1007, true, &occupant));
        CHECK(text().find("5.00") != std::string::npos);
        host.plumbing.upkeepOk = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetWaterBuildingCostPerUnit(dialog, 0x1007, true, &occupant));
        host.plumbing.upkeepOk = true;
        // the pump: two words by the pump flag
        host.plumbing.pump = true;
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingPumpCondition(dialog, 0x1007, true, &occupant));
        CHECK(host.strings.size() == 1 && host.strings[0] == 0x0a5d4f43);
        host.plumbing.pump = false;
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingPumpCondition(dialog, 0x1007, true, &occupant));
        CHECK(host.strings.size() == 1 && host.strings[0] == 0x2a5d4f3b);
        // the condition percent: 0 when the simulator has none
        host.plumbing.condition = 0.876f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingPercentCondition(dialog, 0x1007, true, &occupant) && text() == "87%");
        host.plumbing.conditionOk = false;
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingPercentCondition(dialog, 0x1007, true, &occupant) && text() == "0%");
        host.plumbing.conditionOk = true;
        // no producer info: false; clearing: true
        host.plumbing.infoOk = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetWaterBuildingMaxCapacity(dialog, 0x1007, true, &occupant));
        CHECK(!cSC4ViewInputControlQuery::SetOrGetWaterBuildingPumpCondition(dialog, 0x1007, true, &occupant));
        CHECK(cSC4ViewInputControlQuery::SetOrGetWaterBuildingMaxCapacity(dialog, 0x1007, false, &occupant));
        host.plumbing.infoOk = true;
        delete dialog;
    }

    // --- M6: landmark rows and the transit station's global funding ------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        CHECK(dialog->ChildAdd(label));
        TestOccupant occupant;
        Holder holder;
        occupant.holder = &holder;
        auto text = [&]() { return std::string(label->GetCaption().ToChar()); };
        host.budgetSim.departmentFraction = 0.876f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationGlobalFunding(dialog, 0x1007, true, &occupant) && text() == "87%");
        CHECK(host.budgetSim.lastDepartment == 0xc921ceeb);
        host.budgetSim.departmentOk = false;
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationGlobalFunding(dialog, 0x1007, true, &occupant) && text() == "0%");
        host.budgetSim.departmentOk = true;
        host.budgetSim.item.department = 7;
        host.budgetSim.item.cost = 12000;
        CHECK(cSC4ViewInputControlQuery::SetOrGetLandmarkFunding(dialog, 0x1007, true, &occupant) && text() == "12000");
        CHECK(host.budgetSim.lastPurpose == 0xaa59670c);
        host.budgetSim.departmentFraction = 0.5f;
        CHECK(cSC4ViewInputControlQuery::SetOrGetLandmarkMaintenance(dialog, 0x1007, true, &occupant) && text() == "6000");
        CHECK(host.budgetSim.lastDepartment == 7);
        // the homage is the string the key property names
        host.strings.clear();
        CHECK(!cSC4ViewInputControlQuery::SetOrGetLandmarkHomage(dialog, 0x1007, true, &occupant));   // no property
        holder.id = 0xaa5c23fa;
        holder.values = {0x2026bd2b, 0xea5524eb, 0x1234abcd};
        CHECK(cSC4ViewInputControlQuery::SetOrGetLandmarkHomage(dialog, 0x1007, true, &occupant));
        CHECK(host.strings.size() == 1 && host.strings[0] == 0x1234abcd);
        CHECK(cSC4ViewInputControlQuery::SetOrGetLandmarkHomage(dialog, 0x1007, false, &occupant));
        delete dialog;
    }

    // --- M7: transit station rows ---------------------------------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        auto* meter = new gz::cGZWinBMP(&host.mgr);
        meter->SetID(0x1001);
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(meter));
        TestOccupant occupant;
        auto text = [&]() { return std::string(label->GetCaption().ToChar()); };
        auto meterOf = [&]() {
            cIGZVariant* value = nullptr;
            float result = -1.0f;
            if (meter->AsIGZWin()->GetParam(0xea563ab0, &value) && value) { result = value->GetValFloat32(); value->Release(); }
            return result;
        };
        host.city.cellX = 5;
        host.city.cellZ = 9;
        host.trafficSim.transit.served = 90;
        host.trafficSim.transit.capacity = 200;
        host.trafficSim.transit.usage = 120;
        host.trafficSim.transit.maintenance = 4500;
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationMaxCapacity(dialog, 0x1007, true, &occupant) && text() == "200");
        CHECK(host.trafficSim.transitX == 5 && host.trafficSim.transitZ == 9);   // the occupant's position -> city cell
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationActualUsage(dialog, 0x1007, true, &occupant) && text() == "120");
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationMaintenanceCost(dialog, 0x1007, true, &occupant) && text() == "4500");
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationUsagePercent(dialog, 0x1007, true, &occupant) && text() == "60%");
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationServiceQuality(dialog, 0x1001, true, &occupant));
        CHECK(std::fabs(meterOf() - 0.75f) < 1e-6f);   // 90 served over 120 in use
        host.trafficSim.transit.served = 300;           // more served than used: capped at 1
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationServiceQuality(dialog, 0x1001, true, &occupant) && std::fabs(meterOf() - 1.0f) < 1e-6f);
        host.trafficSim.transit.usage = 0;              // nothing in use: full
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationServiceQuality(dialog, 0x1001, true, &occupant) && std::fabs(meterOf() - 1.0f) < 1e-6f);
        host.trafficSim.transit.served = 0;             // nothing served: empty
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationServiceQuality(dialog, 0x1001, true, &occupant) && std::fabs(meterOf()) < 1e-6f);
        host.trafficSim.transit.capacity = 0;           // no capacity: 0 %
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationUsagePercent(dialog, 0x1007, true, &occupant) && text() == "0%");
        // no station at the cell: the figure rows decline, the meter and percent rows show their empty value; clearing answers true
        host.trafficSim.transitOk = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetTransitStationMaxCapacity(dialog, 0x1007, true, &occupant));
        CHECK(!cSC4ViewInputControlQuery::SetOrGetTransitStationMaintenanceCost(dialog, 0x1007, true, &occupant));
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationServiceQuality(dialog, 0x1001, true, &occupant) && std::fabs(meterOf()) < 1e-6f);
        CHECK(cSC4ViewInputControlQuery::SetOrGetTransitStationActualUsage(dialog, 0x1007, false, &occupant));
        host.trafficSim.transitOk = true;
        delete dialog;
    }

    // --- M8: RCI building rows (occupancy, customers, land value, the goofy stat) -------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        CHECK(dialog->ChildAdd(label));
        TestOccupant occupant;
        Lot lot;
        host.lots.byCell = &lot;
        host.city.cellX = 2;
        host.city.cellZ = 3;
        auto text = [&]() { return std::string(label->GetCaption().ToChar()); };
        // capacity: the lot's for the occupant wealth, else the building profile's; 0 without a lot
        lot.capacity = 120;
        CHECK(cSC4ViewInputControlQuery::GetOccupantCapacity(&occupant) == 120);
        lot.capacity = 0;
        occupant.profileCapacity = 75;
        CHECK(cSC4ViewInputControlQuery::GetOccupantCapacity(&occupant) == 75);
        host.lots.byCell = nullptr;
        CHECK(cSC4ViewInputControlQuery::GetOccupantCapacity(&occupant) == 0);
        host.lots.byCell = &lot;
        // occupancy: capacity times the lot's occupancy fraction; 0 for a lot without a building
        lot.capacity = 200;
        lot.occupancy = 0.755f;
        CHECK(cSC4ViewInputControlQuery::GetCurrentOccupancyValue(&occupant) == 151);
        lot.buildingType = 0;
        CHECK(cSC4ViewInputControlQuery::GetCurrentOccupancyValue(&occupant) == 0);
        lot.buildingType = 1;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingCurrentOccupancy(dialog, 0x1007, true, &occupant) && text() == "151/200");
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingMaxOccupancy(dialog, 0x1007, true, &occupant) && text() == "200");
        lot.capacity = 0;
        occupant.profileCapacity = 0;
        label->SetCaption(cRZString("untouched"));
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingMaxOccupancy(dialog, 0x1007, true, &occupant) && text() == "untouched");   // nothing to show
        lot.capacity = 200;
        CHECK(cSC4ViewInputControlQuery::SetOrGetCommercialServiceBuildingCustomers(dialog, 0x1007, true, &occupant) && text() == "75");   // 151 * 100 / 200
        lot.capacity = 0;
        label->SetCaption(cRZString("x"));
        CHECK(cSC4ViewInputControlQuery::SetOrGetCommercialServiceBuildingCustomers(dialog, 0x1007, true, &occupant) && text() == "0");   // capacity 0: infinity -> 0
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingCurrentOccupancy(dialog, 0x1007, false, &occupant));   // set-only
        // land value: three words, anything else the lowest
        host.strings.clear();
        host.landValue.type = 3;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingLandValue(dialog, 0x1007, true, &occupant) && host.strings.back() == 0xaa5fe249);
        host.landValue.type = 2;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingLandValue(dialog, 0x1007, true, &occupant) && host.strings.back() == 0x0a5fe260);
        host.landValue.type = 1;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingLandValue(dialog, 0x1007, true, &occupant) && host.strings.back() == 0x4a5fe266);
        host.landValue.type = 9;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingLandValue(dialog, 0x1007, true, &occupant) && host.strings.back() == 0x4a5fe266);
        CHECK(host.landValue.x == 2 && host.landValue.z == 3);
        // goofy stat: the mayor rating through the curve (identity without a property, a property's points otherwise)
        host.aura.mayorRating = 40;
        Holder none;
        CHECK(cSC4ViewInputControlQuery::SetOrGetGoofyStat(dialog, 0x1007, true, &none, 0xaa60e745) && text() == "40");
        host.aura.mayorRating = -5;
        CHECK(cSC4ViewInputControlQuery::SetOrGetGoofyStat(dialog, 0x1007, true, &none, 0xaa60e745) && text() == "0");   // left of the curve: its first y
        CHECK(cSC4ViewInputControlQuery::SetOrGetGoofyStat(dialog, 0x1007, false, &none, 0xaa60e745));
        host.aura.mayorRating = 40;
        CHECK(cSC4ViewInputControlQuery::SetOrGetOccupantGoofyStat(dialog, 0x1007, true, &occupant) && text() == "40");   // no holder: the default curve
        // the airport lost luggage reads the tuning exemplar {0x6534284a, 0x67cd5fa1, 0xca270e9d}; false without it
        host.tuning = nullptr;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetAirportLostLuggage(dialog, 0x1007, true, 0, 0));
        CHECK(!cSC4ViewInputControlQuery::SetOrGetAirportLostLuggage(dialog, 0x1007, false, 0, 0));   // false even when clearing
        CHECK(host.tuningType == 0x6534284a && host.tuningGroup == 0x67cd5fa1 && host.tuningInstance == 0xca270e9d);
        host.tuning = &none;
        CHECK(cSC4ViewInputControlQuery::SetOrGetAirportLostLuggage(dialog, 0x1007, true, 0, 0) && text() == "40");
        delete dialog;
        host.lots.byCell = nullptr;
    }

    // --- M9: RCI rating rows (the GetSubjective* helpers and the rows that print them) ----------------------------
    {
        auto& dev = host.developer;
        for (int i = 0; i < 9; ++i) dev.present[i] = true;
        for (int k = 0; k < 7; ++k) { dev.lows[k] = 10; dev.highs[k] = 50; }
        int value = 0;
        // pollution: the curves of maps 0, 1 and 3 add up (curve 2 never does); bad 2 with the magnitude, neutral 1, good 0 with half
        host.pollutionMaps[0] = 3; host.pollutionMaps[1] = 4; host.pollutionMaps[2] = 200; host.pollutionMaps[3] = 2;
        CHECK(cSC4ViewInputControlQuery::GetSubjectivePollution(&dev, 0, 0, value) == 2 && value == -9);   // 3 + 4 + 2 = 9 < 10
        host.pollutionMaps[0] = 30;
        CHECK(cSC4ViewInputControlQuery::GetSubjectivePollution(&dev, 0, 0, value) == 1 && value == 0);    // 36
        host.pollutionMaps[0] = 100;
        CHECK(cSC4ViewInputControlQuery::GetSubjectivePollution(&dev, 0, 0, value) == 0 && value == 53);   // 106 / 2
        dev.present[3] = false;
        CHECK(cSC4ViewInputControlQuery::GetSubjectivePollution(&dev, 0, 0, value) == 0 && value == 52);   // 104 / 2
        dev.present[3] = true;
        // garbage: map 2 through curve 2
        host.pollutionMaps[2] = 5;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveGarbage(&dev, 0, 0, value) == 2 && value == -5);
        host.pollutionMaps[2] = 20;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveGarbage(&dev, 0, 0, value) == 1 && value == 0);
        host.pollutionMaps[2] = 80;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveGarbage(&dev, 0, 0, value) == 0 && value == 40);
        dev.present[2] = false;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveGarbage(&dev, 0, 0, value) == 2 && value == 0);   // reading 0 < 10 -> bad with -0
        dev.present[2] = true;
        // school and hospital: bad 0, neutral 1, good 2; no curve is neutral
        host.school = 4.0f;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveSchoolGrade(&dev, 0, 0, value) == 0 && value == -4);
        host.school = 12.0f;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveSchoolGrade(&dev, 0, 0, value) == 1 && value == 0);
        host.school = 90.0f;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveSchoolGrade(&dev, 0, 0, value) == 2 && value == 45);
        dev.present[4] = false;
        value = 77;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveSchoolGrade(&dev, 0, 0, value) == 1 && value == 0);
        host.hospitalGrade = 60.0f;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveHospitalGrade(&dev, 0, 0, value) == 2 && value == 30);
        // crime: bad when high
        host.crime = 7;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveCrimeLevel(&dev, 0, 0, value) == 2 && value == -7);
        host.crime = 30;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveCrimeLevel(&dev, 0, 0, value) == 1 && value == 0);
        host.crime = 200;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveCrimeLevel(&dev, 0, 0, value) == 0 && value == 100);
        // traffic: the curve at the byte and the next byte; the value is only written at the ends
        host.traffic = 5;
        value = 77;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveTrafficLevel(&dev, 0, 0, value) == 0 && value == -5);   // 5 > ... here <= next: 0
        host.traffic = 30;
        value = 77;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveTrafficLevel(&dev, 0, 0, value) == 1 && value == 77);    // middle band leaves value alone
        host.traffic = 100;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveTrafficLevel(&dev, 0, 0, value) == 2 && value == 50);    // here <= next: 2
        dev.present[8] = false;
        value = 77;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveTrafficLevel(&dev, 0, 0, value) == 1 && value == 77);
        dev.present[8] = true;
        // trip length: a zero byte reads 255
        host.tripLength = 3;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveTripLength(&dev, 0, 0, value) == 2 && value == -3);
        host.tripLength = 0;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveTripLength(&dev, 0, 0, value) == 0 && value == 127);   // 255 / 2
        host.tripLength = 20;
        CHECK(cSC4ViewInputControlQuery::GetSubjectiveTripLength(&dev, 0, 0, value) == 1 && value == 0);

        // the rows: the lot of the occupant, its centre cell, the developer, then the string by the rating index
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        CHECK(dialog->ChildAdd(label));
        TestOccupant occupant;
        Lot lot;
        host.lots.byOccupant = &lot;
        host.city.cellX = 1;
        host.city.cellZ = 1;
        host.pollutionMaps[0] = 100; host.pollutionMaps[1] = 0; host.pollutionMaps[3] = 0;
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingPollution(dialog, 0x1007, true, &occupant));
        CHECK(host.strings.size() == 1 && host.strings[0] == 0x4a5fe266);   // rank 0 -> the first general string
        host.crime = 30;
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingCrime(dialog, 0x1007, true, &occupant) && host.strings[0] == 0x0a5fe260);
        host.crime = 7;
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingCrime(dialog, 0x1007, true, &occupant) && host.strings[0] == 0xaa5fe249);   // rank 2
        host.strings.clear();
        host.school = 90.0f; dev.present[4] = true;
        CHECK(cSC4ViewInputControlQuery::SetOrGetResidentialBuildingEQ(dialog, 0x1007, true, &occupant) && host.strings[0] == 0xaa5fe249);
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetResidentialBuildingHQ(dialog, 0x1007, true, &occupant) && host.strings[0] == 0xaa5fe249);   // hospital 60
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingTraffic(dialog, 0x1007, true, &occupant) && host.strings.size() == 1);
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingGarbage(dialog, 0x1007, true, &occupant) && host.strings.size() == 1);
        // commute: its own strings; an abandoned lot (reason 4) has one word of its own
        host.strings.clear();
        host.tripLength = 3;
        CHECK(cSC4ViewInputControlQuery::SetOrGetResidentialBuildingCommute(dialog, 0x1007, true, &occupant) && host.strings[0] == 0x0a71fad3);   // bad = index 2
        lot.reason = 4;
        host.strings.clear();
        host.tripLength = 20;
        CHECK(cSC4ViewInputControlQuery::SetOrGetResidentialBuildingCommute(dialog, 0x1007, true, &occupant) && host.strings[0] == 0x0a71fad3);
        lot.reason = 0;
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetResidentialBuildingCommute(dialog, 0x1007, true, &occupant) && host.strings[0] == 0x6a71fadc);   // neutral
        // no lot or no developer: nothing is loaded; the rows still answer true and are set-only
        host.hasDeveloper = false;
        host.strings.clear();
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingPollution(dialog, 0x1007, true, &occupant) && host.strings.empty());
        host.hasDeveloper = true;
        host.lots.byOccupant = nullptr;
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingPollution(dialog, 0x1007, true, &occupant) && host.strings.empty());
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingPollution(dialog, 0x1007, false, &occupant));
        CHECK(cSC4ViewInputControlQuery::SetOrGetRCIBuildingPollution(dialog, 0x1007, true, nullptr));
        delete dialog;
    }

    // --- M10: tip summaries (prop / flora / network / terrain) -----------------------------------------------------
    {
        auto* tool = new cSC4ViewInputControlQuery(cSC4ViewInputControlQuery::kQueryBuilding, 250);
        TestOccupant occupant;
        Holder holder;
        occupant.holder = &holder;
        tool->SetHoveredOccupantForTest(&occupant);
        // prop / flora: the property 0x899afbad, else the exemplar's localized name, else property 0x20
        cRZString title;
        holder.id = 0;
        host.exemplarOk = true;
        tool->GetPropOccupantSummaryInfo(title);
        CHECK(std::string(title.ToChar()) == "exemplar name" && host.exemplarProperty == 0x8a416a99);
        host.exemplarOk = false;
        cRZString none;
        tool->GetFloraOccupantSummaryInfo(none);
        CHECK(none.Strlen() == 0);
        // network: the first kind the piece is of, in the order road, rail, ...; the pipe network adds the pipe counts in debug mode
        NetworkFake network;
        occupant.network = &network;
        host.strings.clear();
        cRZString netTitle, netText;
        network.types = (1u << 8) | (1u << 6);
        tool->GetNetworkOccupantSummaryInfo(netTitle, netText);
        CHECK(host.strings.size() == 1 && host.strings[0] == 0x8b8c5cbd);   // type 8 comes before type 6
        network.types = 1u << 6;
        host.strings.clear();
        tool->GetNetworkOccupantSummaryInfo(netTitle, netText);
        CHECK(host.strings.size() == 1 && host.strings[0] == 0x8b8c5cd0);
        network.types = 1u << 5;   // a type no row names: nothing loaded
        host.strings.clear();
        tool->GetNetworkOccupantSummaryInfo(netTitle, netText);
        CHECK(host.strings.empty());
        network.types = 1u << 4;
        host.plumbing.pipes = 40;
        host.plumbing.distressedPipes = 3;
        host.debug = true;
        host.keys = {0x10, 0x11, 0x12};
        host.strings.clear();
        tool->GetNetworkOccupantSummaryInfo(netTitle, netText);
        CHECK(host.strings.size() == 1 && host.strings[0] == 0x0a558a0a && std::string(netText.ToChar()) == "Distressed Pipes: 3 / 40\n");
        host.debug = false;
        host.keys.clear();
        // terrain: nothing over water; the zone word goes to the text (to the title for zones 0 and 0xf), a lot adds its headlines
        host.city.cellX = 4;
        host.city.cellZ = 6;
        tool->SetPickPositionForTest(70.0f, 0.0f, 100.0f);
        cRZString terrainTitle, terrainText;
        host.water = true;
        host.strings.clear();
        tool->GetTerrainSummaryInfo(terrainTitle, terrainText);
        CHECK(host.strings.empty());
        host.water = false;
        host.zone = 3;
        tool->GetTerrainSummaryInfo(terrainTitle, terrainText);
        CHECK(host.strings.size() == 2 && host.strings[0] == 0x6a5bbca6 && host.strings[1] == 0x8a5bbe12);
        host.zone = 0;
        host.strings.clear();
        tool->GetTerrainSummaryInfo(terrainTitle, terrainText);
        CHECK(host.strings.size() == 2 && host.strings[1] == 0xaa5bbde4);   // zone 0 replaces the title
        host.zone = 0xff;
        host.strings.clear();
        tool->GetTerrainSummaryInfo(terrainTitle, terrainText);
        CHECK(host.strings.size() == 1);   // an unknown zone: only the terrain title
        tool->Release();
    }

    // --- M11: GetLotHeadlines / GetBuildingWealth ------------------------------------------------------------------
    {
        auto& dev = host.developer;
        for (int i = 0; i < 9; ++i) dev.present[i] = true;
        for (int k = 0; k < 7; ++k) { dev.lows[k] = 10; dev.highs[k] = 50; }
        for (int i = 0; i < 4; ++i) host.pollutionMaps[i] = 30;   // pollution neutral, garbage neutral
        host.school = 30.0f; host.hospitalGrade = 30.0f; host.crime = 30; host.traffic = 30; host.tripLength = 30;
        Lot lot;
        lot.buildingType = 1;
        lot.state = 0;
        lot.wealth = 5;
        host.typeWealth = 4;
        host.city.cellX = 1; host.city.cellZ = 1;
        host.zone = 1;   // residential
        host.hasDeveloper = true;
        std::multimap<int, cRZString> headlines;
        // a calm lot with enough wealth: no headline at all
        cSC4ViewInputControlQuery::GetLotHeadlines(&lot, headlines);
        CHECK(headlines.empty());
        CHECK(cSC4ViewInputControlQuery::GetBuildingWealth(&lot) == 4);
        lot.buildingType = 0;
        lot.wealth = 7;
        CHECK(cSC4ViewInputControlQuery::GetBuildingWealth(&lot) == 7);   // no building type: the occupants' wealth
        lot.buildingType = 1;
        // needs power without it, an abandoned lot (state 4), a wealth below the building's, bad crime and pollution
        lot.needsPower = true; lot.hasPower = false;
        lot.state = 4;
        lot.wealth = 1;
        host.crime = 5;                       // curve 5 < low 10 -> rank 2 (bad for the headline: only rank 0 / 2 verdicts add)
        host.pollutionMaps[0] = 2; host.pollutionMaps[1] = 2; host.pollutionMaps[3] = 2;   // sum 6 < 10 -> rank 2
        host.strings.clear();
        headlines.clear();
        cSC4ViewInputControlQuery::GetLotHeadlines(&lot, headlines);
        std::vector<int> keys;
        for (auto& entry : headlines) keys.push_back(entry.first);
        CHECK(headlines.size() >= 4);
        CHECK(headlines.count(100) == 1 && headlines.count(0) == 1 && headlines.count(1) == 1);   // power, vacated state, wealth
        std::vector<std::uint32_t> loaded = host.strings;
        auto has = [&](std::uint32_t instance) { for (auto v : loaded) if (v == instance) return true; return false; };
        CHECK(has(0x6a5bbba0) && has(0x0a5fbdf2) && has(0x6a5fbed8) && has(0x8a5bbbf1));
        // a lot with a property 0x8999b20b headlines first at key 0x5a; a building type zero forces the power check
        Holder holder;
        holder.id = 0x8999b20b;
        holder.value = true;
        lot.lotHolder = &holder;
        // (the holder answers a bool; the property is read as a number, so it counts when non-zero)
        host.hasDeveloper = false;
        headlines.clear();
        cSC4ViewInputControlQuery::GetLotHeadlines(&lot, headlines);
        CHECK(headlines.empty());   // no growth developer: nothing
    }

    // --- N: neighbour-connection rows ----------------------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        auto* cityRow = new gz::cGZWinText(&host.mgr);
        cityRow->SetID(0x104b);
        auto* mayorRow = new gz::cGZWinText(&host.mgr);
        mayorRow->SetID(0x104c);
        auto* congestionRow = new gz::cGZWinText(&host.mgr);
        congestionRow->SetID(0x104d);
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(cityRow));
        CHECK(dialog->ChildAdd(mayorRow) && dialog->ChildAdd(congestionRow));

        NeighborCityFake city;
        city.cityName = "Neighbourville";
        city.mayorName = "Mayor Bob";
        host.neighborsSim.validCell = true;
        host.neighborsSim.hasConnection = true;
        host.neighborsSim.city = &city;

        CHECK(cSC4ViewInputControlQuery::SetOrGetNeighborConnectionNeighborCityName(dialog, 0x1007, true, 5, 6, 2));
        CHECK(std::string(label->GetCaption().ToChar()) == "Neighbourville");
        CHECK(host.neighborsSim.lastType == 2 && host.neighborsSim.lastX == 5 && host.neighborsSim.lastZ == 6);
        CHECK(cSC4ViewInputControlQuery::SetOrGetNeighborConnectionNeighborCityMayorName(dialog, 0x1007, true, 5, 6, 2));
        CHECK(std::string(label->GetCaption().ToChar()) == "Mayor Bob");

        // the connection lookup runs first: an invalid cell or a missing connection means "no city"
        host.neighborsSim.validCell = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetNeighborConnectionNeighborCityMayorName(dialog, 0x1007, true, 5, 6, 2));
        host.neighborsSim.validCell = true;
        host.neighborsSim.hasConnection = false;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetNeighborConnectionNeighborCityMayorName(dialog, 0x1007, true, 5, 6, 2));
        host.neighborsSim.hasConnection = true;
        // the city-name row still writes the placeholder text and answers true
        host.neighborsSim.city = nullptr;
        CHECK(cSC4ViewInputControlQuery::SetOrGetNeighborConnectionNeighborCityName(dialog, 0x1007, true, 5, 6, 2));
        CHECK(std::string(label->GetCaption().ToChar()) == "Loaded");   // the fake host's placeholder
        host.neighborsSim.city = &city;
        CHECK(cSC4ViewInputControlQuery::SetOrGetNeighborConnectionNeighborCityName(dialog, 0x1007, false, 5, 6, 2));

        // congestion: a byte scaled to a percentage and clamped to the city's cell counts
        host.city.cellsX = 64;
        host.city.cellsZ = 48;
        host.trafficSim.cell = 51;   // 20%
        CHECK(cSC4ViewInputControlQuery::SetOrGetNeighborConnectionCongestion(dialog, 0x1007, true, 5, 6));
        CHECK(std::string(label->GetCaption().ToChar()) == "20%");
        CHECK(host.trafficSim.x == 5 && host.trafficSim.z == 6);
        CHECK(cSC4ViewInputControlQuery::SetOrGetNeighborConnectionCongestion(dialog, 0x1007, true, -4, 1000));
        CHECK(host.trafficSim.x == 0 && host.trafficSim.z == 47);
        host.trafficSim.cell = 255;
        CHECK(cSC4ViewInputControlQuery::SetOrGetNeighborConnectionCongestion(dialog, 0x1007, true, 63, 47));
        CHECK(std::string(label->GetCaption().ToChar()) == "100%");
        CHECK(cSC4ViewInputControlQuery::SetOrGetNeighborConnectionCongestion(dialog, 0x1007, false, 5, 6));

        // the cell-based dispatcher picks the row by id and adds the cell info's id offset
        cSC4ViewInputControlQuery::tCellInfo info;
        info.x = 5;
        info.z = 6;
        info.idOffset = 0;
        info.connectionType = 2;
        host.trafficSim.cell = 26;   // 10%
        CHECK(!cSC4ViewInputControlQuery::SetOrGetControlValue(dialog, 0x104d, true, &info));
        CHECK(std::string(congestionRow->GetCaption().ToChar()) == "10%");
        CHECK(!cSC4ViewInputControlQuery::SetOrGetControlValue(dialog, 0x104b, true, &info));
        CHECK(std::string(cityRow->GetCaption().ToChar()) == "Neighbourville");
        CHECK(!cSC4ViewInputControlQuery::SetOrGetControlValue(dialog, 0x104c, true, &info));
        CHECK(std::string(mayorRow->GetCaption().ToChar()) == "Mayor Bob");
        // the row id is offset by the cell info (used to address several neighbour panels)
        info.idOffset = 0x100;
        CHECK(!cSC4ViewInputControlQuery::SetOrGetControlValue(dialog, 0x104d, true, &info));   // 0x114d: no control
        CHECK(!cSC4ViewInputControlQuery::SetOrGetControlValue(dialog, 0x1011, true, &info));   // no row for the airport ids yet
        CHECK(!cSC4ViewInputControlQuery::SetOrGetControlValue(dialog, 0x104b, true, nullptr));

        dialog->Release();
    }

    // --- O: the row enumeration callbacks ------------------------------------------------------------------
    {
        auto* dialog = new cGZWin(&host.mgr);
        auto* label = new gz::cGZWinText(&host.mgr);
        label->SetID(0x1007);
        auto* slider = new gz::cGZWinSlider(&host.mgr);
        slider->SetID(0x1002);
        slider->SetMinimumValue(0);
        slider->SetMaximumValue(100);
        auto* cityRow = new gz::cGZWinText(&host.mgr);
        cityRow->SetID(0x104b);
        CHECK(dialog->ChildAdd(label) && dialog->ChildAdd(slider) && dialog->ChildAdd(cityRow));

        TestOccupant occupant;
        // EnumFuncSetValue applies the row named by the child id (through the 0x7f-entry table)
        host.policeSim.arrests = 7;
        CHECK(cSC4ViewInputControlQuery::EnumFuncSetValue(dialog, 0x1007, label, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "7");
        // a null table entry falls through to the child enumeration (no child answers 0x22ba0121 here)
        CHECK(cSC4ViewInputControlQuery::EnumFuncSetValue(dialog, 0x1040, label, &occupant));
        CHECK(cSC4ViewInputControlQuery::EnumFuncSetValue(dialog, 0x1040, nullptr, &occupant));
        // EnumFuncGetValue calls the same row with set = false: the slider's percentage goes back to the sim
        host.fireSim.fundingFraction = 0.75f;   // control 0x1002 is the *fire* station funding row
        CHECK(cSC4ViewInputControlQuery::SetOrGetFireStationLocalFunding(dialog, 0x1002, true, &occupant));
        host.fireSim.setFundingCalls = 0;
        CHECK(cSC4ViewInputControlQuery::EnumFuncGetValue(dialog, 0x1002, slider, &occupant));
        CHECK(host.fireSim.setFundingCalls == 1 && std::fabs(host.fireSim.setFraction - 0.75f) < 1e-6f);
        // EnumFuncUpdateValue: a plain window is updated like SetValue (id 0x1007 is the arrest-count row)
        host.policeSim.arrests = 19;
        CHECK(cSC4ViewInputControlQuery::EnumFuncUpdateValue(dialog, 0x1007, label, &occupant));
        CHECK(std::string(label->GetCaption().ToChar()) == "19");

        // SetFilter installs the filter on the child; ClearFilter removes it and drops the five parameters
        WinFilterFake filter;
        CHECK(cSC4ViewInputControlQuery::EnumFuncSetFilter(dialog, 0, label, &filter));
        CHECK(filter.refs == 2);   // the child holds one reference
        auto* paramValue = new cRZVariant();
        paramValue->SetValUint32(3);
        label->SetParam(0xea563ab0, paramValue);   // the window takes the only counted reference
        cIGZVariant* stored = nullptr;
        CHECK(label->GetParam(0xea563ab0, &stored) && stored);
        if (stored) stored->Release();
        CHECK(cSC4ViewInputControlQuery::EnumFuncClearFilter(dialog, 0, label, &filter));
        CHECK(filter.refs == 1);
        CHECK(!label->GetParam(0xea563ab0, &stored));

        // the neighbour-dialog wrappers are the same dispatcher with the cell info as context
        NeighborCityFake city;
        city.cityName = "Across";
        host.neighborsSim.validCell = true;
        host.neighborsSim.hasConnection = true;
        host.neighborsSim.city = &city;
        cSC4ViewInputControlQuery::tCellInfo info;
        info.x = 1;
        info.z = 2;
        info.idOffset = 0;
        info.connectionType = 1;
        CHECK(cSC4ViewInputControlQuery::EnumFuncNeighborDialogSetValue(dialog, 0x104b, &info, nullptr));
        CHECK(std::string(cityRow->GetCaption().ToChar()) == "Across");
        CHECK(cSC4ViewInputControlQuery::EnumFuncNeighborDialogGetValue(dialog, 0x104b, &info, nullptr));

        dialog->Release();
    }

    // window 0/1 of the tool may already be gone; the view windows are still alive
    std::printf(failures ? "FAILED (%d)\n" : "sc4_viewinputcontrolquery ok\n", failures);
    return failures ? 1 : 0;
}

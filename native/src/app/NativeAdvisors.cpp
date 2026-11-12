// The advisor system's environment on the native application: the hosts of cSC4AdvisorSystem, cSC4Advisor, cSC4Advice,
// cSC4AdviceSubject, cSC4RewardAdvice and the concrete advisors, and the advice API the scripts call (nsAdviceAPI's
// `game.*` functions, OpenSC4 advisors.md 8 and lua-game-extensions.md). Before this every one of those hosts was a
// generated stub: the system had no Lua, so no advisor was ever created and no advice evaluated.
//
// The advisor window (cSC4AdvisorUIAdvisorWinProc) is created here too, over the city view, with its own host.
//
// PARTIAL: loose advisor scripts from the plugin folders
// (UpdateResourcesFromDirectory), the tutorial registry's Lua, the MySim functions, is_tutorial, and the building words of
// a reward subject.
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "gz/CheatCodeManager.h"
#include "gz/GZCOMDirector.h"
#include "lua/SCLuaInstance.h"
#include "lua/SCLuaScripts.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/Advice.h"
#include "sc4/AdviceSubject.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorConcrete.h"
#include "sc4/AdviceDetokenizer.h"
#include "sc4/AdvisorUIAdvisorWinProc.h"
#include "sc4/CitySituationManager.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/Budget.h"
#include "sc4/CameraControl.h"
#include "sc4/CivicBuildingSimulator.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/NeighborDeal.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/Occupant.h"
#include "sc4/Ordinance.h"
#include "sc4/RewardAdvice.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/Render3D.h"
#include "sc4/ScriptLuaAdapter.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/StringDetokenizer.h"
#include "app/NativeBoot.h"
#include "app/NativeCitySystems.h"
#include "app/NativeUi.h"
#include "gz/Win.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC4City.h"

namespace native {
float NativeDispatchAltitude(float x, float z);   // NativeDispatch.cpp: the terrain height at a world point
void UpdateAdvisorStatistics(std::uint32_t advisorClassId);   // NativeAdvisorStatistics.cpp
namespace {
using namespace sc4;

const bool kTrace = std::getenv("SC4_ADVISORDEBUG") != nullptr;

// A city component as its concrete class (the serializable view carries the class).
template <class T> T* Comp(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = dynamic_cast<T*>(serializable);
    serializable->Release();
    return out;
}
cSC4Simulator* Simulator() { return Comp<cSC4Simulator>(CityComponent::Simulator); }

cSC4AdvisorSystem* System() { return cSC4AdvisorSystem::Instance(); }

// The app's scripting host (SL::spLua is its cIGZUnknown view; borrowed: the application holds it).
cISCLua* AppLua() {
    static cISCLua* cached = nullptr;
    static cIGZUnknown* cachedFor = nullptr;
    if (cachedFor != SL::spLua) {
        cached = nullptr;
        cachedFor = SL::spLua;
        if (SL::spLua && SL::spLua->QueryInterface(0xea10c4a2, reinterpret_cast<void**>(&cached)) && cached) cached->Release();
    }
    return cached;
}

// GZCOM()->CreateInstance(clsid, iid) as the original calls it. The native COM directors only know the classes registered
// by name, so the city's class table (the serializable factory) makes the recovered advisor / advice / subject classes.
bool CreateObject(std::uint32_t clsid, std::uint32_t iid, void** out) {
    *out = nullptr;
    if (GZCOM() && GZCOM()->CreateInstance(clsid, iid, out) && *out) return true;
    *out = nullptr;
    cIGZSerializable* object = NativeCreateSerializable(clsid);
    if (!object) return false;
    const bool ok = object->QueryInterface(iid, out) && *out;
    object->Release();
    if (!ok) *out = nullptr;
    return ok;
}

void Post(std::uint32_t type, std::uint64_t data1, std::uint64_t data2, std::uint64_t data3, std::uint64_t data4, cIGZUnknown* unknown) {
    if (!SL::spMS2) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(data1);
    message->SetData2(data2);
    message->SetData3(data3);
    message->SetData4(data4);
    if (unknown) message->SetIGZUnknown(unknown);
    SL::spMS2->MessagePost(message, false);
    message->Release();
}

// gAdviceMessageBox: the original opens a modal box; natively the text goes to the log.
void AdviceMessageBox(const char* caption, const char* text) { std::printf("advice box: %s - %s\n", caption, text); }

// ---- the advice API (lua-game-extensions.md 2: every function finds its host through the state, does all Lua access
// through cISCLua and acts on the system's context advice)
cISCLua* HostOf(lua_State* L) { return lua5::SCLuaFromState(reinterpret_cast<::lua_State*>(L)); }
int Trunc(cISCLua* lua, int index) { return static_cast<int>(lua->ToNumber(index)); }
bool HasNumberArg(cISCLua* lua) { return lua->GetTop() > 0 && lua->IsNumber(-1); }
cISC4Advice* Context() { cSC4AdvisorSystem* s = System(); return s ? s->GetContextAdvice() : nullptr; }
cISC4AdviceSubject* SubjectArg(cISCLua* lua, cSC4AdvisorSystem* system) {
    if (!system || lua->GetTop() <= 0 || !lua->IsUserData(-1) || !lua->ToUserData(-1) || system->GetDataTag(0) != lua->Tag(-1)) return nullptr;
    return static_cast<cISC4AdviceSubject*>(lua->ToUserTag(-1));
}
// A string as tag-1 userdata holding one reference (released by the system's gc method).
void PushStringUserData(cISCLua* lua, const cRZString& text) {
    auto* value = new cRZString(text);
    value->AddRef();
    lua->PushUserData(static_cast<cIGZString*>(value), System() ? System()->GetDataTag(1) : 0);
}

cSC4HistoryWarehouse* History() { return Comp<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse); }
cSC4OrdinanceSimulator* Ordinances() { return Comp<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator); }
cSC4CivicBuildingSimulator* Civic() { return Comp<cSC4CivicBuildingSimulator>(CityComponent::CivicBuildingSimulator); }

// trend_value / trend_slope / trend_count(type, months): the history over the last 30 x months days (advisors.md 8.3).
int TrendArgs(cISCLua* lua, std::uint32_t& series, int& months) {
    if (lua->GetTop() < 2 || !lua->IsNumber(-1) || !lua->IsNumber(-2)) return 0;
    series = static_cast<std::uint32_t>(Trunc(lua, -2));
    months = Trunc(lua, -1);
    return 1;
}
int L_trend_value(lua_State* L) {
    cISCLua* lua = HostOf(L);
    std::uint32_t series = 0; int months = 0;
    float value = 0.0f;
    if (TrendArgs(lua, series, months)) {
        cSC4Simulator* sim = Simulator();
        if (cSC4HistoryWarehouse* h = History(); h && sim)
            if (!h->GetPointDay(series, sim->GetSimDateNumber() - 30u * static_cast<std::uint32_t>(months), value)) value = 0.0f;
    }
    lua->PushNumber(value);
    return 1;
}
int L_trend_slope(lua_State* L) {
    cISCLua* lua = HostOf(L);
    std::uint32_t series = 0; int months = 0;
    float result = 0.0f;
    if (TrendArgs(lua, series, months)) {
        cSC4Simulator* sim = Simulator();
        float slope = 0.0f, intercept = 0.0f;
        if (cSC4HistoryWarehouse* h = History(); h && sim) {
            const std::uint32_t now = sim->GetSimDateNumber();
            if (h->GetTrendDays(series, now - 30u * static_cast<std::uint32_t>(months), now, slope, intercept))
                result = static_cast<float>(std::atan(static_cast<double>(slope)) * 63.6618f);
        }
    }
    lua->PushNumber(result);
    return 1;
}
int L_trend_count(lua_State* L) {
    cISCLua* lua = HostOf(L);
    std::uint32_t series = 0; int months = 0;
    float count = 0.0f;
    if (TrendArgs(lua, series, months)) {
        cSC4Simulator* sim = Simulator();
        if (cSC4HistoryWarehouse* h = History(); h && sim) {
            const std::uint32_t now = sim->GetSimDateNumber();
            if (!h->GetCountDays(series, now - 30u * static_cast<std::uint32_t>(months) + 1u, now, count)) count = 0.0f;
        }
    }
    lua->PushNumber(count);
    return 1;
}

// camera_jump(subject) / camera_zoom(level) (advisors.md 8.3.2): no results.
int L_camera_jump(lua_State* L) {
    cISCLua* lua = HostOf(L);
    cSC4AdvisorSystem* system = System();
    if (system) system->GetContextAdvice();
    cSC43DRender* render = CityRenderer();
    cSC4CameraControl* camera = render ? render->GetCameraControl() : nullptr;
    cISC4AdviceSubject* subject = SubjectArg(lua, system);
    if (subject && camera) {
        cS3DVector3 p{0.0f, 0.0f, 0.0f};
        bool have = false;
        if (subject->HasLocation()) {
            p.x = 16.0f * subject->GetLocationX();
            p.z = 16.0f * subject->GetLocationZ();
            p.y = NativeDispatchAltitude(p.x, p.z);
            have = true;
        } else if (cIGZUnknown* object = subject->GetObjectA()) {
            sc4::cISC4Occupant* occupant = nullptr;
            if (object->QueryInterface(kGZIID_cISC4Occupant, reinterpret_cast<void**>(&occupant)) && occupant) {
                float xyz[3] = {0.0f, 0.0f, 0.0f};
                occupant->GetPosition(xyz);
                p.x = xyz[0]; p.y = xyz[1]; p.z = xyz[2];
                occupant->Release();
                have = true;
            }
        }
        if (have) {
            camera->SetViewTargetPosition(p);
            return 0;
        }
    }
    AdviceMessageBox("game.LUAExtension_camera_jump()", "The subject for camera jump is unknown...");
    return 0;
}
int L_camera_zoom(lua_State* L) {
    cISCLua* lua = HostOf(L);
    if (cSC4AdvisorSystem* system = System()) system->GetContextAdvice();
    cSC43DRender* render = CityRenderer();
    cSC4CameraControl* camera = render ? render->GetCameraControl() : nullptr;
    if (HasNumberArg(lua) && camera) {
        camera->SetZoom(static_cast<float>(lua->ToNumber(-1)));
        return 0;
    }
    AdviceMessageBox("game.LUAExtension_camera_zoom()", "'zoom_level' was not specified or valid range of zooms.");
    return 0;
}

// The ordinance functions: 1 or nil, fees as numbers (0 when missing), names as string userdata.
cISC4Ordinance* OrdinanceArg(cISCLua* lua) {
    cSC4OrdinanceSimulator* sim = Ordinances();
    return sim && HasNumberArg(lua) ? sim->GetOrdinanceByID(static_cast<std::uint32_t>(Trunc(lua, -1))) : nullptr;
}
int PushFlag(cISCLua* lua, bool on) { if (on) lua->PushNumber(1.0); else lua->PushNil(); return 1; }
int L_ordinance_is_on(lua_State* L) { cISCLua* lua = HostOf(L); cISC4Ordinance* o = OrdinanceArg(lua); return PushFlag(lua, o && o->IsOn()); }
int L_ordinance_is_available(lua_State* L) { cISCLua* lua = HostOf(L); cISC4Ordinance* o = OrdinanceArg(lua); return PushFlag(lua, o && o->IsAvailable()); }
int L_ordinance_set_on(lua_State* L) {
    cISCLua* lua = HostOf(L);
    if (cSC4OrdinanceSimulator* sim = Ordinances(); sim && HasNumberArg(lua)) sim->SetOrdinanceOn(static_cast<std::uint32_t>(Trunc(lua, -1)), true);
    return 0;
}
int L_ordinance_set_off(lua_State* L) {
    cISCLua* lua = HostOf(L);
    if (cSC4OrdinanceSimulator* sim = Ordinances(); sim && HasNumberArg(lua)) sim->SetOrdinanceOn(static_cast<std::uint32_t>(Trunc(lua, -1)), false);
    return 0;
}
int L_ordinance_enact_fee(lua_State* L) { cISCLua* lua = HostOf(L); cISC4Ordinance* o = OrdinanceArg(lua); lua->PushNumber(o ? static_cast<double>(o->GetEnactmentIncome()) : 0.0); return 1; }
int L_ordinance_repeal_fee(lua_State* L) { cISCLua* lua = HostOf(L); cISC4Ordinance* o = OrdinanceArg(lua); lua->PushNumber(o ? static_cast<double>(o->GetRetractionIncome()) : 0.0); return 1; }
int L_ordinance_monthly_cost(lua_State* L) { cISCLua* lua = HostOf(L); cISC4Ordinance* o = OrdinanceArg(lua); lua->PushNumber(o ? static_cast<double>(o->GetMonthlyAdjustedIncome()) : 0.0); return 1; }
int OrdinanceText(lua_State* L, bool name) {
    cISCLua* lua = HostOf(L);
    if (cSC4AdvisorSystem* system = System()) system->GetContextAdvice();
    cISC4Ordinance* o = OrdinanceArg(lua);
    if (!o) { lua->PushNil(); return 1; }
    PushStringUserData(lua, name ? o->GetName() : o->GetDescription());
    return 1;
}
int L_ordinance_name(lua_State* L) { return OrdinanceText(L, true); }
int L_ordinance_description(lua_State* L) { return OrdinanceText(L, false); }

int L_difficulty_level(lua_State* L) {
    cISCLua* lua = HostOf(L);
    cSC4City* city = HostCity();
    lua->PushNumber(city ? static_cast<double>(static_cast<int>(city->GetDifficultyLevel())) : -1.0);
    return 1;
}
int L_event_feedback(lua_State* L) {
    cISCLua* lua = HostOf(L);
    cISC4Advice* ctx = Context();
    if (!ctx) return 0;
    const int n = HasNumberArg(lua) ? Trunc(lua, -1) : 0;
    cISC4AdviceSubject* subject = ctx->GetEventSubject();
    Post(static_cast<std::uint32_t>(ctx->GetEventID()), static_cast<std::uint32_t>(n), 0, 0, 0, subject ? subject->GetObjectA() : nullptr);
    return 0;
}
int L_expire_advice(lua_State*) {
    if (cISC4Advice* ctx = Context()) ctx->SetFlags(0x8, true);   // the expire request (advisors.md 2.3; the port's kAdviceFlagNoLinks)
    return 0;
}
int L_retire_advice(lua_State*) {
    cSC4AdvisorSystem* system = System();
    cISC4Advice* ctx = Context();
    if (system && ctx)
        if (cISC4Advisor* advisor = system->GetAdvisor(ctx->GetAdvisorID())) advisor->RetireAdvice(ctx);
    return 0;
}
int L_advice_subject(lua_State* L) {
    cISCLua* lua = HostOf(L);
    cSC4AdvisorSystem* system = System();
    cISC4Advice* ctx = Context();
    cISC4AdviceSubject* subject = ctx ? ctx->GetEventSubject() : nullptr;
    cISC4AdviceSubject* clone = nullptr;
    if (!system || !subject || !subject->Clone(&clone) || !clone) { lua->PushNil(); return 1; }
    lua->PushUserData(clone, system->GetDataTag(0));   // the clone's reference goes to the userdata
    return 1;
}
int L_trigger_event(lua_State* L) {
    cISCLua* lua = HostOf(L);
    cSC4AdvisorSystem* system = System();
    if (system && HasNumberArg(lua)) { system->PushEvent(Trunc(lua, -1), nullptr); return 0; }
    AdviceMessageBox("LUAExtension_trigger_event()", "'eventGUID' was not specified or was not a valid number.");
    return 0;
}
int L_building_is_group(lua_State* L) {
    cISCLua* lua = HostOf(L);
    cSC4AdvisorSystem* system = System();
    bool result = false;
    if (system && lua->GetTop() >= 2 && lua->IsNumber(-1) && lua->IsUserData(-2) && lua->Tag(-2) == system->GetDataTag(0)) {
        auto* subject = static_cast<cISC4AdviceSubject*>(lua->ToUserTag(-2));
        cIGZUnknown* object = subject ? subject->GetObjectA() : nullptr;
        sc4::cISC4Occupant* occupant = nullptr;
        if (object && object->QueryInterface(kGZIID_cISC4Occupant, reinterpret_cast<void**>(&occupant)) && occupant) {
            result = occupant->IsOccupantGroup(static_cast<std::uint32_t>(Trunc(lua, -1)));
            occupant->Release();
        }
    }
    return PushFlag(lua, result);
}
int SubjectData1(lua_State* L, int onlyEvent) {
    cISCLua* lua = HostOf(L);
    cISC4Advice* ctx = Context();
    cISC4AdviceSubject* subject = ctx ? ctx->GetEventSubject() : nullptr;
    if (!subject || (onlyEvent && ctx->GetEventID() != onlyEvent)) { lua->PushNil(); return 1; }
    lua->PushNumber(static_cast<double>(subject->GetData1()));
    return 1;
}
int L_event_building_distance(lua_State* L) { return SubjectData1(L, static_cast<int>(0xca4e8986u)); }
int L_event_disaster_distance(lua_State* L) { return SubjectData1(L, 0); }
// The MySim functions act on the context advice's advisor as a MySim (out of the advisor system's scope; PARTIAL: no value).
int L_mysim_nil(lua_State* L) { HostOf(L)->PushNil(); return 1; }
int L_mysim_dispatch_tool(lua_State*) { return 0; }
// random_chance(p): 1 when a draw in [0, 100) from the system's generator is below p (advisors.md 9).
int L_random_chance(lua_State* L) {
    cISCLua* lua = HostOf(L);
    cSC4AdvisorSystem* system = System();
    if (!system || !HasNumberArg(lua)) { lua->PushNil(); return 1; }
    const double p = lua->ToNumber(-1);
    const double draw = system->GetRandom()->RandomDoubleRangeUniform(0.0, 100.0);
    return PushFlag(lua, draw < p);
}
int L_nd_cancel(lua_State* L) {
    cISCLua* lua = HostOf(L);
    cISC4AdviceSubject* subject = SubjectArg(lua, System());
    cIGZUnknown* object = subject ? subject->GetObjectA() : nullptr;
    cISC4NeighborDeal* deal = nullptr;
    if (object && object->QueryInterface(kGZIID_cISC4NeighborDeal, reinterpret_cast<void**>(&deal)) && deal) {
        deal->Cancel();
        deal->Release();
    }
    return 0;
}
// The reward building status the reward advices write (advisors.md 6.6).
int L_reward_instance_count(lua_State* L) {
    cISCLua* lua = HostOf(L);
    if (!HasNumberArg(lua)) { lua->PushNumber(0.0); return 1; }
    const auto id = static_cast<std::uint32_t>(Trunc(lua, -1));
    double count = 0.0;
    cSC4CivicBuildingSimulator* civic = Civic();
    if (auto* status = civic ? civic->GetConditionalBuildingStatus(id) : nullptr) {
        count = status->BuildingCount();
    } else if (cSC4AdvisorSystem* system = System()) {
        if (auto* planning = dynamic_cast<cSC4AdvisorCityPlanning*>(system->GetAdvisor(23))) count = planning->GetLandmarkCount(static_cast<int>(id));
    }
    lua->PushNumber(count);
    return 1;
}
int L_get_reward_building_state(lua_State* L) {
    cISCLua* lua = HostOf(L);
    if (!HasNumberArg(lua)) { lua->PushNil(); return 1; }
    const double argument = lua->ToNumber(-1);
    cSC4CivicBuildingSimulator* civic = Civic();
    auto* status = civic ? civic->GetConditionalBuildingStatus(static_cast<std::uint32_t>(argument)) : nullptr;
    if (!status) lua->PushNumber(1.0);
    else if (status->state & 4u) lua->PushNumber(2.0);
    else lua->PushNumber(argument);   // quirk kept: answers its argument
    return 1;
}
int L_unlock_reward_building(lua_State* L) {
    cISCLua* lua = HostOf(L);
    if (!HasNumberArg(lua)) {
        AdviceMessageBox("LUAExtension_unlock_reward_building()", "'buildingGUID' was not specified or was not a valid number.");
        return 0;
    }
    const auto guid = static_cast<std::uint32_t>(Trunc(lua, -1));
    cSC4CivicBuildingSimulator* civic = Civic();
    if (!civic) return 0;
    cSC4CivicBuildingSimulator::ConditionalBuildingStatus record;
    if (auto* status = civic->GetConditionalBuildingStatus(guid)) {
        record.state = status->state;
        record.text = status->text;
    }
    record.state = (record.state & 0xffffff00u) | 2u;
    civic->UpdateConditionalBuildingStatus(guid, &record);
    return 0;
}
int L_is_tutorial(lua_State* L) { HostOf(L)->PushNumber(0.0); return 1; }   // PARTIAL: no tutorial city natively yet (scripts compare it with < 1)
int L_advice_info(lua_State*) {
    cRZString text("No context advice available...\n");
    cSC4AdvisorSystem* system = System();
    if (cISC4Advice* ctx = Context(); ctx && system)
        if (cISC4Advisor* advisor = system->GetAdvisor(ctx->GetAdvisorID()))
            text.Sprintf("\tAdvisor : %s\n\tAdvice title : %s\n ", advisor->GetName().ToChar(), ctx->GetTextTitle().ToChar());
    AdviceMessageBox("game.advice_info()", text.ToChar());
    return 0;
}
int L_combine_guids(lua_State* L) {
    cISCLua* lua = HostOf(L);
    if (lua->GetTop() > 1 && lua->IsNumber(-1) && lua->IsNumber(-2))
        lua->PushNumber(static_cast<double>(Trunc(lua, -1) ^ Trunc(lua, -2)));
    else
        lua->PushNumber(-1.0);
    return 1;
}

// The UI collector (advisors.md 8.3.1): messages to the 3D view window.
cIGZWin* ViewWin() {
    auto* view = dynamic_cast<cSC4View3DWin*>(SL::spView3DWin);
    return view ? view->AsIGZWin() : nullptr;
}
void ToolPlop(lua_State* L, std::uint32_t item) {
    cISCLua* lua = HostOf(L);
    if (lua->GetTop() == 0 || lua->Type(-1) != 3) return;
    if (kTrace) std::fprintf(stderr, "advisors: tool plop 0x%08x type %u (view %s)\n", item,
                             static_cast<unsigned>(Trunc(lua, -1)), ViewWin() ? "yes" : "none");
    if (cIGZWin* win = ViewWin()) {
        cGZMessage message{3, 0x49d993d4, item, static_cast<std::uint32_t>(Trunc(lua, -1))};
        win->DoMessage(message);
    }
}
void WindowOpen(lua_State* L, std::uint32_t window) {
    cISCLua* lua = HostOf(L);
    if (cSC4AdvisorSystem* system = System()) system->GetContextAdvice();
    if (lua->GetTop() == 0 || lua->Type(-1) != 3) return;
    if (cIGZWin* win = ViewWin()) {
        cGZMessage message{window, static_cast<std::uint32_t>(Trunc(lua, -1)), 0, 0};
        win->DoMessage(message);
    }
}
int L_tool_plop_network(lua_State* L) { ToolPlop(L, 0x4a2b2467); return 0; }
int L_tool_plop_zone(lua_State* L) { ToolPlop(L, 0x4a2b2466); return 0; }
int L_tool_plop_building(lua_State* L) { ToolPlop(L, 0x6a2b245f); return 0; }
int L_tool_plop_flora(lua_State* L) { ToolPlop(L, 0xea5ad08d); return 0; }
int L_tool_button(lua_State* L) {
    cISCLua* lua = HostOf(L);
    if (cSC4AdvisorSystem* system = System()) system->GetContextAdvice();
    if (lua->GetTop() != 1 || lua->Type(-1) != 3) return 0;
    const int n = Trunc(lua, -1);
    if (n == 0) return 0;
    if (cIGZWin* win = ViewWin()) {   // PARTIAL: the original hands it to the window's 0xc6ae7085 proc; DoMessage reaches the same commands
        cGZMessage message{3, 0x287259f6, static_cast<std::uint32_t>(n), 0};
        win->DoMessage(message);
    }
    return 0;
}
int L_window_budget(lua_State* L) { WindowOpen(L, 0xea5bdfbc); return 0; }
int L_window_map(lua_State* L) { WindowOpen(L, 0x6a5bdfcd); return 0; }
int L_window_graph(lua_State* L) { WindowOpen(L, 0x6a720993); return 0; }
int L_window_query(lua_State*) {   // PARTIAL: the query dialog (cSC4ViewInputControlQuery::DoQueryDialog) is not wired from here
    if (cSC4AdvisorSystem* system = System()) system->GetContextAdvice();
    return 0;
}

using CF = cISCLua::CFunction;
// The one extension vector, simulation collector first, then the UI collector (advisors.md 8.2, AD7).
const IAdvisorSystemHost::LuaExtension kExtensions[] = {
    {"advice_info", &L_advice_info, false},
    {"trend_value", &L_trend_value, false},
    {"trend_slope", &L_trend_slope, false},
    {"trend_count", &L_trend_count, false},
    {"camera_jump", &L_camera_jump, false},
    {"camera_zoom", &L_camera_zoom, false},
    {"ordinance_is_on", &L_ordinance_is_on, false},
    {"ordinance_set_off", &L_ordinance_set_off, false},
    {"ordinance_set_on", &L_ordinance_set_on, false},
    {"ordinance_is_available", &L_ordinance_is_available, false},
    {"ordinance_name", &L_ordinance_name, false},
    {"ordinance_description", &L_ordinance_description, false},
    {"ordinance_enact_fee", &L_ordinance_enact_fee, false},
    {"ordinance_repeal_fee", &L_ordinance_repeal_fee, false},
    {"ordinance_monthly_cost", &L_ordinance_monthly_cost, false},
    {"difficulty_level", &L_difficulty_level, false},
    {"event_feedback", &L_event_feedback, false},
    {"expire_advice", &L_expire_advice, false},
    {"retire_advice", &L_retire_advice, false},
    {"advice_subject", &L_advice_subject, false},
    {"trigger_event", &L_trigger_event, false},
    {"building_is_group", &L_building_is_group, false},
    {"event_building_distance", &L_event_building_distance, false},
    {"mysim_distance_to_closest_building", &L_mysim_nil, false},
    {"mysim_distance_to_closest_landmark_or_reward", &L_mysim_nil, false},
    {"mysim_local_school_grade", &L_mysim_nil, false},
    {"mysim_local_hospital_grade", &L_mysim_nil, false},
    {"mysim_dispatch_tool", &L_mysim_dispatch_tool, false},
    {"event_disaster_distance", &L_event_disaster_distance, false},
    {"random_chance", &L_random_chance, false},
    {"nd_cancel", &L_nd_cancel, false},
    {"reward_instance_count", &L_reward_instance_count, false},
    {"unlock_reward_building", &L_unlock_reward_building, false},
    {"get_reward_building_state", &L_get_reward_building_state, false},
    {"is_tutorial", &L_is_tutorial, false},
    {"_combine_guids", &L_combine_guids, true},
    {"_dofile_from_resource", reinterpret_cast<CF>(&SCLuaHelp::LUAExtension_dofile_from_resource), true},
    {"print", reinterpret_cast<CF>(&SCLuaHelp::LUAExtension_print), true},
    {"tool_plop_network", &L_tool_plop_network, false},
    {"tool_plop_zone", &L_tool_plop_zone, false},
    {"tool_plop_building", &L_tool_plop_building, false},
    {"tool_plop_flora", &L_tool_plop_flora, false},
    {"tool_button", &L_tool_button, false},
    {"window_budget", &L_window_budget, false},
    {"window_map", &L_window_map, false},
    {"window_query", &L_window_query, false},
    {"window_graph", &L_window_graph, false},
};

// ---- the advisor window
const bool kWindowTrace = std::getenv("SC4_ADVWINDEBUG") != nullptr;

// What cSC4AdvisorUIAdvisorWinProc reaches: the city view, the city's advisors, the UI scripts and the message server.
class AdvisorWindowHost final : public IAdvisorUIAdvisorHost {
public:
    cIGZWin* View3DWindow() override {
        cSC4View3DWin* view = CityView3DWin();
        return view ? static_cast<cIGZWin*>(static_cast<cGZWin*>(view)) : nullptr;
    }
    bool CityPresent() override { return HostCity() != nullptr; }
    cISC4Advisor* Advisor(int advisorId) override {
        cSC4AdvisorSystem* system = HostCity() ? System() : nullptr;
        return system ? system->GetAdvisor(advisorId) : nullptr;   // borrowed: the system keeps it
    }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid,
                                void** out) override {
        *out = nullptr;
        cIGZWin* window = native::CreateWindowFromScript(key, parent, id);
        if (kWindowTrace) std::printf("advisor window: script %08x/%08x window %08x -> %s\n", key.group, key.instance, id, window ? "created" : "FAILED");
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out) && *out;
        window->Release();
        return ok;
    }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    void FilterUICommandMessage(cGZMessage&) override {}   // PARTIAL: the tutorial system is not recovered
};

// The advisor system's view of the window (IAdvisorWinProc) over the recovered procedure.
class AdvisorWinProcAdapter final : public IAdvisorWinProc {
public:
    AdvisorWinProcAdapter() : proc_(new cSC4AdvisorUIAdvisorWinProc()) { proc_->AddRef(); }
    ~AdvisorWinProcAdapter() override { proc_->Release(); }
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ == 0) delete this;
    }
    void Init() override {
        proc_->Init();
        if (kWindowTrace) std::printf("advisor window: init -> %s\n", proc_->Window() ? "window" : "NO WINDOW");
    }
    void Shutdown() override { proc_->Shutdown(); }
    void ShowWindow() override { proc_->ShowWindow(); }
    void HideWindow() override { proc_->HideWindow(); }
    bool IsVisible() override { return proc_->IsVisible(); }
    cSC4AdvisorUIAdvisorWinProc* Proc() const { return proc_; }

private:
    int refs_ = 0;
    cSC4AdvisorUIAdvisorWinProc* proc_;
};

// The city view's mayor panel reaches the window through the city's advisor system (IAdvisorWindow, one reference).
class AdvisorWindowView final : public IAdvisorWindow {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }
    void ShowAdvisorWindow() override { if (cSC4AdvisorSystem* s = System()) s->ShowAdvisorWindow(); }
    void HideAdvisorWindow() override { if (cSC4AdvisorSystem* s = System()) s->HideAdvisorWindow(); }
    bool IsAdvisorWindowVisible() override { cSC4AdvisorSystem* s = System(); return s && s->IsAdvisorWindowVisible(); }

private:
    std::uint32_t refs_ = 0;
};

// The advice detokenizer reaches the city's situation manager.
class AdviceDetokenizerHostImpl final : public IAdviceDetokenizerHost {
public:
    cSC4CitySituationManager* SituationManager() override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::CitySituationManager) : nullptr;
        return object ? dynamic_cast<cSC4CitySituationManager*>(object) : nullptr;
    }
    cISC4AdvisorSystem* System(void* context) override {
        return static_cast<cISC4AdvisorSystem*>(static_cast<cSC4AdvisorSystem*>(context));
    }
};

// ---- the hosts
bool g_llama = false;   // the llama cheat: every advisor head is a llama

class SystemHost final : public IAdvisorSystemHost {
public:
    cISCLua* Lua() override { return AppLua(); }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    cGZCheatCodeManager* CheatCodeManager() override { return dynamic_cast<cGZCheatCodeManager*>(SL::spCCM); }
    cSCStringDetokenizer* Detokenizer() override { return NativeDetokenizer(); }
    IAdvisorWinProc* CreateAdvisorWinProc() override { return new AdvisorWinProcAdapter(); }
    cIGZUnknown* City() override { return HostCity() ? static_cast<cIGZUnknown*>(SL::spCity) : nullptr; }
    bool CityEstablished(cIGZUnknown*) override { cSC4City* c = HostCity(); return c && c->GetEstablished(); }
    int CityDifficultyLevel() override { cSC4City* c = HostCity(); return c ? static_cast<int>(c->GetDifficultyLevel()) : 0; }
    bool DebugFunctionalityEnabled() override { return SL::spSC4 && SL::spSC4->GetDebugFunctionalityEnabled(); }
    void GetDataDirectory(cIGZString& out, int language) override { if (SL::spSC4) SL::spSC4->GetDataDirectory(out, language); }
    void GetPluginDirectory(cIGZString& out) override { if (SL::spSC4) SL::spSC4->GetPluginDirectory(out); }
    void SetTutorialRegistryLua(cISCLua*) override {}   // PARTIAL: cSC4TutorialRegistry is not recovered
    bool CreateAdvisor(std::uint32_t classId, std::uint32_t iid, cISC4Advisor** out) override {
        const bool ok = CreateObject(classId, iid, reinterpret_cast<void**>(out));
        if (kTrace) std::fprintf(stderr, "advisors: create advisor class 0x%08x -> %s\n", classId, ok ? "ok" : "FAILED");
        return ok;
    }
    DetokenizeMethod AdviceDetokenizerFunction() override { return &sc4::AdviceDetokenizerFunction; }
    const LuaExtension* LuaExtensions(std::size_t& count) override {
        count = sizeof(kExtensions) / sizeof(kExtensions[0]);
        return kExtensions;
    }
    void UpdateResourcesFromDirectory(std::uint32_t, const char*) override {}   // PARTIAL: loose advisor scripts
    void LoadPackageScripts(cISCLua* lua, std::uint32_t package, int flags) override {
        const int failed = SCLuaHelp::LoadPackageScripts(lua, package, static_cast<std::uint32_t>(flags));
        if (kTrace) std::fprintf(stderr, "advisors: package scripts of 0x%08x loaded, %d did not run\n", package, failed);
    }
    cIGZDate* SimDate() override {
        cSC4Simulator* sim = Simulator();
        return sim ? const_cast<cGZDate*>(&sim->GetSimDate()) : nullptr;
    }
    void SimStartDate(cIGZDate& out) override { cSC4Simulator::GetSimStartDateStatic(out); }
    bool SimulatorAnyPaused() override { cSC4Simulator* sim = Simulator(); return sim && sim->IsAnyPaused(); }
    // cSC4AdvisorUIImageWinProc::mbLlamaCheatActive ^= 1 (the heads read it through NativeLlamaCheatActive)
    void ToggleLlamaCheat() override { llama_ = !llama_; g_llama = llama_; }
    cISCLua* LuaFromState(lua_State* state) override {
        cISCLua* lua = lua5::SCLuaFromState(reinterpret_cast<::lua_State*>(state));
        if (lua) lua->AddRef();
        return lua;
    }

private:
    bool llama_ = false;
};

// The system's scripting context as the advisor / advice seam (IScriptLua).
IScriptLua* SystemScriptLua() {
    static cISCLuaAdapter* adapter = nullptr;
    static cISCLua* adapterFor = nullptr;
    cSC4AdvisorSystem* system = System();
    cISCLua* lua = system ? system->GetScriptingContext() : nullptr;
    if (!lua) return nullptr;
    if (lua != adapterFor) {
        delete adapter;
        adapter = new cISCLuaAdapter(lua);
        adapterFor = lua;
    }
    return adapter;
}

class AdvisorHost final : public IAdvisorHost {
public:
    IScriptLua* Lua() override { return SystemScriptLua(); }
    void RegisterAdvice(int eventId, cISC4Advisor* advisor) override { if (System()) System()->SubscribeToEvent(eventId, advisor); }
    void UnregisterAdvice(int eventId, cISC4Advisor* advisor) override { if (System()) System()->UnsubscribeFromEvent(eventId, advisor); }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    bool CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) override {
        return CreateObject(classId, iid, out);
    }
    int SimDayNumber() override { cSC4Simulator* sim = Simulator(); return sim ? static_cast<int>(sim->GetSimDateNumber()) : 0; }
    void PostMessage2(std::uint32_t type, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3, std::uint32_t d4, cIGZUnknown* unknown) override {
        if (kTrace) std::fprintf(stderr, "advisors: post 0x%08x data1 0x%x\n", type, d1);
        Post(type, d1, d2, d3, d4, unknown);
    }
};

class AdviceHost final : public IAdviceHost {
public:
    bool AdvisorSystemPresent() override { return System() != nullptr; }
    IScriptLua* Lua() override { return SystemScriptLua(); }
    void BeginScript(cSC4Advice* advice) override { if (System()) System()->PushContextAdvice(advice); }
    void EndScript() override { if (System()) System()->PopContextAdvice(); }
    bool DebugFunctionalityEnabled() override { return SL::spSC4 && SL::spSC4->GetDebugFunctionalityEnabled(); }
    cISC4Advice* CreateAdvice(std::uint32_t clsid, std::uint32_t iid) override {
        void* out = nullptr;
        return CreateObject(clsid, iid, &out) ? static_cast<cISC4Advice*>(out) : nullptr;
    }
    bool HasView3D() override { return dynamic_cast<cSC4View3DWin*>(SL::spView3DWin) != nullptr; }
    void GlobalEffectOn(std::uint32_t id) override { if (auto* v = dynamic_cast<cSC4View3DWin*>(SL::spView3DWin)) v->AssertGlobalEffectTrigger(id); }
    void GlobalEffectOff(std::uint32_t id) override { if (auto* v = dynamic_cast<cSC4View3DWin*>(SL::spView3DWin)) v->RetractGlobalEffectTrigger(id); }
};

class SubjectHost final : public IAdviceSubjectHost {
public:
    std::int32_t CityCellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    std::int32_t CityCellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
};

cISC4AdviceSubject* NewSubject() {
    void* out = nullptr;
    return CreateObject(kGZCLSID_cSC4AdviceSubject, kGZIID_cISC4AdviceSubject, &out) ? static_cast<cISC4AdviceSubject*>(out) : nullptr;
}

class RewardHost final : public IRewardAdviceHost {
public:
    bool GetConditionState(std::uint32_t guid, std::uint32_t& state, cRZString& text) override {
        cSC4CivicBuildingSimulator* civic = Civic();
        auto* status = civic ? civic->GetConditionalBuildingStatus(guid) : nullptr;
        if (!status) return false;
        state = status->state;
        text = status->text;
        return true;
    }
    void SetConditionState(std::uint32_t guid, std::uint32_t state, const cRZString& text) override {
        cSC4CivicBuildingSimulator* civic = Civic();
        if (!civic) return;
        cSC4CivicBuildingSimulator::ConditionalBuildingStatus record;
        record.state = state;
        record.text = text;
        civic->UpdateConditionalBuildingStatus(guid, &record);
    }
    bool GetBuildingWords(std::uint32_t, std::uint32_t& word0, std::uint32_t& word1) override { word0 = word1 = 0; return false; }   // PARTIAL
    cISC4AdviceSubject* CreateSubject() override { return NewSubject(); }
};

class ConcreteHost final : public IAdvisorConcreteHost {
public:
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    void SetGlobal(const char* name, double value) override {
        if (kTrace) std::fprintf(stderr, "advisors: game.%s = %g\n", name, value);
        if (System()) System()->SetGlobalValue(name, value);
    }
    void SetLocal(const char* name, double value, cISC4AdviceSubject* subject) override {
        if (kTrace) std::fprintf(stderr, "advisors: game.%s = %g (subject %p)\n", name, value, static_cast<void*>(subject));
        if (System()) System()->SetLocalValue(name, value, subject);
    }
    bool PushEvent(int eventId, cISC4AdviceSubject* subject) override { return System() && System()->PushEvent(eventId, subject); }
    bool CreateAdviceSubject(cISC4AdviceSubject** out) override { *out = NewSubject(); return *out != nullptr; }
    std::int64_t BudgetInt(std::uint32_t vslot) override {
        cSC4BudgetSimulator* b = Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        if (!b) return 0;
        switch (vslot) {
            case 0x30: return b->GetTotalFunds();
            case 0x70: return b->GetTotalMonthlyExpense();
            case 0x80: return b->GetTotalMonthlyIncome();
            case 0x90: return b->GetTaxIncome();
            case 0x148: return b->GetTotalBorrowed();
            case 0x150: return b->GetCurrentBorrowingLimit();
            case 0x168: return b->GetTotalMonthlyBondPayments();
            default: return 0;
        }
    }
    float BudgetTaxRate(int index) override {
        cSC4BudgetSimulator* b = Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        return b ? b->GetTaxRate(index) : 0.0f;
    }
    // The department's funding percentage (advisors.md 7.3: GetDepartmentBudget(id) -> funding).
    float BudgetLineItemFunding(std::uint32_t id) override {
        cSC4BudgetSimulator* b = Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        cSC4DepartmentBudget* d = b ? b->GetDepartmentBudget(id) : nullptr;
        return d ? d->GetFundingPercentage() : 0.0f;
    }
    std::int64_t NeighborsInt(std::uint32_t vslot) override {
        cSC4NeighborsSimulator* n = Comp<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator);
        if (!n) return 0;
        return vslot == 0x100 ? n->GetTotalMonthlyNeighborDealIncome() : vslot == 0x108 ? n->GetTotalMonthlyNeighborDealExpense() : 0;
    }
    // advisors.md 7.5: +0x88 GetGlobalLE, +0x84 health (global HQ), +0x80 education (global EQ); Mac 0x110 / 0x108 / 0x100.
    std::int64_t ResidentialInt(std::uint32_t vslot) override {
        auto* r = Comp<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator);
        if (!r) return 0;
        return vslot == 0x110 ? r->GetGlobalLE() : vslot == 0x80 ? r->GetPopulation() : 0;
    }
    float ResidentialFloat(std::uint32_t vslot) override {
        auto* r = Comp<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator);
        if (!r) return 0.0f;
        return vslot == 0x108 ? r->GetGlobalHQ() : vslot == 0x100 ? r->GetGlobalEQ() : 0.0f;
    }
    void ResidentialTracts(std::uint32_t, float& bestValue, int& bestX, int& bestZ, float& worstValue, int& worstX, int& worstZ) override {
        bestValue = worstValue = 0.0f; bestX = bestZ = worstX = worstZ = 0;
    }
    bool MessageVoid1IsPropertyHolder(cIGZMessage2*) override { return false; }
    bool MessageVoid1HasProperty(cIGZMessage2*, std::uint32_t) override { return false; }
    bool BuildingDevelopmentLookup(cIGZMessage2*, std::uint32_t*) override { return false; }
    bool CivicBuildingAllowsLinks(std::uint32_t data) override {
        cSC4CivicBuildingSimulator* civic = Civic();
        auto* status = civic ? civic->GetConditionalBuildingStatus(data) : nullptr;
        return !status || (status->state & 2u) == 0;
    }
    void UpdateSimulatorStatistics(std::uint32_t advisorClassId) override { UpdateAdvisorStatistics(advisorClassId); }
};

}  // namespace

bool NativeLlamaCheatActive() { return g_llama; }

// cSC4HTMLLinkHandler::DoURL (Mac i386 0x0016982c; 64-bit 0x1001f22b6). "sc4://advice/REST" goes to the city's advisor
// system (DoLink(REST), its answer is the result). "sc4://action/..." is split at '?' and dropped: the original does
// nothing with it and answers false. Any other link answers false.
bool NativeHtmlLinkDoURL(const cIGZString& link) {
    std::string text(link.ToChar(), link.Strlen());
    const bool action = text.find("action") == 6 && text.size() > 11 && text[12] == '/';
    if (kTrace || kWindowTrace) std::printf("html link: %s\n", text.c_str());
    if (action) return false;
    if (text.find("advice") != 6 || text.size() <= 11 || text[12] != '/') return false;
    text.erase(0, 13);
    cSC4AdvisorSystem* system = HostCity() ? System() : nullptr;
    return system && system->DoLink(cRZString(text.c_str()));
}

sc4::IAdvisorWindow* NativeAdvisorWindow() {
    if (!HostCity() || !System()) return nullptr;
    auto* view = new AdvisorWindowView();
    view->AddRef();   // the caller's reference
    return view;
}

sc4::cSC4AdvisorUIAdvisorWinProc* NativeAdvisorWinProc() {
    cSC4AdvisorSystem* system = System();
    auto* adapter = system ? dynamic_cast<AdvisorWinProcAdapter*>(system->GetAdvisorWinProc()) : nullptr;
    return adapter ? adapter->Proc() : nullptr;
}

void InstallAdvisorHosts() {
    static SystemHost system;
    static AdvisorHost advisor;
    static AdviceHost advice;
    static SubjectHost subject;
    static RewardHost reward;
    static ConcreteHost concrete;
    SetAdvisorSystemHost(&system);
    SetAdvisorHost(&advisor);
    SetAdviceHost(&advice);
    SetAdviceSubjectHost(&subject);
    SetRewardAdviceHost(&reward);
    SetAdvisorConcreteHost(&concrete);
    static AdvisorWindowHost window;
    SetAdvisorUIAdvisorHost(&window);
    static AdviceDetokenizerHostImpl tokens;
    SetAdviceDetokenizerHost(&tokens);
}

// Runs `code` in the app's Lua (the advisors' VM) and prints its results: the scenario runner's `alua CODE`.
void NativeRunAppLua(const char* code) {
    cISCLua* lua = AppLua();
    if (!lua) { std::printf("alua: no app Lua\n"); return; }
    const int top = lua->GetTop();
    const std::string chunk = std::string("return ") + code;
    int status = lua->DoString(chunk.c_str());
    if (status != 0) { lua->SetTop(top); status = lua->DoString(code); }
    std::printf("alua: %s -> status %d:", code, status);
    for (int i = top + 1; i <= lua->GetTop(); ++i) {
        const char* text = lua->IsString(i) || lua->IsNumber(i) ? lua->ToString(i) : lua->TypeName(lua->Type(i));
        std::printf(" %s", text ? text : "?");
    }
    std::printf("\n");
    lua->SetTop(top);
}

// The advisor window's state (the scenario runner's `advwin`): layout, advisor, list entries, each head's mood and
// whether its procedure is attached, and the visibility of the three pages.
void NativeAdvisorWindowReport() {
    cSC4AdvisorUIAdvisorWinProc* proc = NativeAdvisorWinProc();
    if (!proc) { std::printf("advwin: no advisor window\n"); return; }
    std::printf("advwin: window %s visible %d layout %d large %d advisor %d index %d listed %zu list %s notifications %d\n",
                proc->Window() ? "yes" : "NO", proc->IsVisible(), proc->Layout(), proc->Large(), proc->AdvisorId(),
                proc->AdvisorIndex(), proc->Messages().size(), proc->AdviceList() ? "yes" : "no", proc->NotificationsRegistered());
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
        cSC4AdvisorUIImageWinProc* head = proc->Head(i);
        if (!head) continue;
        std::printf("  head %d advisor 0x%x mood %d enabled %d model %08x\n", i, kAdvisorWindowAdvisorIds[i], head->Mood(),
                    head->Enabled(), head->ModelIdA());
    }
    cIGZWin* window = proc->Window() ? proc->Window()->AsIGZWin() : nullptr;
    cIGZWin* parent = window ? window->GetParentWin() : nullptr;
    for (std::uint32_t id : {kAdvisorWindowAll, kAdvisorWindowSmall, kAdvisorWindowLarge}) {
        cIGZWin* page = parent ? parent->GetChildWindowFromIDRecursive(id) : nullptr;
        std::printf("  page %08x %s\n", id, page ? (page->IsVisible() ? "visible" : "hidden") : "missing");
    }
    for (const auto& entry : proc->Messages()) {
        if (cISC4Advice* a = entry.get()) std::printf("  listed mood %d current %d \"%s\"\n", a->GetMood(), a->CheckFlags(2), a->GetTextTitle().ToChar());
    }
}

// The scenario runner's `adlink ADVISOR CODE`: follows "sc4://advice/<advisor><guid>CODE" for the advisor's first
// active advice through NativeHtmlLinkDoURL (the link an advice page's anchor carries).
void NativeAdviceLinkProbe(int advisorId, const char* code) {
    cSC4AdvisorSystem* system = System();
    cISC4Advisor* advisor = system ? system->GetAdvisor(advisorId) : nullptr;
    if (!advisor || advisor->GetCurrentAdvices().empty()) { std::printf("adlink: advisor %d has no active advice\n", advisorId); return; }
    cISC4Advice* advice = advisor->GetCurrentAdvices().begin()->first.get();
    char head[64];
    std::snprintf(head, sizeof head, "sc4://advice/%08x%08x", static_cast<unsigned>(advisorId), static_cast<unsigned>(advice->GetSessionID()));
    const std::string url = std::string(head) + code;
    const bool followed = NativeHtmlLinkDoURL(cRZString(url.c_str()));
    std::printf("adlink: %s -> %s\n", url.c_str(), followed ? "followed" : "NOT followed");
}

// The advisors and their active advices (the scenario runner's `advisors`).
void NativePrintAdvisors() {
    cSC4AdvisorSystem* system = System();
    if (!system) { std::printf("advisors: no advisor system\n"); return; }
    AdvisorMap* advisors = system->GetAdvisors();
    std::printf("advisors: %zu\n", advisors ? advisors->size() : 0u);
    if (!advisors) return;
    for (auto& [id, advisor] : *advisors) {
        if (!advisor) continue;
        const AdviceMap& active = advisor->GetCurrentAdvices();
        std::printf("  advisor %d %s: %zu active\n", id, advisor->GetName().ToChar(), active.size());
        for (const auto& entry : active) {
            cISC4Advice* a = entry.first.get();
            if (!a) continue;
            std::printf("    advice 0x%08x mood %d priority %d event %d title \"%s\"\n", static_cast<std::uint32_t>(a->GetGUID()), a->GetMood(),
                        a->GetPriority(), a->GetEventID(), a->GetTextTitle().ToChar());
            if (std::getenv("SC4_ADVICETEXT")) std::printf("      message \"%s\"\n", a->GetTextMessage().ToChar());
        }
    }
}

// Is `occupant` the running mission's current target (city vslot 0xc4 GetCitySituationManager, its vslot 0x24)? The same
// object answers the same cIGZUnknown.
bool NativeIsSituationTarget(cIGZUnknown* occupant) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::CitySituationManager) : nullptr;
    auto* situations = object ? dynamic_cast<cSC4CitySituationManager*>(object) : nullptr;
    ISituationOccupant* target = situations ? situations->GetCurrentTarget() : nullptr;
    if (!target || !occupant) return false;
    void *a = nullptr, *b = nullptr;
    const bool same = target->QueryInterface(kGZIID_cIGZUnknown, &a) && occupant->QueryInterface(kGZIID_cIGZUnknown, &b) && a == b;
    if (a) static_cast<cIGZUnknown*>(a)->Release();
    if (b) static_cast<cIGZUnknown*>(b)->Release();
    return same;
}

}  // namespace native

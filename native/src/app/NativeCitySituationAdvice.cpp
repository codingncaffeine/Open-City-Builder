// The city situation advice's services on the native application (sc4::ICitySituationAdviceHost): the advisor system's
// script state (the original's SL::spLua: the "advices" table the situations read lives there), the city size, the linked
// "evil twin" advice and the advisor system's context-advice stack. Also the sc4::ISituationLua view of a cISCLua that the
// situation manager's host shares.
#include <vector>

#include "app/NativeCity.h"
#include "lua/ISCLua.h"
#include "lua/SCLuaHelp.h"
#include "sc4/Advice.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/CitySituationAdvice.h"
#include "sc4/cSC4City.h"

namespace native {
cISCLua* NativeCityScriptingContext();   // NativeAdvisors.cpp: the advisor system's script state

namespace {
using namespace sc4;

// sc4::ISituationLua on a cISCLua (each slot is the cISCLua call at the vslot the seam names).
class SituationLua final : public ISituationLua {
public:
    void Bind(cISCLua* lua) { lua_ = lua; }
    int GetTop() override { return lua_->GetTop(); }
    void SetTop(int top) override { lua_->SetTop(top); }
    void PushValue(int index) override { lua_->PushValue(index); }
    void Pop(int count) override { lua_->Pop(count); }
    bool IsNumber(int index) override { return lua_->IsNumber(index); }
    double ToNumber(int index) override { return lua_->ToNumber(index); }
    void PushNumber(double value) override { lua_->PushNumber(value); }
    void GetGlobal(const char* name) override { lua_->GetGlobal(name); }
    void RawGetTable(int index) override { lua_->GetTable(index); }
    void RawSetTable(int index) override { lua_->SetTable(index); }
    void GetRef(int ref) override { lua_->GetRef(ref); }
    void NewTable() override { lua_->NewTable(); }
    int Ref(bool lockValue) override { return lua_->Ref(lockValue ? 1 : 0); }
    void CallProtected(int argCount, int resultCount) override { lua_->CallProtected(argCount, resultCount); }
    void Unref(int ref) override { lua_->Unref(ref); }
    bool IsFunction(int index) override { return lua_->IsFunction(index); }
    bool IsTable(int index) override { return lua_->IsTable(index); }
    bool GetGlobalTable(const char* name) override { return SCLuaHelp::GetGlobal(lua_, name); }
    bool GetField(const char* name, int tableIndex) override { return SCLuaHelp::GetField(lua_, name, tableIndex); }
    void ReadFieldU32(const char* name, std::uint32_t& value, int tableIndex) override {
        unsigned v = value;
        if (SCLuaHelp::ReadTableField(lua_, name, v, tableIndex)) value = v;
    }
    void ReadFieldInt(const char* name, std::int32_t& value, int tableIndex) override {
        int v = value;
        if (SCLuaHelp::ReadTableField(lua_, name, v, tableIndex)) value = v;
    }
    void ReadFieldFloat(const char* name, float& value, int tableIndex) override {
        float v = value;
        if (SCLuaHelp::ReadTableField(lua_, name, v, tableIndex)) value = v;
    }
    void ReadFieldBool(const char* name, bool& value, int tableIndex) override {
        bool v = value;
        if (SCLuaHelp::ReadTableField(lua_, name, v, tableIndex)) value = v;
    }
    void ReadFieldString(const char* name, cRZString& value, int tableIndex) override {
        cRZString v;
        if (SCLuaHelp::ReadTableField(lua_, name, v, tableIndex)) value = v;
    }
    void ReadTableAsArrayU32(std::vector<std::uint32_t>& out, int tableIndex) override {
        std::vector<unsigned> values;
        out.clear();
        if (SCLuaHelp::ReadTableAsArray(lua_, values, tableIndex)) out.assign(values.begin(), values.end());
    }
    void ReadTableAsArrayString(std::vector<cRZString>& out, int tableIndex) override {
        out.clear();
        SCLuaHelp::ReadTableAsArray(lua_, out, tableIndex);
    }
    // SCLuaTableRef's walk: every key/value pair of the table on top of the stack, the value pushed for the visitor.
    void ForEachEntry(void (*visit)(void* context), void* context) override {
        const int table = lua_->GetTop();
        lua_->PushNil();
        while (lua_->Next(table) != 0) {
            visit(context);
            lua_->Pop(1);
        }
    }

private:
    cISCLua* lua_ = nullptr;
};

}  // namespace

// The situation code's view of the advisor system's script state (null before the city's advisor system exists).
sc4::ISituationLua* NativeSituationLua() {
    static SituationLua view;
    cISCLua* lua = NativeCityScriptingContext();
    if (!lua) return nullptr;
    view.Bind(lua);
    return &view;
}

namespace {

class CitySituationAdviceHost final : public ICitySituationAdviceHost {
public:
    ISituationLua* Lua() override { return NativeSituationLua(); }
    int CityCellsX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    float CityCellSizeXMeters() override {
        cSC4City* city = HostCity();
        return city && city->CellCountX() > 0 ? city->SizeX() / static_cast<float>(city->CellCountX()) : 16.0f;
    }
    // The advisor's advice of that guid answers GetTrigger (advice vslot 0x40).
    bool IsLinkedAdviceAvailable(int advisorId, std::uint32_t guid) override {
        cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
        cISC4Advisor* advisor = system ? system->GetAdvisor(advisorId) : nullptr;
        cISC4Advice* advice = advisor ? advisor->GetAdviceByGUID(static_cast<int>(guid)) : nullptr;
        return advice && advice->GetTrigger();
    }
    // cSC4AdvisorSystem vslot 0x98: PopContextAdvice.
    void AdvisorSystemReleaseHook() override {
        if (cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance()) system->PopContextAdvice();
    }
    // cSC4AdvisorSystem vslot 0x90: PushContextAdvice(the situation's cSC4Advice).
    void AdvisorSystemRegister(void* base) override {
        cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
        auto* view = static_cast<ISituationAdviceBase*>(base);
        cIGZUnknown* outer = view ? view->Outer() : nullptr;
        if (system && outer) system->PushContextAdvice(static_cast<cISC4Advice*>(outer));
    }
} g_adviceHost;

}  // namespace

void InstallCitySituationAdvice() { SetCitySituationAdviceHost(&g_adviceHost); }

}  // namespace native

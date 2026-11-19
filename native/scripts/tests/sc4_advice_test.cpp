#include <cstdio>
#include <map>
#include <string>

#include "fake_resourcemanager.h"
#include "fake_scriptlua.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZSysServPtr.h"
#include "sc4/Advice.h"
#include "sc4/DBSegment.h"
#include "sc4/TablePropertyReader.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
using namespace fakelua;

namespace {
struct Manager final : FakeResourceManager {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> strings;
    bool GetResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t, cIGZUnknown*) override {
        if (iid != kGZIID_cIGZString || key.type != kResourceTypeSLString) return false;
        auto it = strings.find({key.group, key.instance});
        if (it == strings.end()) return false;
        *out = new cRZString(it->second.c_str());
        static_cast<cIGZString*>(*out)->AddRef();
        return true;
    }
};
Manager g_manager;
bool Resolve(std::uint32_t service, std::uint32_t iid, void** out) {
    if (service == 0x056b906e && iid == 0x656b8efc) { *out = static_cast<cIGZPersistResourceManager*>(&g_manager); return true; }
    return false;
}
struct SlHost final : ISLStringHost {
    int LanguageOffset() override { return 0; }
    cSCStringDetokenizer* Detokenizer() override { return nullptr; }
};

struct Host final : IAdviceHost {
    FakeLua lua;
    bool present = true, debug = false, view3d = true;
    int begins = 0, ends = 0;
    cSC4Advice* lastOwner = nullptr;
    std::vector<std::uint32_t> on, off;
    bool AdvisorSystemPresent() override { return present; }
    IScriptLua* Lua() override { return &lua; }
    void BeginScript(cSC4Advice* a) override { ++begins; lastOwner = a; }
    void EndScript() override { ++ends; }
    bool DebugFunctionalityEnabled() override { return debug; }
    cISC4Advice* CreateAdvice(std::uint32_t clsid, std::uint32_t iid) override {
        if (clsid != kGZCLSID_cSC4Advice || iid != kGZIID_cISC4Advice) return nullptr;
        auto* a = new cSC4Advice();
        a->AddRef();
        return a;
    }
    bool HasView3D() override { return view3d; }
    void GlobalEffectOn(std::uint32_t id) override { on.push_back(id); }
    void GlobalEffectOff(std::uint32_t id) override { off.push_back(id); }
};

ValuePtr Row(std::map<std::string, ValuePtr> fields) {
    ValuePtr t = Tab();
    t->table->byName = std::move(fields);
    return t;
}
std::string S(const cIGZString& s) { return std::string(s.Data(), s.Strlen()); }
}  // namespace

int main() {
    SlHost slHost;
    SetSLStringHost(&slHost);
    SetRZServiceResolver(&Resolve);
    g_manager.strings[{kAdvisorTextGroup, 5}] = "Build more parks!";
    g_manager.strings[{kAdvisorTextGroup, 6}] = "A title";

    Host host;
    SetAdviceHost(&host);
    ValuePtr advices = Tab();
    advices->table->length = 3;
    advices->table->byIndex[2] = Row({{"guid", Num(0x77)}, {"priority", Num(9)}, {"mood", Num(2)}, {"frequency", Num(5)},
                                      {"timeout", Num(6)}, {"once", Num(1)}, {"news_only", Num(0)}, {"event", Num(3)},
                                      {"command", Num(4)}, {"persist", Num(1)}, {"no_timeout", Num(1)},
                                      {"class_id", Num(0x8a09f5f4)}, {"trigger", Str("1")},
                                      {"message", Str("text@0x00000005")}, {"title", Str("text@0x00000006")}});
    {
        ValuePtr effects = Tab();
        effects->table->length = 2;
        effects->table->byIndex[1] = Num(10);
        effects->table->byIndex[2] = Num(20);
        advices->table->byIndex[2]->table->byName["effects"] = effects;
    }
    advices->table->byIndex[3] = Row({{"guid", Num(0x78)}, {"trigger", Str("false")}, {"class_id", Num(0x1234)},
                                      {"message", Str("text@0x00000063")}, {"title", Str("")}, {"effects", Num(77)}});
    advices->table->byIndex[1] = Row({{"guid", Num(0x79)}, {"trigger", Str("")}, {"class_id", Num(0x8a09f5f4)}});
    host.lua.globals["advices"] = advices;

    // ---- cTablePropertyReader
    {
        cTablePropertyReader r(&host.lua, "advices", 2);
        CHECK(r.IsValid() && r.GetNumber("guid") == 0x77 && r.GetNumber("nothing") == 0);
        CHECK(r.HasField("guid") && r.HasField("trigger") && !r.HasField("effects") && !r.HasField("nothing"));
        CHECK(S(r.GetString("trigger")) == "1");
        CHECK(host.lua.GetTop() == 2);   // the table and the row
    }
    CHECK(host.lua.GetTop() == 0);       // destructor restored the top
    {
        cTablePropertyReader bad(&host.lua, "advices", 9);   // beyond the table length
        CHECK(!bad.IsValid() && bad.GetNumber("guid") == 0 && !bad.HasField("guid"));
        cTablePropertyReader zero(&host.lua, "advices", 0);  // id 0 keeps the table itself
        CHECK(zero.IsValid());
        cTablePropertyReader negative(&host.lua, "advices", -1);
        CHECK(!negative.IsValid());
    }
    CHECK(host.lua.GetTop() == 1);   // invalid readers leave their table behind (kept quirk); the valid one trims it again
    host.lua.SetTop(0);

    // ---- construction defaults
    cSC4Advice* a = new cSC4Advice();
    a->AddRef();
    CHECK(a->GetPriority() == 50 && a->GetFrequencyInDays() == 30 && a->GetTimeOutInDays() == 30 && a->GetMood() == 3);
    CHECK(a->CheckFlags(kAdviceFlagTextDirty) && !a->IsCurrent() && a->LuaRef() == -2 && a->GetGZCLSID() == 0x8a09f5f4);
    // quirk: cADVString(char const*) only parses the key, so the placeholder texts are empty strings with key 0
    CHECK(S(a->GetTextTitle()).empty() && S(a->GetTextMessage()).empty() && a->GetTextTitleGUID() == 0);
    cSC4Advice* b = new cSC4Advice();
    b->AddRef();
    CHECK(b->GetSessionID() == a->GetSessionID() + 1);
    b->Release();
    CHECK(!a->Init(0) && !a->Init(7));
    CHECK(a->CheckFlags(0) && a->AdviceId() == 7);   // the id is stored before the row check
    CHECK(host.lua.GetTop() == 1);                   // quirk: an invalid reader never restores the stack (the table stays)
    host.lua.SetTop(0);

    // ---- Init
    a->Release();
    a = new cSC4Advice();
    a->AddRef();
    CHECK(a->Init(2));
    CHECK(a->GetGUID() == 0x77 && a->GetPriority() == 9 && a->GetMood() == 2 && a->GetFrequencyInDays() == 5 && a->GetTimeOutInDays() == 6);
    CHECK(a->CheckFlags(kAdviceFlagOnce) && !a->CheckFlags(kAdviceFlagNewsOnly) && a->CheckFlags(kAdviceFlagEvent));
    CHECK(a->CheckFlags(kAdviceFlagPersist) && a->CheckFlags(kAdviceFlagNoTimeout) && !a->CheckFlags(kAdviceFlagCustomClass));
    CHECK(a->GetEventID() == 3 && a->GetCommandID() == 4 && a->LuaRef() != -2);
    CHECK(host.lua.lastChunk == "_atrg_ = function () return (1); end");
    CHECK(a->GlobalEffects().size() == 2 && a->GlobalEffects().front() == 10 && a->GlobalEffects().back() == 20);
    CHECK(!a->CheckFlags(kAdviceFlagNoText) && !a->IsCurrent() && a->GetTextMessageGUID() == 5 && a->GetTextTitleGUID() == 6);
    CHECK(host.lua.GetTop() == 0);
    CHECK(a->GetTrigger());
    CHECK(host.begins == host.ends && host.lastOwner == a);
    CHECK(S(a->GetTriggerString()).empty());   // only filled when debug functionality is on
    // a bound guid must match the row
    {
        cSC4Advice* e = new cSC4Advice();
        e->AddRef();
        CHECK(e->Init(2) && !e->Init(3));
        CHECK(e->GetGUID() == 0x77 && e->AdviceId() == 3);   // quirk @ 0x10043d1f5: the row id is stored before the guid check
        e->Release();
    }

    // forced trigger / the NoText flag mutes the trigger
    a->SetFlags(kAdviceFlagForceTrigger, true);
    CHECK(a->GetTrigger());
    a->SetFlags(kAdviceFlagForceTrigger, false);
    a->SetFlags(kAdviceFlagNoText, true);
    CHECK(!a->GetTrigger());
    a->SetFlags(kAdviceFlagNoText, false);
    // a failing trigger call answers false and leaves the callee on the stack (kept)
    host.lua.callFails = true;
    CHECK(!a->GetTrigger() && host.lua.GetTop() == 1);
    host.lua.callFails = false;
    host.lua.SetTop(0);

    // ---- the other rows
    cSC4Advice* c = new cSC4Advice();
    c->AddRef();
    CHECK(c->Init(3) && c->CheckFlags(kAdviceFlagCustomClass) && !c->GetTrigger());
    CHECK(c->GlobalEffects().size() == 1 && c->GlobalEffects().front() == 77);   // a single number
    CHECK(!c->CheckFlags(kAdviceFlagNoText));   // message set, title empty
    cSC4Advice* d = new cSC4Advice();
    d->AddRef();
    CHECK(d->Init(1) && d->CheckFlags(kAdviceFlagNoText) && d->GetTrigger() == false);   // trigger "" becomes "0"
    CHECK(host.lua.lastChunk == "_atrg_ = function () return (0); end");
    d->Release();

    // ---- debug trigger text
    host.debug = true;
    cSC4Advice* dbg = new cSC4Advice();
    dbg->AddRef();
    CHECK(dbg->Init(2) && S(dbg->GetTriggerString()) == "_atrg_=(1)");
    dbg->Release();
    host.debug = false;

    // ---- SetFlags semantics: current loads text, notifies global effects
    CHECK(a->SetFlags(kAdviceFlagCurrent, true));
    CHECK(a->IsCurrent() && !a->CheckFlags(kAdviceFlagTextDirty));
    CHECK(S(a->GetTextMessage()) == "Build more parks!" && S(a->GetTextTitle()) == "A title");
    CHECK(host.on.size() == 2 && host.on[0] == 10 && host.on[1] == 20 && host.off.empty());
    a->SetFlags(kAdviceFlagCurrent, true);   // already current: no second notification
    CHECK(host.on.size() == 2);
    a->SetFlags(kAdviceFlagCurrent, false);
    CHECK(!a->IsCurrent() && a->CheckFlags(kAdviceFlagTextDirty) && host.off.size() == 2);
    host.view3d = false;
    a->SetFlags(kAdviceFlagCurrent, true);
    CHECK(host.on.size() == 2);              // no 3D view: nothing is switched
    host.view3d = true;
    a->SetFlags(kAdviceFlagCurrent, false);

    // text that is not in the database; debug keys
    c->SetFlags(kAdviceFlagCurrent, true);
    CHECK(S(c->GetTextMessage()) == "***_NOT_IN_TEXT_DATABASE_*** text@0x00000063");
    a->SetFlags(kAdviceFlagShowTextKeys, true);
    CHECK(a->CheckFlags(kAdviceFlagTextDirty));
    CHECK(a->UpdateText());
    CHECK(S(a->GetTextMessage()) == "=text@00000005= Build more parks! =adv@00000077=");
    a->SetFlags(kAdviceFlagShowTextKeys, false);
    c->Release();

    // ---- links run only for a current advice without flag 8
    a->DoLink(cRZString("go()"));
    CHECK(host.lua.buffers.empty());
    a->SetFlags(kAdviceFlagCurrent, true);
    a->DoLink(cRZString("go()"));
    CHECK(host.lua.buffers.size() == 1 && host.lua.buffers[0] == "go()");
    a->SetFlags(kAdviceFlagNoLinks, true);
    a->DoLink(cRZString("never()"));
    CHECK(host.lua.buffers.size() == 1);
    a->SetFlags(kAdviceFlagNoLinks, false);
    a->SetFlags(kAdviceFlagCurrent, false);

    // ---- event subject & clone
    CHECK(a->GetEventSubject() == nullptr);
    {
        cISC4Advice* clone = nullptr;
        CHECK(!a->Clone(nullptr));
        CHECK(a->Clone(&clone) && clone);   // no subject: the clone carries none
        CHECK(clone->GetGUID() == 0x77 && clone->GetEventSubject() == nullptr);
        a->SetAdvisorID(12);
        cISC4Advice* clone2 = nullptr;
        auto* subject = new cSC4AdviceSubject();
        subject->AddRef();
        subject->SetData1(42);
        a->SetEventSubject(subject);
        CHECK(a->GetEventSubject() == subject);
        CHECK(a->Clone(&clone2) && clone2->GetAdvisorID() == 12);
        CHECK(clone2->GetEventSubject() != nullptr && clone2->GetEventSubject() != subject && clone2->GetEventSubject()->GetData1() == 42);
        clone2->Release();
        clone->Release();
        subject->Release();
        cSC4Advice* unbound = new cSC4Advice();
        unbound->AddRef();
        cISC4Advice* none = nullptr;
        CHECK(!unbound->Clone(&none) && none == nullptr);   // advice id 0
        unbound->Release();
    }

    // ---- stream round trip
    {
        a->SetEventSubject(nullptr);
        a->SetFlags(kAdviceFlagCurrent, true);
        PortMemoryOStream os;
        CHECK(a->Write(os));
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4Advice* r = new cSC4Advice();
        r->AddRef();
        CHECK(r->Read(is));
        CHECK(r->AdviceId() == 2 && r->GetGUID() == 0x77 && r->GetAdvisorID() == 12 && r->GetPriority() == 9);
        CHECK(r->GetFrequencyInDays() == 5 && r->GetTimeOutInDays() == 6 && r->GetMood() == 2);
        CHECK(r->IsCurrent() && r->CheckFlags(kAdviceFlagOnce) && r->GlobalEffects().size() == 2 && r->LoadedFromSave());
        CHECK(r->CheckFlags(kAdviceFlagTextDirty) && r->GetEventSubject() == nullptr);
        r->Release();
        a->SetFlags(kAdviceFlagCurrent, false);
    }
    // version 3 records have no effect list
    {
        PortMemoryOStream os;
        os.SetUint16(3);
        for (int i = 0; i < 8; ++i) os.SetSint32(i + 1);
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4Advice* r = new cSC4Advice();
        r->AddRef();
        r->Read(is);
        CHECK(r->AdviceId() == 1 && r->GetGUID() == 2 && r->GetMood() == 8 && r->GlobalEffects().empty());
        r->Release();
    }

    // ---- interfaces
    {
        void* iface = nullptr;
        CHECK(a->QueryInterface(kGZIID_cISC4Advice, &iface) && iface == static_cast<cISC4Advice*>(a));
        CHECK(a->QueryInterface(kGZIID_cIGZSerializable, &iface) && iface == static_cast<cIGZSerializable*>(a));
        CHECK(a->QueryInterface(kGZIID_cIGZUnknown, &iface));
        CHECK(!a->QueryInterface(0x55, &iface));
        a->Release(); a->Release(); a->Release();   // three references taken by the queries
    }

    // ---- shutdown releases the trigger reference
    {
        const int ref = a->LuaRef();
        CHECK(ref != -2);
        host.present = false;
        CHECK(a->Shutdown() && a->LuaRef() == ref);   // no advisor system: nothing happens
        host.present = true;
        CHECK(a->Shutdown() && a->LuaRef() == -2 && host.lua.unrefs.size() == 1 && host.lua.unrefs[0] == ref);
        CHECK(a->Shutdown() && host.lua.unrefs.size() == 1);
    }
    a->Release();

    std::printf(failures ? "FAILED\n" : "sc4_advice OK\n");
    return failures ? 1 : 0;
}

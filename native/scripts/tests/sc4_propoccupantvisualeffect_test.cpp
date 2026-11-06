// cSC4PropOccupantVisualEffect tests: layout and defaults, Init (box property, effect-name / effect-type
// variants), RestartEffect lookups, the transform copy, Start/Stop gating by the occupant flag byte, the state
// switch, "off", serialisation quirks, QueryInterface and Shutdown.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "sc4/SCPropertyHelp.h"

using namespace sc4;

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

class FakeLink final : public IEffectsManagerLink {
public:
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void StartAncilliary(cSC4VisualEffect*) override { ++starts; }
    void StopAncilliary(cSC4VisualEffect*) override { ++stops; }
    int refs = 0, starts = 0, stops = 0;
};

class FakeHost final : public IPropVisualEffectHost {
public:
    struct Created {
        std::string name;   // empty for a type lookup
        std::uint32_t group = 0;
        std::uint32_t type = 0;
        cSC4VisualEffect* effect = nullptr;
    };

    ~FakeHost() override {
        for (cSC4VisualEffect* effect : owned_) effect->Release();
    }

    bool CreateEffectByName(const char* name, cSC4VisualEffect** out) override {
        if (failNext) {
            failNext = false;
            return false;
        }
        Created record;
        record.name = name;
        record.effect = Make();
        created.push_back(record);
        *out = record.effect;
        return true;
    }

    bool CreateEffectByType(std::uint32_t group, std::uint32_t type, cSC4VisualEffect** out) override {
        if (failNext) {
            failNext = false;
            return false;
        }
        Created record;
        record.group = group;
        record.type = type;
        record.effect = Make();
        created.push_back(record);
        *out = record.effect;
        return true;
    }

    FakeLink link;
    std::vector<Created> created;
    bool failNext = false;

private:
    cSC4VisualEffect* Make() {
        auto* effect = new cSC4VisualEffect();
        effect->AddRef();   // the manager's reference, handed to the occupant
        effect->AddRef();   // the fake's reference, so the test can inspect it after the occupant releases it
        effect->SetManager(&link);
        owned_.push_back(effect);
        return effect;
    }
    std::vector<cSC4VisualEffect*> owned_;
};

cSC4PropOccupantVisualEffect* NewObject() {
    auto* object = new cSC4PropOccupantVisualEffect();
    object->AddRef();
    return object;
}

void AddString(cISCPropertyHolder* holder, std::uint32_t id, const char* text) {
    const cRZString value(text);
    cSCPropertyHelp::SetPropertyValue(holder, id, static_cast<const cIGZString&>(value));
}

void AddUint(cISCPropertyHolder* holder, std::uint32_t id, std::uint32_t value) {
    cSCPropertyHelp::SetPropertyValue(holder, id, value);
}

bool BitwiseEqual(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

int main() {
    FakeHost host;
    SetPropVisualEffectHost(&host);

    // --- layout and construction -------------------------------------------------------------
    auto* object = NewObject();
    // The original object is 0xe8 bytes with the effect name at +0xb0; the port's prop base carries the original's
    // cIGZMessageTarget2 view as a C++ base, whose vptr the port's layout had left out: 8 more bytes ahead of the members.
    constexpr std::size_t kTargetView = 8;
    CHECK(sizeof(cSC4PropOccupantVisualEffect) == 0xe8 + kTargetView);
    CHECK(object->GetGZCLSID() == 0x2977aa49);
    CHECK(object->AsOccupant() == static_cast<cISC4Occupant*>(object->AsPropOccupant()));
    CHECK(reinterpret_cast<const char*>(object->AsPropOccupant()) - reinterpret_cast<const char*>(object) == 0x08);
    CHECK(reinterpret_cast<const char*>(&object->EffectName()) - reinterpret_cast<const char*>(object) == 0xb0 + kTargetView);
    CHECK(object->Effect() == nullptr && !object->EffectActive() && object->DefaultTransition());
    CHECK(object->EffectName().Strlen() == 0 && object->EffectType() == 0);
    CHECK(object->NameVariantCount() == 0 && object->TypeVariantCount() == 0);
    CHECK(host.created.empty());

    // --- Init: default box, no effect ------------------------------------------------------
    CHECK(object->Init());
    CHECK(object->IsInitialized());
    float lo[3], hi[3];
    object->GetBoundingBox(lo, hi);
    CHECK(lo[0] == -8.0f && lo[1] == 0.0f && lo[2] == -8.0f);
    CHECK(hi[0] == 8.0f && hi[1] == 16.0f && hi[2] == 8.0f);
    CHECK(object->Effect() == nullptr && host.created.empty());
    CHECK(object->Init());   // already initialised: no second read

    // --- Init: the box-size property -------------------------------------------------------
    auto* sized = NewObject();
    cSCPropertyHelp::SetPropertyValue(sized->AsPropertyHolder(), kPropertyPropEffectBoxSize,
                                      std::vector<float>{10.0f, 20.0f, 30.0f});
    CHECK(sized->Init());
    sized->GetBoundingBox(lo, hi);
    CHECK(lo[0] == -5.0f && lo[1] == 0.0f && lo[2] == -15.0f);
    CHECK(hi[0] == 5.0f && hi[1] == 20.0f && hi[2] == 15.0f);

    // --- Init: name variants and the effect lookup ----------------------------------------
    auto* named = NewObject();
    AddString(named->AsPropertyHolder(), kPropertyPropEffectName, "smoke");
    AddString(named->AsPropertyHolder(), kPropertyPropEffectName + 1, "fire");
    CHECK(named->Init());
    CHECK(named->NameVariantCount() == 2 && named->TypeVariantCount() == 0);
    CHECK(host.created.size() == 1);
    CHECK(host.created[0].name == "smoke" && host.created[0].group == 0);
    CHECK(named->Effect() == host.created[0].effect);
    CHECK(named->EffectActive());
    CHECK(!named->Effect()->IsStarted());   // the occupant flag byte has no "shown" bit yet (quirk)

    // The transform the effect received: translation = the prop's centre, flags 5, rotY(-orientation * pi/2).
    cS3DTransform transform;
    named->Effect()->GetEffectTransform(transform);
    CHECK(transform.flags == 5);
    CHECK(transform.translation[0] == 0.0f && transform.translation[1] == 0.0f && transform.translation[2] == 0.0f);
    CHECK(transform.rotation[0][0] == 1.0f && transform.rotation[1][1] == 1.0f && transform.rotation[2][2] == 1.0f);
    CHECK(BitwiseEqual(transform.rotation[0][1], -0.0f) && BitwiseEqual(transform.rotation[1][2], -0.0f));
    CHECK(BitwiseEqual(transform.rotation[1][0], 0.0f) && BitwiseEqual(transform.rotation[2][1], 0.0f));

    // SetPosition re-centres the box and copies the new centre into the effect transform.
    const float centre[3] = {1.0f, 2.0f, 3.0f};
    named->SetPosition(centre);
    named->GetBoundingBox(lo, hi);
    CHECK(lo[0] == -7.0f && lo[1] == 2.0f && lo[2] == -5.0f);
    CHECK(hi[0] == 9.0f && hi[1] == 18.0f && hi[2] == 11.0f);
    named->Effect()->GetEffectTransform(transform);
    CHECK(transform.translation[0] == 1.0f && transform.translation[1] == 2.0f && transform.translation[2] == 3.0f);

    // SetOrientation clamps to 0..3 and rotates about Y by -orientation * pi/2.
    CHECK(named->SetOrientation(1));
    CHECK(named->GetOrientation() == 1);
    named->Effect()->GetEffectTransform(transform);
    CHECK(std::fabs(transform.rotation[0][0]) < 1e-6f);
    CHECK(std::fabs(transform.rotation[0][2] + 1.0f) < 1e-6f);
    CHECK(std::fabs(transform.rotation[2][0] - 1.0f) < 1e-6f);
    CHECK(std::fabs(transform.rotation[2][2]) < 1e-6f);
    CHECK(named->SetOrientation(9) && named->GetOrientation() == 3);
    CHECK(named->SetOrientation(-5) && named->GetOrientation() == 0);

    // --- Start/Stop are gated by the "shown" occupant flag --------------------------------
    CHECK(!named->Effect()->IsStarted());
    CHECK(named->StartEffect(false));                   // effect exists -> true, but no flag -> no Start
    CHECK(named->EffectActive() && !named->Effect()->IsStarted());
    named->SetAllFlags(0x4 | kOccupantFlagShown);       // 0x4 -> 0x6: first start uses the default transition
    CHECK(named->Effect()->IsStarted());
    CHECK(!named->DefaultTransition());
    named->SetAllFlags(0x4);                            // clearing the flag stops the effect
    CHECK(!named->Effect()->IsStarted());
    CHECK(named->StopEffect(true));                     // flips the running flag, no flag -> no Stop call
    CHECK(!named->EffectActive());
    CHECK(!named->StopEffect(true));                    // not running any more

    // --- state selects the next name variant ----------------------------------------------
    CHECK(named->StartEffect(false));                  // re-arm the running flag after the StopEffect above
    named->SetAllFlags(0x4 | kOccupantFlagShown);
    CHECK(named->Effect()->IsStarted());
    const std::size_t beforeState = host.created.size();
    named->SetState(1);
    CHECK(named->GetState() == 1);
    CHECK(host.created.size() == beforeState + 1 && host.created.back().name == "fire");
    CHECK(named->Effect() == host.created.back().effect);
    CHECK(named->Effect()->IsStarted());                // the old active flag is re-applied
    CHECK(host.created[beforeState - 1].effect->IsStarted());   // old effect: released without a real stop (quirk)
    CHECK(host.created[beforeState - 1].effect->IsStopping());
    named->SetState(1);                                 // same state: nothing happens
    CHECK(host.created.size() == beforeState + 1);

    // A state past the variant count disables the effect but leaves the running flag set.
    named->SetState(2);
    CHECK(named->Effect() == nullptr && named->EffectActive());
    CHECK(host.created.size() == beforeState + 1);
    named->SetState(0);                                 // back to "smoke"
    CHECK(named->Effect() != nullptr && host.created.back().name == "smoke");

    // --- "off" disables the effect --------------------------------------------------------
    auto* off = NewObject();
    AddString(off->AsPropertyHolder(), kPropertyPropEffectName, "off");
    CHECK(off->Init());
    CHECK(off->NameVariantCount() == 1);
    CHECK(off->Effect() == nullptr);
    CHECK(off->EffectActive());

    // --- type-id property and SetEffectType ----------------------------------------------
    auto* typed = NewObject();
    AddUint(typed->AsPropertyHolder(), kPropertyPropEffectType, 0x1234);
    CHECK(typed->Init());
    CHECK(typed->TypeVariantCount() == 1 && typed->NameVariantCount() == 0);
    CHECK(typed->EffectType() == 0x1234);
    CHECK(host.created.back().group == kPropEffectGroupId && host.created.back().type == 0x1234);
    CHECK(host.created.back().name.empty());
    const std::size_t afterInit = host.created.size();
    typed->SetEffectType(0x2222);
    CHECK(typed->EffectType() == 0x2222 && typed->TypeVariantCount() == 0 && typed->NameVariantCount() == 0);
    CHECK(host.created.size() == afterInit + 1 && host.created.back().type == 0x2222);
    typed->SetEffectType(0x2222);                        // unchanged type id: no lookup
    CHECK(host.created.size() == afterInit + 1);
    typed->SetEffectType("water");
    CHECK(typed->EffectType() == 0);
    CHECK(host.created.size() == afterInit + 2 && host.created.back().name == "water");
    const std::size_t afterWater = host.created.size();
    typed->SetEffectType("water");                       // unchanged name: no lookup
    CHECK(host.created.size() == afterWater);
    // "off" only disables the effect through a variant property: on this plain-name path it is looked up as a name.
    typed->SetEffectType("off");
    CHECK(typed->Effect() != nullptr && host.created.back().name == "off");

    // A failing lookup keeps the running flag and leaves no effect.
    auto* failing = NewObject();
    AddString(failing->AsPropertyHolder(), kPropertyPropEffectName, "boom");
    host.failNext = true;
    CHECK(failing->Init());
    CHECK(failing->Effect() == nullptr && failing->EffectActive());

    // --- StartEffect with no effect -------------------------------------------------------
    auto* empty = NewObject();
    CHECK(empty->Init());
    CHECK(!empty->EffectActive());
    CHECK(!empty->StartEffect(true));                    // no effect: flips the flag and reports false
    CHECK(empty->EffectActive());
    CHECK(!empty->StartEffect(true));                    // already flagged: early false

    // --- QueryInterface -------------------------------------------------------------------
    auto* qi = NewObject();                              // reference count 1
    void* out = nullptr;
    CHECK(qi->QueryInterface(kGZCLSID_cSC4PropOccupantVisualEffect, &out));
    CHECK(out == static_cast<cISC4PropOccupantVisualEffect*>(qi));
    CHECK(qi->QueryInterface(kGZIID_cISC4PropOccupantVisualEffect, &out));
    CHECK(out == static_cast<cISC4PropOccupantVisualEffect*>(qi));
    CHECK(qi->QueryInterface(kGZIID_cISC4Occupant, &out));
    CHECK(out == static_cast<cISC4Occupant*>(qi));
    CHECK(!qi->QueryInterface(0xdeadbeefu, &out));
    qi->Release();                                       // 4 -> 3
    qi->Release();                                       // 3 -> 2
    qi->Release();                                       // 2 -> 1
    CHECK(qi->Release() == 0);                           // deletes the object

    // --- serialisation --------------------------------------------------------------------
    auto* writer = NewObject();
    AddString(writer->AsPropertyHolder(), kPropertyPropEffectName, "smoke");
    AddString(writer->AsPropertyHolder(), kPropertyPropEffectName + 1, "fire");
    CHECK(writer->Init());
    {
        PortMemoryOStream stream;
        CHECK(writer->Write(stream));
        PortMemoryIStream input(stream.bytes().data(), stream.bytes().size());
        auto* reader = NewObject();
        CHECK(reader->Read(input));
        // The variant counts are derived from the exemplar, never serialised, and the name is written empty when
        // a variant property exists.
        CHECK(reader->NameVariantCount() == 0 && reader->TypeVariantCount() == 0);
        CHECK(reader->EffectName().Strlen() == 0);
        CHECK(reader->EffectType() == 0);
        CHECK(reader->EffectActive() && reader->DefaultTransition());
        reader->Release();
    }

    // A plain name (no variant properties) is part of the record; Init's state is not re-run by Read.
    writer->SetEffectType("water");
    CHECK(writer->NameVariantCount() == 0 && writer->TypeVariantCount() == 0);
    {
        PortMemoryOStream stream;
        CHECK(writer->Write(stream));
        PortMemoryIStream input(stream.bytes().data(), stream.bytes().size());
        auto* reader = NewObject();
        CHECK(reader->Read(input));
        CHECK(reader->EffectName().Strlen() == 5 && std::strcmp(reader->EffectName().Data(), "water") == 0);
        CHECK(reader->EffectActive());
        reader->Release();
    }

    // Wrong record version: the body is skipped and Read reports false.
    {
        const std::uint8_t wrongVersion[] = {2, 0};
        PortMemoryIStream input(wrongVersion, sizeof wrongVersion);
        auto* reader = NewObject();
        CHECK(!reader->Read(input));
        reader->Release();
    }

    // --- Shutdown: the effect stops with transition 0 (a fade), so it keeps "started" -
    auto* shutting = NewObject();
    AddString(shutting->AsPropertyHolder(), kPropertyPropEffectName, "smoke");
    CHECK(shutting->Init());
    shutting->SetAllFlags(0x4 | kOccupantFlagShown);
    CHECK(shutting->Effect()->IsStarted());
    cSC4VisualEffect* faded = shutting->Effect();
    CHECK(shutting->Shutdown());
    CHECK(!shutting->IsInitialized());
    CHECK(shutting->Effect() == nullptr);
    CHECK(faded->IsStarted() && faded->IsStopping());    // transition 0 keeps the started flag (quirk)
    shutting->Release();

    // --- release everything ----------------------------------------------------------------
    named->Release();
    off->Release();
    typed->Release();
    failing->Release();
    empty->Release();
    sized->Release();
    writer->Release();
    object->Release();
    SetPropVisualEffectHost(nullptr);

    std::puts(failures ? "sc4_propoccupantvisualeffect: FAILED" : "sc4_propoccupantvisualeffect ok");
    return failures != 0;
}

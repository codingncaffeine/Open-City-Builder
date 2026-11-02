// cSC4EffectsCollection: the effects-description library. Checks the six name tables (case-insensitive through the shared
// lowercase scratch), the (name, version) keyed effect table with its record / event index resolution, the component
// buckets with their reference counting, the camera parameters, the effect bindings and message triggers, the
// effect-priority / alias bookkeeping and the three collection merges (swap, by name, by renumbering) with every quirk
// listed in the header. The sink interface stands in for the unrecovered cISC4EffectsCollection.
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "sc4/EffectsCollection.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

template <class T>
bool Equals(const std::vector<T>& value, std::initializer_list<T> expected) {
    if (value.size() != expected.size()) return false;
    std::size_t i = 0;
    for (const T& item : expected)
        if (!(value[i++] == item)) return false;
    return true;
}

bool Same(const cS3DVector3& a, const cS3DVector3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

std::string Num(float value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.3f", static_cast<double>(value));
    return text;
}

cS3DVector3 Vec3(float x, float y, float z) {
    cS3DVector3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

// ---- fakes -----------------------------------------------------------------------------------------------------
// A component description whose Type() the collection buckets on. Release() on the base deletes the object once the
// last reference is gone, so the destructor counter is the reference-counting oracle.
struct FakeComponent final : cISC4ComponentDescription {
    explicit FakeComponent(std::uint32_t t) : type(t) {}
    ~FakeComponent() override { ++destroyed; }
    std::uint32_t Type() const override { return type; }
    cISC4VisualEffectComponent* Instantiate() override { return nullptr; }
    void Read(cIGZIStream&) override {}
    void Write(cIGZOStream&) const override {}
    std::uint32_t type;
    static int destroyed;
};
int FakeComponent::destroyed = 0;

// cISC4EffectsCollection's seven Add* slots: records what AddToCollectionWithNames forwards, in which order and with
// which stored (lowercase) name and payload.
struct RecordingSink final : IEffectsCollectionHost {
    struct Call {
        std::string kind;
        std::string name;
        std::string value;
    };
    std::vector<Call> calls;

    void AddEffectDescription(const char* name, const cSC4EffectDescription& description) override {
        calls.push_back({"effect", name, description.label});
    }
    void AddParticleDescription(const char* name, const cSC4ParticlesDescription& description) override {
        calls.push_back({"particle", name, Num(description.lifetimeMin)});
    }
    void AddDynamicParticleDescription(const char* name,
                                       const cSC4DynamicParticleDescription& description) override {
        calls.push_back({"dynamic", name, Num(description.values[0])});
    }
    void AddDecalDescription(const char* name, const cSC4DecalDescription& description) override {
        calls.push_back({"decal", name, Num(description.life)});
    }
    void AddShakeDescription(const char* name, const cSC4ShakeDescription& description) override {
        calls.push_back({"shake", name, Num(description.amplitude)});
    }
    void AddLightDescription(const char* name, const cSC4LightDescription& description) override {
        calls.push_back({"light", name, Num(description.range)});
    }
    std::uint32_t AddComponentDescription(const char* name, cISC4ComponentDescription* description) override {
        calls.push_back({"component", name, std::to_string(description->Type())});
        return 0;
    }
};

// ---- description builders --------------------------------------------------------------------------------------
cSC4EffectDescription::cDescriptionRec Rec(const char* name, std::uint32_t kind, std::int32_t index) {
    cSC4EffectDescription::cDescriptionRec record;
    record.name = name;
    record.kind = kind;
    record.intValue = index;
    return record;
}

cSC4EffectDescription::cEventRec Ev(std::uint64_t type, const char* name, std::uint32_t flags) {
    cSC4EffectDescription::cEventRec event;
    event.type = type;
    event.name = name;
    event.flags = flags;
    return event;
}

void AddParticles(cSC4EffectsCollection& collection, const char* prefix, int count) {
    for (int i = 0; i < count; ++i) {
        cSC4ParticlesDescription description;
        description.flags = 0x10u + static_cast<std::uint64_t>(i);
        description.lifetimeMin = static_cast<float>(i);
        collection.AddParticleDescription((std::string(prefix) + std::to_string(i)).c_str(), description);
    }
}

void AddDecals(cSC4EffectsCollection& collection, const char* prefix, int count) {
    for (int i = 0; i < count; ++i) {
        cSC4DecalDescription description;
        description.textureId = 0x100u + static_cast<std::uint32_t>(i);
        description.life = static_cast<float>(i) + 0.5f;
        collection.AddDecalDescription((std::string(prefix) + std::to_string(i)).c_str(), description);
    }
}

void AddShakes(cSC4EffectsCollection& collection, const char* prefix, int count) {
    for (int i = 0; i < count; ++i) {
        cSC4ShakeDescription description;
        description.amplitude = static_cast<float>(i) + 0.25f;
        collection.AddShakeDescription((std::string(prefix) + std::to_string(i)).c_str(), description);
    }
}

void AddLights(cSC4EffectsCollection& collection, const char* prefix, int count) {
    for (int i = 0; i < count; ++i) {
        cSC4LightDescription description;
        description.range = static_cast<float>(i) + 2.0f;
        collection.AddLightDescription((std::string(prefix) + std::to_string(i)).c_str(), description);
    }
}

void AddComponents(cSC4EffectsCollection& collection, const char* prefix, std::uint32_t type, int count) {
    for (int i = 0; i < count; ++i) {
        FakeComponent* component = new FakeComponent(type);
        collection.AddComponentDescription((std::string(prefix) + std::to_string(i) + "_" + std::to_string(type)).c_str(),
                                           component);
    }
}

// ---- sections --------------------------------------------------------------------------------------------------
void EmptyCollection() {
    cSC4EffectsCollection collection;
    CHECK(sizeof(cSC4EffectsCollection) == 0x220);
    CHECK(collection.ComponentDescs().size() == 13);          // one bucket per component type 3..15
    CHECK(collection.Effects().empty());
    CHECK(collection.Particles().empty());
    CHECK(collection.DynamicParticles().empty());
    CHECK(collection.Decals().empty());
    CHECK(collection.Shakes().empty());
    CHECK(collection.Lights().empty());
    CHECK(collection.EffectBindings().empty());
    CHECK(collection.MessageTriggers().empty());
    CHECK(!collection.CameraParamsSet());
    CHECK(!collection.AddWithNames());

    cSC4ParticlesDescription particle;
    cSC4DynamicParticleDescription dynamic;
    cSC4DecalDescription decal;
    cSC4ShakeDescription shake;
    cSC4LightDescription light;
    cSC4EffectDescription effect;
    cISC4ComponentDescription* component = nullptr;
    CHECK(!collection.GetParticleDescription("none", &particle));
    CHECK(!collection.GetDynamicParticleDescription("none", &dynamic));
    CHECK(!collection.GetDecalDescription("none", &decal));
    CHECK(!collection.GetShakeDescription("none", &shake));
    CHECK(!collection.GetLightDescription("none", &light));
    CHECK(!collection.GetEffectDescription("none", &effect));
    CHECK(!collection.GetComponentDescription("none", 3, &component));
    CHECK(!collection.UpdateEffectPriority("none", 1, nullptr));

    collection.EndCollection();                               // an empty virtual: nothing changes
    CHECK(collection.ComponentDescs().size() == 13);
    CHECK(collection.Particles().empty());
}

void ParticleTable() {
    cSC4EffectsCollection collection;
    cSC4ParticlesDescription dust;
    dust.flags = 0x21;
    dust.lifetimeMin = 1.5f;
    dust.lifetimeMax = 2.5f;
    dust.name = "Dust Cloud";
    dust.sizeCurve = {1.0f, 2.0f, 3.0f};
    collection.AddParticleDescription("Dust", dust);
    CHECK(collection.Particles().size() == 1);

    cSC4ParticlesDescription out;
    CHECK(collection.GetParticleDescription("dUSt", &out));    // every lookup lowercases
    CHECK(out.flags == 0x21);
    CHECK(out.lifetimeMin == 1.5f && out.lifetimeMax == 2.5f);
    CHECK(out.name == "Dust Cloud");
    CHECK(Equals(out.sizeCurve, {1.0f, 2.0f, 3.0f}));
    CHECK(collection.GetParticleDescription("Dust", nullptr)); // name-only test
    CHECK(!collection.GetParticleDescription("smoke", &out));

    // The same name overwrites the stored copy in place (no second element, and the whole record is replaced).
    cSC4ParticlesDescription other;
    other.flags = 0x7;
    other.lifetimeMin = 9.0f;
    collection.AddParticleDescription("DUST", other);
    CHECK(collection.Particles().size() == 1);
    CHECK(collection.GetParticleDescription("dust", &out));
    CHECK(out.flags == 0x7 && out.lifetimeMin == 9.0f);
    CHECK(out.sizeCurve.empty());

    AddParticles(collection, "extra", 2);
    CHECK(collection.Particles().size() == 3);
    CHECK(collection.GetParticleDescription("EXTRA1", &out) && out.lifetimeMin == 1.0f);
}

void DynamicParticleTable() {
    cSC4EffectsCollection collection;
    cSC4DynamicParticleDescription wisp;
    wisp.type = 0x1234;
    wisp.name = "Wisp";
    wisp.values[0] = 3.5f;
    wisp.count = 2;
    wisp.curve = {0.0f, 1.0f};
    collection.AddDynamicParticleDescription("Wisp", wisp);
    CHECK(collection.DynamicParticles().size() == 1);

    cSC4DynamicParticleDescription out;
    CHECK(collection.GetDynamicParticleDescription("WISP", &out));
    CHECK(out.type == 0x1234 && out.name == "Wisp");
    CHECK(out.values[0] == 3.5f && out.count == 2);
    CHECK(Equals(out.curve, {0.0f, 1.0f}));
    CHECK(!collection.GetDynamicParticleDescription("none", &out));
    CHECK(collection.GetDynamicParticleDescription("wisp", nullptr));

    cSC4DynamicParticleDescription second;
    second.values[0] = 1.0f;
    collection.AddDynamicParticleDescription("wisp", second);
    CHECK(collection.DynamicParticles().size() == 1);
    CHECK(collection.GetDynamicParticleDescription("wisp", &out) && out.values[0] == 1.0f && out.curve.empty());
}

void DecalTable() {
    cSC4EffectsCollection collection;
    cSC4DecalDescription scorch;
    scorch.flags = 0x40;
    scorch.textureId = 0xabc;
    scorch.layer = 1;
    scorch.lifetime = 2;
    scorch.life = 0.75f;
    scorch.rotationCurve = {1.0f, 0.5f};
    scorch.sizeCurve = {0.25f};
    scorch.opacityCurve = {0.0f, 1.0f, 2.0f};
    scorch.colorCurve = {Vec3(1, 2, 3), Vec3(4, 5, 6)};
    scorch.aspectCurve = {7.0f};
    scorch.opacityVariance = 1.0f;
    scorch.sizeVariance = 2.0f;
    scorch.rotationVariance = 3.0f;
    scorch.mapping = 4.0f;
    scorch.offset.x = 5.0f;
    scorch.offset.y = 6.0f;
    collection.AddDecalDescription("Scorch", scorch);
    CHECK(collection.Decals().size() == 1);

    cSC4DecalDescription out;
    CHECK(collection.GetDecalDescription("SCORCH", &out));
    CHECK(out.flags == 0x40 && out.textureId == 0xabc);
    CHECK(out.layer == 1 && out.lifetime == 2 && out.life == 0.75f);
    CHECK(Equals(out.rotationCurve, {1.0f, 0.5f}));
    CHECK(Equals(out.sizeCurve, {0.25f}));
    CHECK(Equals(out.opacityCurve, {0.0f, 1.0f, 2.0f}));
    CHECK(out.colorCurve.size() == 2 && Same(out.colorCurve[1], Vec3(4, 5, 6)));
    CHECK(Equals(out.aspectCurve, {7.0f}));
    CHECK(out.opacityVariance == 1.0f && out.mapping == 4.0f);
    CHECK(out.offset.x == 5.0f && out.offset.y == 6.0f);
    CHECK(!collection.GetDecalDescription("none", &out));

    AddDecals(collection, "mark", 2);
    CHECK(collection.Decals().size() == 3);
    CHECK(collection.GetDecalDescription("mark1", &out) && out.life == 1.5f);
}

void ShakeAndLightTables() {
    cSC4EffectsCollection collection;
    cSC4ShakeDescription rumble;
    rumble.amplitude = 4.0f;
    rumble.frequency = 8.0f;
    rumble.curveA = {0.0f, 1.0f};
    rumble.curveB = {2.0f};
    rumble.falloff = 0.5f;
    rumble.flags = 3;
    collection.AddShakeDescription("Rumble", rumble);
    cSC4ShakeDescription outShake;
    CHECK(collection.GetShakeDescription("rUMBLE", &outShake));
    CHECK(outShake.amplitude == 4.0f && outShake.frequency == 8.0f);
    CHECK(Equals(outShake.curveA, {0.0f, 1.0f}));
    CHECK(Equals(outShake.curveB, {2.0f}));
    CHECK(outShake.falloff == 0.5f && outShake.flags == 3);
    CHECK(!collection.GetShakeDescription("none", &outShake));
    AddShakes(collection, "shake", 2);
    CHECK(collection.Shakes().size() == 3);
    CHECK(collection.GetShakeDescription("shake1", &outShake) && outShake.amplitude == 1.25f);

    cSC4LightDescription glow;
    glow.colors = {Vec3(1, 0, 0), Vec3(0, 1, 0)};
    glow.intensities = {0.5f, 1.0f};
    glow.range = 12.0f;
    collection.AddLightDescription("Glow", glow);
    cSC4LightDescription outLight;
    CHECK(collection.GetLightDescription("GLOW", &outLight));
    CHECK(outLight.colors.size() == 2 && Same(outLight.colors[0], Vec3(1, 0, 0)));
    CHECK(Equals(outLight.intensities, {0.5f, 1.0f}));
    CHECK(outLight.range == 12.0f);
    CHECK(!collection.GetLightDescription("none", &outLight));
    AddLights(collection, "light", 2);
    CHECK(collection.Lights().size() == 3);
    CHECK(collection.GetLightDescription("light1", &outLight) && outLight.range == 3.0f);
}

void EffectTable() {
    // The other tables are the index source the effect's records / events resolve against.
    cSC4EffectsCollection collection;
    AddParticles(collection, "spark", 1);        // particle 0
    AddDecals(collection, "scorch", 1);          // decal 0
    AddShakes(collection, "rumble", 1);          // shake 0
    AddLights(collection, "glow", 2);            // light 0 = glow0, light 1 = glow1
    FakeComponent* brush = new FakeComponent(3);
    collection.AddComponentDescription("Brush", brush);        // component bucket 0, index 0
    cSC4DynamicParticleDescription wisp;
    collection.AddDynamicParticleDescription("Wisp", wisp);    // dynamic particle 0

    cSC4EffectDescription bang;
    bang.id = 0x4242;
    bang.version = 1;
    bang.label = "BigBang";
    bang.values[0] = 1.0f;
    bang.values[1] = 2.0f;
    bang.values[2] = 3.0f;
    bang.records = {
        Rec("SPARK0", 0, -1),      // -> particles
        Rec("SCORCH0", 1, -1),     // -> decals
        Rec("Self", 2, -1),        // -> the "no index" kind, becomes 0
        Rec("BRUSH", 3, -1),       // -> component bucket 0
        Rec("wisp", 16, -1),       // -> dynamic particles
        Rec("Missing", 0, -1),     // unresolved: stays -1 but is lowercased
        Rec("Other", 99, -1),      // unknown kind: untouched
        Rec("Kept", 0, 7),         // already resolved: not even lowercased
    };
    bang.events = {
        Ev(1, "RUMBLE0", 0xffffffff),   // bit 0 -> shakes
        Ev(4, "glow1", 0),              // type & 0xc -> lights
        Ev(2, "Quiet", 30),             // neither: untouched
        Ev(1, "NoShake", 99),           // bit 0 set but the name is not in the shake table: left alone
        Ev(8, "NoLight", 77),           // type & 0xc but the name is not in the light table: left alone
    };
    collection.AddEffectDescription("BigBang", bang);
    CHECK(collection.Effects().size() == 1);
    CHECK(collection.EffectNameCount("bigbang") == 1);

    cSC4EffectDescription out;
    CHECK(collection.GetEffectDescription("BIGBANG", &out));
    CHECK(out.id == 0x4242 && out.version == 1);
    CHECK(out.label == "bigbang");                     // the stored label is lowercased
    CHECK(out.values[0] == 1.0f && out.values[2] == 3.0f);
    CHECK(out.records.size() == 8);
    CHECK(out.records[0].intValue == 0 && out.records[0].name == "spark0");
    CHECK(out.records[1].intValue == 0 && out.records[1].name == "scorch0");
    CHECK(out.records[2].intValue == 0 && out.records[2].name == "self");
    CHECK(out.records[3].intValue == 0 && out.records[3].name == "brush");
    CHECK(out.records[4].intValue == 0 && out.records[4].name == "wisp");
    CHECK(out.records[5].intValue == -1 && out.records[5].name == "missing");
    CHECK(out.records[6].intValue == -1 && out.records[6].name == "other");
    CHECK(out.records[7].intValue == 7 && out.records[7].name == "Kept");
    CHECK(out.events.size() == 5);
    CHECK(out.events[0].flags == 0 && out.events[0].name == "rumble0");
    CHECK(out.events[1].flags == 1 && out.events[1].name == "glow1");
    CHECK(out.events[2].flags == 30 && out.events[2].name == "quiet");
    CHECK(out.events[3].flags == 99 && out.events[3].name == "noshake");
    CHECK(out.events[4].flags == 77 && out.events[4].name == "nolight");
    CHECK(collection.GetEffectDescription("bigbang", nullptr));

    // A non-zero resolved index stays as it is; the incoming description is not modified.
    cSC4EffectDescription third;
    third.version = 4;
    third.records = {Rec("spark0", 0, 5)};
    collection.AddEffectDescription("Third", third);
    CHECK(collection.GetEffectDescription("third", &out) && out.records[0].intValue == 5);
    CHECK(bang.records[0].intValue == -1);              // the caller's record was not touched

    // The effects are keyed by (name, version): a second version appends, the same version overwrites in place.
    cSC4EffectDescription second;
    second.version = 2;
    second.label = "two";
    collection.AddEffectDescription("BigBang", second);
    CHECK(collection.Effects().size() == 3);
    CHECK(collection.EffectNameCount("bigbang") == 2);
    cSC4EffectDescription overwrite;
    overwrite.version = 2;
    overwrite.label = "two-b";
    collection.AddEffectDescription("bigbang", overwrite);
    CHECK(collection.Effects().size() == 3);
    CHECK(collection.EffectNameCount("bigbang") == 2);
    int versionTwo = 0;
    for (const auto& effect : collection.Effects())
        if (effect.version == 2) {
            ++versionTwo;
            CHECK(effect.label == "two-b");
        }
    CHECK(versionTwo == 1);
    // GetEffectDescription is a plain find: the name alone answers, whatever version the first entry carries.
    CHECK(collection.GetEffectDescription("bigbang", &out));
    CHECK(out.version == 1 || out.version == 2);
    CHECK(!collection.GetEffectDescription("absent", &out));
}

void ComponentTable() {
    FakeComponent::destroyed = 0;
    {
        cSC4EffectsCollection collection;
        FakeComponent* brush = new FakeComponent(3);
        FakeComponent* camera = new FakeComponent(8);
        FakeComponent* anonymous = new FakeComponent(3);
        CHECK(collection.AddComponentDescription("Brush", brush) == 0);
        CHECK(collection.AddComponentDescription("Camera", camera) == 0);
        CHECK(collection.AddComponentDescription(nullptr, anonymous) == 1);   // unnamed: the old element count
        CHECK(collection.ComponentDescs()[0].descriptions.size() == 2);       // type 3 -> bucket 0
        CHECK(collection.ComponentDescs()[5].descriptions.size() == 1);       // type 8 -> bucket 5
        CHECK(collection.ComponentDescs()[0].names.count("brush") == 1);

        cISC4ComponentDescription* out = nullptr;
        CHECK(collection.GetComponentDescription("BRUSH", 3, &out));
        CHECK(out == brush);                       // Get hands out the stored pointer with a reference added
        CHECK(FakeComponent::destroyed == 0);
        out->Release();
        CHECK(FakeComponent::destroyed == 0);
        CHECK(!collection.GetComponentDescription("brush", 8, &out));   // wrong bucket
        CHECK(!collection.GetComponentDescription("brush", 2, &out));   // outside 3..15: refused
        CHECK(!collection.GetComponentDescription("brush", 16, &out));
        CHECK(collection.GetComponentDescription("brush", 3, nullptr)); // name-only test

        // Replacing a name keeps its index and swaps the reference: the old component is released to zero.
        FakeComponent* replacement = new FakeComponent(3);
        CHECK(collection.AddComponentDescription("brush", replacement) == 0);
        CHECK(collection.ComponentDescs()[0].descriptions.size() == 2);
        CHECK(FakeComponent::destroyed == 1);
        CHECK(collection.GetComponentDescription("brush", 3, &out) && out == replacement);
        out->Release();

        CHECK(collection.AddComponentDescription("null", nullptr) == 0);   // the original dereferences it
        CHECK(collection.ComponentDescs()[0].descriptions.size() == 2);

        // A component type outside 3..15 is refused instead of indexing the table out of bounds.
        FakeComponent* stray = new FakeComponent(2);
        CHECK(collection.AddComponentDescription("stray", stray) == 0);
        CHECK(collection.ComponentDescs()[0].descriptions.size() == 2);
        delete stray;
        CHECK(FakeComponent::destroyed == 2);

        // Self-assignment: adding the same pointer again does not release it.
        CHECK(collection.AddComponentDescription("brush", replacement) == 0);
        CHECK(FakeComponent::destroyed == 2);
    }
    // The collection released its references (two type-3 + one type-8 + the replacement).
    CHECK(FakeComponent::destroyed == 5);
}

void CameraBindingsAndTriggers() {
    cSC4EffectsCollection collection;
    cSC4CameraParams params;
    params.keys = {1.0f, 2.0f, 3.0f};
    params.values[0] = 10.0f;
    params.values[1] = 11.0f;
    params.values[2] = 12.0f;
    params.values[3] = 13.0f;
    CHECK(!collection.CameraParamsSet());
    collection.SetCameraParams(params);
    CHECK(collection.CameraParamsSet());
    CHECK(Equals(collection.CameraParams().keys, {1.0f, 2.0f, 3.0f}));
    CHECK(collection.CameraParams().values[0] == 10.0f && collection.CameraParams().values[3] == 13.0f);

    collection.AddEffectBinding("MyEffect", 7, 9);
    CHECK(collection.EffectBindings().size() == 1);
    CHECK(collection.EffectBindings()[0].first == "MyEffect");   // bindings are stored as given, not lowercased
    CHECK(collection.EffectBindings()[0].second.id == 7);
    CHECK(collection.EffectBindings()[0].second.value == 9);

    cSC4MessageTriggerDescription trigger;
    trigger.message = 0x1234;
    trigger.name = "Trigger";
    collection.AddMessageTrigger(trigger);
    CHECK(collection.MessageTriggers().size() == 1);
    CHECK(collection.MessageTriggers()[0].message == 0x1234);
    CHECK(collection.MessageTriggers()[0].name == "Trigger");

    // BeginCollection drops the two vectors and the camera flag only: the parameters and the descriptions survive.
    cSC4ParticlesDescription particle;
    collection.AddParticleDescription("dust", particle);
    collection.BeginCollection();
    CHECK(collection.MessageTriggers().empty());
    CHECK(collection.EffectBindings().empty());
    CHECK(!collection.CameraParamsSet());
    CHECK(collection.CameraParams().keys.size() == 3);
    CHECK(collection.Particles().size() == 1);
}

void EffectPriority() {
    cSC4EffectsCollection collection;
    cSC4EffectDescription five;
    five.version = 5;
    five.label = "x";
    collection.AddEffectDescription("x", five);
    cSC4EffectDescription other;
    other.version = 5;
    other.label = "y";
    collection.AddEffectDescription("y", other);
    cSC4EffectDescription newer;
    newer.version = 7;
    newer.label = "y7";
    collection.AddEffectDescription("y", newer);
    CHECK(collection.Effects().size() == 3);
    CHECK(collection.EffectNameCount("y") == 2);

    // Success: the first entry of the name range gets the version and the alias points at it.
    CHECK(collection.UpdateEffectPriority("X", 7, "Alias"));
    CHECK(collection.Effects()[0].version == 7);
    CHECK(collection.EffectNameCount("alias") == 1);
    cSC4EffectDescription out;
    CHECK(collection.GetEffectDescription("alias", &out));
    CHECK(out.version == 7 && out.label == "x");

    // An effect added through the alias resolves to the aliased entry instead of appending a new one.
    cSC4EffectDescription aliased;
    aliased.version = 7;
    aliased.label = "aliased";
    collection.AddEffectDescription("ALIAS", aliased);
    CHECK(collection.Effects().size() == 3);
    CHECK(collection.EffectNameCount("alias") == 1);
    CHECK(collection.Effects()[0].label == "aliased");

    // Failure: the alias already names a different entry for the same version. The version write still sticks.
    cSC4EffectsCollection refused;
    cSC4EffectDescription first;
    first.version = 5;
    refused.AddEffectDescription("first", first);
    cSC4EffectDescription second;
    second.version = 5;
    refused.AddEffectDescription("second", second);
    cSC4EffectDescription secondNewer;
    secondNewer.version = 7;
    refused.AddEffectDescription("second", secondNewer);
    CHECK(!refused.UpdateEffectPriority("first", 7, "second"));
    CHECK(refused.Effects()[0].version == 7);          // the version was written before the alias check failed
    CHECK(refused.EffectNameCount("second") == 2);     // and no alias entry was added

    // An alias name that is free is registered even when it points at the same description twice.
    CHECK(refused.UpdateEffectPriority("first", 7, "third"));
    CHECK(refused.EffectNameCount("third") == 1);

    // Without an alias the same name is looked up again: it finds the entry just written, so nothing is added.
    const std::size_t firstNames = refused.EffectNameCount("first");
    CHECK(refused.UpdateEffectPriority("first", 7, nullptr));
    CHECK(refused.EffectNameCount("first") == firstNames);
    CHECK(!refused.UpdateEffectPriority("absent", 1, nullptr));
}

void SwapMerge() {
    // A destination without effect descriptions takes everything by swap; the camera parameters and the flag are the
    // two members that are not swapped.
    cSC4EffectsCollection source;
    cSC4EffectsCollection destination;
    cSC4CameraParams params;
    params.keys = {4.0f, 5.0f};
    params.values[0] = 40.0f;
    params.values[1] = 41.0f;
    params.values[2] = 42.0f;
    params.values[3] = 43.0f;
    source.SetCameraParams(params);
    AddParticles(source, "dust", 2);
    AddDecals(source, "scorch", 1);
    source.AddEffectBinding("bind", 1, 2);
    source.AddMessageTrigger(cSC4MessageTriggerDescription{0x99, "Trigger"});
    CHECK(source.Particles().size() == 2);

    source.AddToCollection(&destination);
    CHECK(source.Particles().empty());                 // everything moved over
    CHECK(source.Decals().empty());
    CHECK(source.EffectBindings().empty());
    CHECK(source.MessageTriggers().empty());
    CHECK(destination.Particles().size() == 2);
    CHECK(destination.Decals().size() == 1);
    CHECK(destination.EffectBindings().size() == 1);
    CHECK(destination.MessageTriggers().size() == 1);
    CHECK(destination.GetParticleDescription("dust0", nullptr));
    // The camera parameters were copied, but the destination's flag stays false while the source keeps both.
    CHECK(source.CameraParamsSet());
    CHECK(!destination.CameraParamsSet());
    CHECK(Equals(destination.CameraParams().keys, {4.0f, 5.0f}));
    CHECK(destination.CameraParams().values[3] == 43.0f);
    CHECK(Equals(source.CameraParams().keys, {4.0f, 5.0f}));
}

void MergeWithNames() {
    cSC4EffectsCollection source;
    cSC4ParticlesDescription particle;
    particle.flags = 0x55;
    particle.lifetimeMin = 1.5f;
    source.AddParticleDescription("Dust", particle);
    cSC4DynamicParticleDescription dynamic;
    dynamic.values[0] = 2.5f;
    source.AddDynamicParticleDescription("Wisp", dynamic);
    cSC4DecalDescription decal;
    decal.life = 0.75f;
    source.AddDecalDescription("Scorch", decal);
    cSC4ShakeDescription shake;
    shake.amplitude = 4.5f;
    source.AddShakeDescription("Rumble", shake);
    cSC4LightDescription light;
    light.range = 12.0f;
    source.AddLightDescription("Glow", light);
    FakeComponent* brush = new FakeComponent(3);
    source.AddComponentDescription("Brush", brush);
    cSC4EffectDescription effect;
    effect.version = 1;
    effect.label = "Bang";
    source.AddEffectDescription("Bang", effect);

    RecordingSink sink;
    source.AddToCollectionWithNames(&sink);
    CHECK(sink.calls.size() == 7);
    CHECK(sink.calls[0].kind == "particle" && sink.calls[0].name == "dust" && sink.calls[0].value == "1.500");
    CHECK(sink.calls[1].kind == "dynamic" && sink.calls[1].name == "wisp" && sink.calls[1].value == "2.500");
    CHECK(sink.calls[2].kind == "decal" && sink.calls[2].name == "scorch" && sink.calls[2].value == "0.750");
    CHECK(sink.calls[3].kind == "shake" && sink.calls[3].name == "rumble" && sink.calls[3].value == "4.500");
    CHECK(sink.calls[4].kind == "light" && sink.calls[4].name == "glow" && sink.calls[4].value == "12.000");
    CHECK(sink.calls[5].kind == "component" && sink.calls[5].name == "brush" && sink.calls[5].value == "3");
    CHECK(sink.calls[6].kind == "effect" && sink.calls[6].name == "bang" && sink.calls[6].value == "bang");

    // With a real collection as the target the forwarded descriptions land in its own tables.
    cSC4EffectsCollection target;
    source.AddToCollectionWithNames(&target);
    cSC4ParticlesDescription outParticle;
    CHECK(target.GetParticleDescription("dust", &outParticle) && outParticle.flags == 0x55);
    cSC4DecalDescription outDecal;
    CHECK(target.GetDecalDescription("scorch", &outDecal) && outDecal.life == 0.75f);
    cISC4ComponentDescription* outComponent = nullptr;
    CHECK(target.GetComponentDescription("brush", 3, &outComponent));
    outComponent->Release();
    cSC4EffectDescription outEffect;
    CHECK(target.GetEffectDescription("bang", &outEffect) && outEffect.label == "bang");
}

void MergeDispatch() {
    // The +0x200 mode picks the named merge; without it the indices are renumbered.
    cSC4EffectsCollection destination;
    cSC4EffectDescription seed;
    seed.version = 1;
    seed.label = "seed";
    destination.AddEffectDescription("seed", seed);     // a non-empty effect table: the swap branch is not taken

    cSC4ParticlesDescription particle;
    cSC4EffectsCollection byName;
    byName.AddParticleDescription("dust", particle);
    byName.SetAddWithNames(true);
    CHECK(byName.AddWithNames());
    byName.AddToCollection(&destination);
    CHECK(destination.GetParticleDescription("dust", nullptr));      // the named merge registers the name

    cSC4EffectsCollection byIndex;
    byIndex.AddParticleDescription("smoke", particle);
    CHECK(!byIndex.AddWithNames());
    byIndex.AddToCollection(&destination);
    CHECK(destination.Particles().size() == 2);                       // appended to the table...
    CHECK(!destination.GetParticleDescription("smoke", nullptr));     // ...but the name map was not merged
    CHECK(destination.GetParticleDescription("dust", nullptr));
}

void RenumberingMerge() {
    cSC4EffectsCollection destination;
    AddParticles(destination, "destParticle", 3);
    AddDecals(destination, "destDecal", 2);
    AddShakes(destination, "destShake", 4);
    AddLights(destination, "destLight", 1);
    AddComponents(destination, "destComp", 3, 1);       // component bucket 0
    AddComponents(destination, "destComp", 8, 1);       // component bucket 5
    cSC4EffectDescription destShared;
    destShared.version = 3;
    destShared.label = "dest";
    destShared.records = {Rec("spark0", 0, 0)};
    destination.AddEffectDescription("shared", destShared);
    destination.AddMessageTrigger(cSC4MessageTriggerDescription{0x11, "DestTrigger"});
    destination.AddEffectBinding("destBind", 1, 1);

    cSC4EffectsCollection source;
    AddParticles(source, "srcParticle", 2);
    AddDecals(source, "srcDecal", 1);
    AddShakes(source, "srcShake", 2);
    AddLights(source, "srcLight", 3);
    AddComponents(source, "srcComp", 3, 2);
    AddComponents(source, "srcComp", 8, 0);
    cSC4EffectDescription shared;
    shared.version = 3;
    shared.label = "source";
    shared.records = {
        Rec("spark", 0, 0),      // + 3 particles
        Rec("scorch", 1, 5),     // + 2 decals
        Rec("nested", 2, 0),     // + 1 effect
        Rec("brush", 3, 0),      // + 1 component of kind 3
        Rec("camera", 8, 3),     // + 1 component of kind 8
        Rec("wisp", 16, 4),      // kind 16 is not renumbered
        Rec("other", 99, 6),     // unknown kinds are not renumbered
    };
    shared.events = {Ev(1, "rumble", 10), Ev(4, "glow", 20), Ev(2, "quiet", 30)};
    source.AddEffectDescription("shared", shared);
    cSC4EffectDescription only;
    only.version = 1;
    only.label = "only";
    source.AddEffectDescription("srconly", only);
    source.AddMessageTrigger(cSC4MessageTriggerDescription{0x22, "SrcTrigger"});
    source.AddEffectBinding("srcBind", 2, 2);

    source.AddToCollectionWithRenumbering(&destination);

    // The value tables are appended to the destination.
    CHECK(destination.Particles().size() == 5);
    CHECK(source.Particles().size() == 2);                     // the source keeps its own table
    CHECK(destination.Decals().size() == 3);
    CHECK(destination.Shakes().size() == 6);
    CHECK(destination.Lights().size() == 4);
    CHECK(destination.Effects().size() == 3);
    CHECK(destination.MessageTriggers().size() == 2);
    CHECK(destination.EffectBindings().size() == 2);
    CHECK(destination.ComponentDescs()[0].descriptions.size() == 3);
    CHECK(destination.ComponentDescs()[5].descriptions.size() == 1);   // the source adds none of kind 8
    CHECK(source.ComponentDescs()[0].descriptions.size() == 2);

    // Only the effect name map is merged: the destination's own names keep working, the merged ones do not resolve.
    CHECK(destination.GetParticleDescription("destParticle1", nullptr));
    CHECK(!destination.GetParticleDescription("srcParticle0", nullptr));
    CHECK(destination.GetDecalDescription("destDecal1", nullptr));
    CHECK(!destination.GetDecalDescription("srcDecal0", nullptr));
    CHECK(destination.GetShakeDescription("destShake3", nullptr));
    CHECK(!destination.GetShakeDescription("srcShake1", nullptr));
    CHECK(!destination.GetComponentDescription("srcComp0_3", 3, nullptr));
    cISC4ComponentDescription* component = nullptr;
    CHECK(destination.GetComponentDescription("destComp0_3", 3, &component));
    component->Release();
    CHECK(destination.EffectNameCount("shared") == 1);         // the same (name, version) destination entry was dropped
    CHECK(destination.EffectNameCount("srconly") == 1);
    cSC4EffectDescription merged;
    CHECK(destination.GetEffectDescription("shared", &merged));
    CHECK(merged.label == "source");                           // the merged entry replaced the destination's
    CHECK(merged.version == 3);

    // The merged records were renumbered by the destination's table sizes; kind 16 and unknown kinds keep their index.
    CHECK(merged.records.size() == 7);
    CHECK(merged.records[0].intValue == 0 + 3);
    CHECK(merged.records[1].intValue == 5 + 2);
    CHECK(merged.records[2].intValue == 0 + 1);
    CHECK(merged.records[3].intValue == 0 + 1);
    CHECK(merged.records[4].intValue == 3 + 1);
    CHECK(merged.records[5].intValue == 4);
    CHECK(merged.records[6].intValue == 6);
    CHECK(merged.events.size() == 3);
    CHECK(merged.events[0].flags == 10 + 4);
    CHECK(merged.events[1].flags == 20 + 1);
    CHECK(merged.events[2].flags == 30);

    // The source's own effect records were renumbered in place: merging the same collection twice shifts it twice.
    CHECK(source.Effects().size() == 2);
    CHECK(source.Effects()[0].records[0].intValue == 3);
    CHECK(source.Effects()[0].events[0].flags == 14);
    // The destination's own effect kept its records: only the merged collection is renumbered.
    CHECK(destination.Effects()[0].label == "dest");
    CHECK(destination.Effects()[0].records[0].intValue == 0);

    // The destination's name maps keep pointing at the entries they had before the merge appended the new ones.
    cSC4ParticlesDescription outParticle;
    CHECK(destination.GetParticleDescription("destParticle2", &outParticle) && outParticle.lifetimeMin == 2.0f);
}

}  // namespace

int main() {
    EmptyCollection();
    ParticleTable();
    DynamicParticleTable();
    DecalTable();
    ShakeAndLightTables();
    EffectTable();
    ComponentTable();
    CameraBindingsAndTriggers();
    EffectPriority();
    SwapMerge();
    MergeWithNames();
    MergeDispatch();
    RenumberingMerge();

    if (failures == 0) std::printf("sc4_effectscollection: all checks passed\n");
    else std::printf("sc4_effectscollection: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

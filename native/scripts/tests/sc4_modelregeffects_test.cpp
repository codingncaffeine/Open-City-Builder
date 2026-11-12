// cSC4ModelRegEffects: the effects made from a model's "fx_" registration points, the ones added by name, start / stop /
// toggle / recreate by effect name, and where an effect is put each update (the model's transform times the point's
// transform of the current frame), against a fake model and a fake effects manager.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sc4/ModelRegEffects.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

struct Model final : IAutomatonModel {
    int refs = 1, frame = 0;
    cS3DTransform transform;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void GetBounds(float* lo, float* hi) override { for (int i = 0; i < 3; ++i) { lo[i] = -1; hi[i] = 1; } }
    void GetTransform(cS3DTransform& out) override { out = transform; }
    void SetTransform(const cS3DTransform& in) override { transform = in; }
    void SetOpacity(float, bool) override {}
    void UpdateAnimation(float) override {}
    int GetFrame() const override { return frame; }
    int GetFrameCount() const override { return 2; }
    void SetZoomAndRotation(int, int) override {}
    void SetPlayMode(s3d::PlayMode) override {}
};

struct Effect final : IRegEffect {
    explicit Effect(std::string n) : name(std::move(n)) { ++alive; }
    ~Effect() override { --alive; }
    void AddRef() override { ++refs; }
    void Release() override { if (--refs == 0) delete this; }
    void Start(int transition) override { started = true; ++starts; lastStart = transition; }
    void Stop(int transition) override { started = false; ++stops; lastStop = transition; }
    bool IsStarted() override { return started; }
    void SetSourceTransform(const cS3DTransform& t) override { place = t; ++placed; }
    void SetVectorParams(std::uint32_t type, const cS3DVector3* values, int count) override { if (type == kEffectParamVelocity && count == 1) velocity = values[0]; }
    bool CheckEffectFlag(std::uint32_t flag) override { return flag == kEffectFlagStartsOff && startsOff; }
    std::string name;
    int refs = 1, starts = 0, stops = 0, lastStart = -1, lastStop = -1, placed = 0;
    bool started = false, startsOff = false;
    cS3DTransform place;
    cS3DVector3 velocity{0, 0, 0};
    static inline int alive = 0;
};

struct Host final : IRegEffectsHost {
    const RegPointMap* RegPoints(IAutomatonModel* model) override { return model == withPoints ? &points : nullptr; }
    IRegEffect* CreateEffect(const char* name) override {
        asked.push_back(name);
        if (unknown == name) return nullptr;
        auto* effect = new Effect(name);
        effect->startsOff = startsOff == name;
        made.push_back(effect);
        return effect;
    }
    Effect* Last(const std::string& name) const {
        for (auto it = made.rbegin(); it != made.rend(); ++it) if ((*it)->name == name) return *it;
        return nullptr;
    }
    IAutomatonModel* withPoints = nullptr;
    RegPointMap points;
    std::vector<std::string> asked;
    std::vector<Effect*> made;     // borrowed: the collection holds the references
    std::string unknown, startsOff;
};

RegPointTransform Point(float x, float y, float z, float angle = 0.0f) { return RegPointTransform{{x, y, z}, {0.0f, 1.0f, 0.0f}, angle}; }
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Host host;
    Model model;
    host.withPoints = &model;
    host.points["fx_smoke"] = {Point(1, 2, 3), Point(4, 5, 6)};
    host.points["fx_00_rotor"] = {Point(0, 10, 0)};
    host.points["fx_12_"] = {Point(0, 0, 0)};                    // digits and the underscore, nothing after: an empty name
    host.points["fx_7lights"] = {Point(0, 0, 0)};                // a digit without an underscore keeps the whole rest
    host.points["regpoint01"] = {Point(9, 9, 9)};                // not an effect point
    host.points["hose"] = {Point(0, 1, 0), Point(0, 2, 0)};

    // ---- without a host or a model nothing is made
    {
        SetRegEffectsHost(nullptr);
        cSC4ModelRegEffects effects;
        CHECK(effects.Init());
        effects.SetModel(&model);
        CHECK(model.refs == 2 && effects.GetModel() == &model);
        CHECK(effects.Init() && effects.Effects().empty());
        CHECK(!effects.AddRegEffect(cRZString("a"), cRZString("a")));
        effects.Update(cS3DVector3{0, 0, 0}, cS3DVector3{1, 0, 0}, 1.0f);
        effects.UpdatePlacement();
        effects.Shutdown();
        CHECK(model.refs == 1 && effects.GetModel() == nullptr);
    }
    SetRegEffectsHost(&host);
    CHECK(RegEffectsHost() == &host);

    // ---- Init: one effect per "fx_" point, named by the rest of the point's name after a leading "<digits>_"
    {
        cSC4ModelRegEffects effects;
        effects.SetModel(&model);
        CHECK(effects.Init());
        CHECK(effects.Effects().size() == 3);
        CHECK(effects.Effects().count("fx_smoke") && effects.Effects().at("fx_smoke").effectName == "smoke");
        CHECK(effects.Effects().count("fx_00_rotor") && effects.Effects().at("fx_00_rotor").effectName == "rotor");
        CHECK(effects.Effects().count("fx_7lights") && effects.Effects().at("fx_7lights").effectName == "7lights");
        // "fx_12_" asks for the effect with the empty name, which this manager does not have: no entry
        bool askedEmpty = false;
        for (const std::string& name : host.asked) askedEmpty = askedEmpty || name.empty();
        CHECK(askedEmpty && effects.Effects().count("fx_12_") == 0);
        CHECK(effects.Effects().at("fx_smoke").frames == &host.points["fx_smoke"]);
        CHECK(Effect::alive == 3);
        Effect* smoke = host.Last("smoke");
        Effect* rotor = host.Last("rotor");
        CHECK(smoke && rotor && !smoke->started && smoke->starts == 0);   // made, not started
        CHECK(effects.FindRegEffect(cRZString("fx_smoke")) == static_cast<IRegEffect*>(smoke));
        CHECK(effects.FindRegEffect(cRZString("smoke")) == nullptr);       // found by the point, not by the effect

        // nothing is placed until it runs
        model.transform.translation[0] = 100;
        model.transform.translation[1] = 200;
        model.transform.translation[2] = 300;
        model.transform.flags |= 1;
        effects.UpdatePlacement();
        CHECK(smoke->placed == 0);
        effects.StartAllEffects(1);
        CHECK(smoke->started && smoke->lastStart == 1 && rotor->started);
        effects.StartAllEffects(0);
        CHECK(smoke->starts == 1);                                         // a running effect is not started again
        // the place: the model's transform and the point's translation of the current frame
        effects.UpdatePlacement();
        CHECK(smoke->placed == 1 && Near(smoke->place.translation[0], 101) && Near(smoke->place.translation[1], 202) && Near(smoke->place.translation[2], 303));
        CHECK(Near(rotor->place.translation[1], 210));
        model.frame = 1;
        effects.Update(cS3DVector3{0, 0, 0}, cS3DVector3{2, 0, -4}, 0.5f);
        CHECK(Near(smoke->place.translation[0], 104) && Near(smoke->place.translation[1], 205) && Near(smoke->place.translation[2], 306));
        CHECK(Near(smoke->velocity.x, 1.0f) && Near(smoke->velocity.z, -2.0f));   // the direction times the time
        // a frame past the point's table: the model's own place
        CHECK(Near(rotor->place.translation[0], 100) && Near(rotor->place.translation[1], 200));
        // a turned model turns the point's offset with it (a quarter turn about y puts +x on the z axis, either way round)
        cS3DTransform turned = model.transform;
        const float quarter[3][3] = {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}};
        std::memcpy(turned.rotation, quarter, sizeof(quarter));
        turned.flags |= 4;
        model.transform = turned;
        model.frame = 0;
        effects.UpdatePlacement();
        CHECK(Near(smoke->place.translation[1], 202));
        CHECK(Near(std::fabs(smoke->place.translation[0] - 100), 3) && Near(std::fabs(smoke->place.translation[2] - 300), 1));

        // by name, any case
        effects.StopNamed(cRZString("SMOKE"), 1);
        CHECK(!smoke->started && smoke->lastStop == 1 && rotor->started);
        effects.StopNamed(cRZString("smoke"), 0);
        CHECK(smoke->stops == 1);                                          // a stopped effect is not stopped again
        CHECK(effects.StartNamedEffects(cRZString("Smoke"), 0, false) && smoke->started && smoke->lastStart == 0);
        CHECK(effects.StartNamedEffects(cRZString("smoke"), 0, false) && smoke->starts == 3);   // StartNamed starts a running one again
        effects.ToggleNamed(cRZString("rotor"), 1, false);
        CHECK(!rotor->started);
        effects.ToggleNamed(cRZString("rotor"), 1, false);
        CHECK(rotor->started);
        // a name nothing carries: nothing without `create`, a new effect on the point of that name with it
        const std::size_t asked = host.asked.size();
        CHECK(!effects.StartNamedEffects(cRZString("hose"), 1, false) && host.asked.size() == asked);
        CHECK(effects.StartNamedEffects(cRZString("hose"), 1, true));
        Effect* hose = host.Last("hose");
        CHECK(hose && hose->started && effects.Effects().at("hose").frames == &host.points["hose"]);
        effects.ToggleNamed(cRZString("siren"), 1, false);
        CHECK(host.Last("siren") == nullptr);
        effects.ToggleNamed(cRZString("siren"), 1, true);
        CHECK(host.Last("siren") && host.Last("siren")->started && effects.Effects().at("siren").frames == nullptr);
        // an effect the manager does not know leaves no entry
        host.unknown = "ghost";
        CHECK(!effects.StartNamedEffects(cRZString("ghost"), 1, true) && effects.Effects().count("ghost") == 0);
        CHECK(!effects.AddRegEffect(cRZString("fx_smoke"), cRZString("ghost")));   // and takes the point's old effect with it
        CHECK(effects.Effects().count("fx_smoke") == 0);
        host.unknown.clear();

        // recreate: a new object under the same point, started
        const int alive = Effect::alive;
        effects.RecreateNamed(cRZString("ROTOR"), 0, false);
        Effect* rotor2 = host.Last("ROTOR");
        CHECK(rotor2 && rotor2 != rotor && rotor2->started && Effect::alive == alive);
        CHECK(effects.FindRegEffect(cRZString("fx_00_rotor")) == static_cast<IRegEffect*>(rotor2));
        CHECK(effects.Effects().at("fx_00_rotor").effectName == "rotor");   // the entry keeps its name
        host.unknown = "rotor";
        effects.RecreateNamed(cRZString("rotor"), 0, false);
        CHECK(effects.FindRegEffect(cRZString("fx_00_rotor")) == nullptr && effects.Effects().count("fx_00_rotor") == 1);
        effects.UpdatePlacement();                                           // an empty point is skipped
        effects.StartAllEffects(1);
        effects.StopAllEffects(1);
        host.unknown.clear();

        // AddRegEffect as the automaton does it: added and started; a second effect on a point replaces the first
        CHECK(effects.AddRegEffect(cRZString("Riot1"), cRZString("Riot1")));
        Effect* riot = host.Last("Riot1");
        CHECK(riot && riot->started && riot->lastStart == 1 && riot->refs == 1);
        riot->AddRef();
        CHECK(effects.AddRegEffect(cRZString("Riot1"), cRZString("Riot2")));
        CHECK(!riot->started && riot->lastStop == 1 && riot->refs == 1);
        riot->Release();
        CHECK(effects.Effects().at("Riot1").effectName == "Riot2");
        // a key's effect is added, not started
        CHECK(effects.AddKeyEffect(cRZString("fx_horn"), cRZString("horn")));
        CHECK(host.Last("horn") && !host.Last("horn")->started);
        // remove: stopped with a transition and let go
        Effect* riot2 = host.Last("Riot2");
        riot2->AddRef();
        effects.RemoveRegEffect(cRZString("Riot1"));
        CHECK(!riot2->started && riot2->lastStop == 1 && riot2->refs == 1 && effects.Effects().count("Riot1") == 0);
        riot2->Release();
        effects.RemoveRegEffect(cRZString("Riot1"));                         // nothing there: nothing happens

        // another model drops everything at once
        Model other;
        effects.SetModel(&other);
        CHECK(effects.Effects().empty() && Effect::alive == 0 && model.refs == 1 && other.refs == 2);
        effects.SetModel(&other);                                            // the same model: nothing
        CHECK(other.refs == 2);
        CHECK(effects.Init() && effects.Effects().empty());                  // a model without points
        effects.Shutdown();
        CHECK(other.refs == 1);
    }

    // ---- the automaton's start: effects that start off stay off, a key's effect runs only while its key is held
    {
        host.made.clear();
        host.startsOff = "rotor";
        cSC4ModelRegEffects effects;
        effects.SetModel(&model);
        effects.Init();
        Effect* smoke = host.Last("smoke");
        Effect* rotor = host.Last("rotor");
        Effect* lights = host.Last("7lights");
        effects.StartAllEffectsByKeys({});
        CHECK(smoke->started && smoke->lastStart == 1 && !rotor->started && rotor->lastStop == 0 && lights->started);
        effects.StartAllEffectsByKeys({{"smoke", false}, {"rotor", true}});
        CHECK(!smoke->started && smoke->lastStop == 0 && rotor->started && lights->started && lights->starts == 1);
        effects.StartAll();                                                  // the plain walk starts what is not running
        CHECK(smoke->started);
        effects.StopAll(true);
        CHECK(!smoke->started && smoke->lastStop == 1);
        effects.StartAll();
        effects.StopAll(false);
        CHECK(smoke->lastStop == 0);
    }
    CHECK(Effect::alive == 0 && model.refs == 1);                            // the destructor let go of the effects and the model

    SetRegEffectsHost(nullptr);
    std::printf(failures ? "%d FAILURES\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}

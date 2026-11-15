// The effect components that act on the city, against fake city services: cSC4ScrubberEffect and its occupant filter
// (demolish / burn / contaminate / extinguish / message / effect-map blob as the effect enters new cells), cSC4AttractorEffect
// (an anchor occupant with automata controllers that follows the effect) and cSC4SequenceEffect (effects played in turn).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sc4/AttractorEffect.h"
#include "sc4/ScrubberEffect.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

cS3DTransform At(float x, float y, float z) {
    cS3DTransform t;
    t.flags = 1;
    t.changeCounter = 1;
    t.translation[0] = x; t.translation[1] = y; t.translation[2] = z;
    return t;
}
const cS3DTransform kIdentity;

// ---- scrubber ---------------------------------------------------------------------------------------------------------------
struct City final : IScrubberHost {
    struct Demolish { std::int32_t rect[4]; std::uint32_t flags; bool option; std::uint32_t filterFlags; float minSize, maxSize; };
    struct Blob { std::uint32_t map; std::int32_t rect[4]; float value; std::uint32_t spread; };
    struct Message { std::uint32_t type; std::uintptr_t data; std::int32_t x, z; };
    bool simulator = true;
    int cellsX = 256, cellsZ = 256;
    std::vector<Demolish> demolished;
    std::vector<Blob> blobs;
    std::vector<Message> messages;
    std::vector<std::string> log;
    bool HasSimulator() override { return simulator; }
    int CityCellCountX() override { return cellsX; }
    int CityCellCountZ() override { return cellsZ; }
    void DemolishRegion(const std::int32_t rect[4], std::uint32_t flags, bool option, std::uint32_t filterFlags, float minSize, float maxSize) override {
        demolished.push_back(Demolish{{rect[0], rect[1], rect[2], rect[3]}, flags, option, filterFlags, minSize, maxSize});
        log.push_back("demolish");
    }
    void Burn(std::int32_t x, std::int32_t z, std::uint32_t radius) override { log.push_back("burn " + std::to_string(x) + "," + std::to_string(z) + " " + std::to_string(radius)); }
    void Extinguish(std::int32_t x, std::int32_t z, std::int32_t amount) override { log.push_back("extinguish " + std::to_string(x) + "," + std::to_string(z) + " " + std::to_string(amount)); }
    void SpillToxic(std::int32_t x, std::int32_t z) override { log.push_back("toxic " + std::to_string(x) + "," + std::to_string(z)); }
    void PostScrubMessage(std::uint32_t type, std::uintptr_t data, std::int32_t x, std::int32_t z) override { messages.push_back(Message{type, data, x, z}); log.push_back("message"); }
    void AddRectToEffectMap(std::uint32_t map, const std::int32_t rect[4], float value, std::uint32_t spread) override {
        blobs.push_back(Blob{map, {rect[0], rect[1], rect[2], rect[3]}, value, spread});
    }
    void SimulatorPause(bool pause) override { log.push_back(pause ? "pause" : "resume"); }
    void SimulatorHiddenPause(bool pause) override { log.push_back(pause ? "hidden pause" : "hidden resume"); }
    void ClockHiddenPause(bool pause) override { log.push_back(pause ? "clock pause" : "clock resume"); }
};

bool Rect(const std::int32_t* r, int a, int b, int c, int d) { return r[0] == a && r[1] == b && r[2] == c && r[3] == d; }

void TestScrubberFilter() {
    ScrubberOccupant building;
    building.type = 0x278128a0;
    building.boxMax[0] = 32.0f; building.boxMax[1] = 20.0f; building.boxMax[2] = 48.0f;
    CHECK(ScrubberFilterIncludes(0, 0.0f, 0.0f, building));
    ScrubberOccupant sign = building;
    sign.signpost = true;
    CHECK(!ScrubberFilterIncludes(0, 0.0f, 0.0f, sign));
    ScrubberOccupant decal = building;
    decal.textDecal = true;
    CHECK(!ScrubberFilterIncludes(0, 0.0f, 0.0f, decal));
    ScrubberOccupant road = building;
    road.network = true;
    CHECK(ScrubberFilterIncludes(0, 0.0f, 0.0f, road));
    CHECK(ScrubberFilterIncludes(kScrubberLeaveFlora, 0.0f, 0.0f, road));
    CHECK(!ScrubberFilterIncludes(kScrubberLeaveNetworks, 0.0f, 0.0f, road));
    ScrubberOccupant tree = building;
    tree.type = kScrubberOccupantTypeFlora;
    CHECK(ScrubberFilterIncludes(kScrubberLeaveNetworks, 0.0f, 0.0f, tree));
    CHECK(!ScrubberFilterIncludes(kScrubberLeaveFlora, 0.0f, 0.0f, tree));
    // sizes: the largest side reaching the upper limit spares it, the smallest side within the lower limit spares it
    CHECK(ScrubberFilterIncludes(0, 0.0f, 48.5f, building));
    CHECK(!ScrubberFilterIncludes(0, 0.0f, 48.0f, building));
    CHECK(ScrubberFilterIncludes(0, 19.5f, 0.0f, building));
    CHECK(!ScrubberFilterIncludes(0, 20.0f, 0.0f, building));
    CHECK(!ScrubberFilterIncludes(0, 25.0f, 100.0f, building));
    CHECK(ScrubberFilterIncludes(0, 10.0f, 100.0f, building));
}

cSC4ScrubberEffect* NewScrubber(cSC4ScrubberDescription* description) {
    auto* effect = new cSC4ScrubberEffect(description);
    effect->AddRef();
    return effect;
}

void TestScrubberDemolish() {
    City city;
    SetScrubberHost(&city);
    auto* description = new cSC4ScrubberDescription;
    description->AddRef();
    description->u18 = kScrubberLeaveNetworks | kScrubberLeaveFlora;
    description->u24 = 1;
    description->u28 = 0x1302;
    description->f2c = 1.5f;
    description->f30 = 90.0f;
    cSC4ScrubberEffect* effect = NewScrubber(description);

    city.simulator = false;                                   // nothing starts without a simulator
    effect->Start(0);
    CHECK(!effect->IsStarted());
    city.simulator = true;
    effect->Start(0);
    CHECK(effect->IsStarted() && city.log.empty());

    effect->SetTransforms(kIdentity, At(529.0f, 270.0f, 1290.0f));
    CHECK(effect->CellX() == 33 && effect->CellZ() == 80);
    effect->ApplyEffect(0.1f);
    CHECK(city.demolished.size() == 1);
    if (city.demolished.size() == 1) {
        const City::Demolish& d = city.demolished[0];
        CHECK(Rect(d.rect, 32, 79, 34, 81));
        CHECK(d.flags == 0x1302 && !d.option && d.filterFlags == 3 && d.minSize == 1.5f && d.maxSize == 90.0f);
    }
    CHECK(effect->TimesApplied() == 1);
    effect->ApplyEffect(0.1f);                                // still in the cell: nothing more
    effect->SetTransforms(kIdentity, At(543.9f, 270.0f, 1295.0f));
    effect->ApplyEffect(0.1f);
    CHECK(city.demolished.size() == 1);
    effect->SetTransforms(kIdentity, At(544.0f, 270.0f, 1295.0f));   // the next cell
    effect->ApplyEffect(0.1f);
    CHECK(city.demolished.size() == 2 && Rect(city.demolished.back().rect, 33, 79, 35, 81));
    CHECK(effect->TimesApplied() == 2);

    // the parent's translation goes through the effect's transform
    effect->SetTransforms(At(16.0f, 0.0f, 32.0f), At(160.0f, 0.0f, 160.0f));
    CHECK(effect->CellX() == 11 && effect->CellZ() == 12);

    // the city's edges cut the square; a square wholly outside becomes the cell (0, 0)
    effect->SetTransforms(kIdentity, At(1.0f, 0.0f, 1.0f));
    effect->ApplyEffect(0.1f);
    CHECK(Rect(city.demolished.back().rect, 0, 0, 1, 1));
    effect->SetTransforms(kIdentity, At(4090.0f, 0.0f, 4060.0f));
    effect->ApplyEffect(0.1f);
    CHECK(Rect(city.demolished.back().rect, 254, 252, 255, 254));
    effect->SetTransforms(kIdentity, At(-1.0f, 0.0f, 100.0f));         // a negative position is the cell before 0
    CHECK(effect->CellX() == -1 && effect->CellZ() == 6);
    effect->ApplyEffect(0.1f);
    CHECK(Rect(city.demolished.back().rect, 0, 5, 0, 7));
    effect->SetTransforms(kIdentity, At(-100.0f, 0.0f, 100.0f));
    effect->ApplyEffect(0.1f);
    CHECK(Rect(city.demolished.back().rect, 0, 0, 0, 0));
    effect->SetTransforms(kIdentity, At(100.0f, 0.0f, 4200.0f));
    effect->ApplyEffect(0.1f);
    CHECK(Rect(city.demolished.back().rect, 0, 0, 0, 0));
    effect->SetTransforms(kIdentity, At(100.0f, 0.0f, 4100.0f));        // one cell past the edge: its near side is inside
    effect->ApplyEffect(0.1f);
    CHECK(Rect(city.demolished.back().rect, 5, 255, 7, 255));

    description->u18 |= kScrubberDemolishOption;
    effect->SetTransforms(kIdentity, At(200.0f, 0.0f, 200.0f));
    effect->ApplyEffect(0.1f);
    CHECK(city.demolished.back().option && city.demolished.back().filterFlags == 7);

    effect->Stop(0);
    CHECK(!effect->IsStarted());
    effect->Release();
    description->Release();
    SetScrubberHost(nullptr);
}

void TestScrubberActions() {
    City city;
    SetScrubberHost(&city);
    auto* description = new cSC4ScrubberDescription;
    description->AddRef();
    description->u34 = 12;            // fire radius
    description->u38 = 1;             // toxic
    description->u3c = 40;            // extinguish
    description->u40 = 0xabcd1234;    // message
    description->u44 = 77;
    cSC4ScrubberEffect* effect = NewScrubber(description);
    effect->Start(0);
    effect->SetTransforms(kIdentity, At(40.0f, 0.0f, 56.0f));
    effect->ApplyEffect(0.1f);
    CHECK(city.demolished.empty());                           // radius 0: no demolition
    CHECK(city.log.size() == 4);
    if (city.log.size() == 4) {
        CHECK(city.log[0] == "burn 2,3 12");                  // the original's order: burn, toxic, (demolish,) extinguish, message
        CHECK(city.log[1] == "toxic 2,3");
        CHECK(city.log[2] == "extinguish 2,3 40");
        CHECK(city.log[3] == "message");
    }
    CHECK(city.messages.size() == 1 && city.messages[0].type == 0xabcd1234 && city.messages[0].data == 77 && city.messages[0].x == 2 && city.messages[0].z == 3);

    void* data = reinterpret_cast<void*>(std::uintptr_t{0x5150});
    CHECK(!effect->SetVoidParams(7, &data, 1));
    CHECK(effect->SetVoidParams(kScrubberParamMessageData, &data, 1));
    effect->SetTransforms(kIdentity, At(80.0f, 0.0f, 56.0f));
    effect->ApplyEffect(0.1f);
    CHECK(city.messages.size() == 2 && city.messages[1].data == 0x5150 && city.messages[1].x == 5);
    effect->Release();
    description->Release();

    // the "once" kind acts on every tick it gets and is done at once
    auto* once = new cSC4ScrubberDescription;
    once->AddRef();
    once->u18 = kScrubberOnce;
    once->u24 = 2;
    effect = NewScrubber(once);
    effect->Start(0);
    effect->SetTransforms(kIdentity, At(160.0f, 0.0f, 160.0f));
    effect->ApplyEffect(0.1f);
    CHECK(city.demolished.size() == 1 && Rect(city.demolished[0].rect, 8, 8, 12, 12));
    CHECK(!effect->IsStarted());
    effect->Release();
    once->Release();

    // a scrubber with a duration stops itself when it is over
    auto* timed = new cSC4ScrubberDescription;
    timed->AddRef();
    timed->u24 = 1;
    timed->f5c = 1.0f;
    effect = NewScrubber(timed);
    effect->Start(0);
    effect->SetTransforms(kIdentity, At(160.0f, 0.0f, 160.0f));
    effect->ApplyEffect(0.6f);
    CHECK(city.demolished.size() == 2 && effect->IsStarted());
    effect->SetTransforms(kIdentity, At(260.0f, 0.0f, 160.0f));
    effect->ApplyEffect(0.6f);                                // 1.2 s: over, the new cell is not touched
    CHECK(city.demolished.size() == 2 && !effect->IsStarted());
    effect->Release();
    timed->Release();
    SetScrubberHost(nullptr);
}

void TestScrubberPausesAndBlob() {
    City city;
    SetScrubberHost(&city);
    auto* description = new cSC4ScrubberDescription;
    description->AddRef();
    description->u18 = kScrubberPause | kScrubberHiddenPause | kScrubberClockHiddenPause;
    cSC4ScrubberEffect* effect = NewScrubber(description);
    effect->Start(0);
    effect->Start(0);                                         // a second start does nothing
    CHECK(city.log.size() == 3);
    if (city.log.size() == 3) CHECK(city.log[0] == "pause" && city.log[1] == "hidden pause" && city.log[2] == "clock pause");
    effect->Stop(0);
    effect->Stop(0);
    CHECK(city.log.size() == 6);
    if (city.log.size() == 6) CHECK(city.log[3] == "resume" && city.log[4] == "hidden resume" && city.log[5] == "clock resume");
    effect->Release();
    description->Release();

    auto* blob = new cSC4ScrubberDescription;
    blob->AddRef();
    blob->u48 = 7;
    blob->f4c = 16.0f;
    blob->v50 = cS3DVector2{2.0f, 1.0f};
    blob->f58 = 2.5f;
    effect = NewScrubber(blob);
    effect->Start(0);
    effect->SetTransforms(kIdentity, At(168.0f, 0.0f, 168.0f));       // cell 10,10; the blob reaches 2 cells in x, 1 in z
    effect->ApplyEffect(0.1f);
    CHECK(city.blobs.size() == 1 && effect->BlobInMap());
    if (city.blobs.size() == 1) {
        CHECK(city.blobs[0].map == 7 && Rect(city.blobs[0].rect, 8, 9, 12, 11));
        CHECK(city.blobs[0].value == 16.0f && city.blobs[0].spread == 3);
    }
    effect->SetTransforms(kIdentity, At(184.0f, 0.0f, 168.0f));       // the next cell: the old blob comes out, the new goes in
    effect->ApplyEffect(0.1f);
    CHECK(city.blobs.size() == 3);
    if (city.blobs.size() == 3) {
        CHECK(Rect(city.blobs[1].rect, 8, 9, 12, 11) && city.blobs[1].value == -16.0f && city.blobs[1].spread == 3);
        CHECK(Rect(city.blobs[2].rect, 9, 9, 13, 11) && city.blobs[2].value == 16.0f);
    }
    effect->Stop(0);                                          // stopping takes the last blob out
    CHECK(city.blobs.size() == 4 && !effect->BlobInMap());
    if (city.blobs.size() == 4) CHECK(Rect(city.blobs[3].rect, 9, 9, 13, 11) && city.blobs[3].value == -16.0f);
    effect->Release();
    blob->Release();
    SetScrubberHost(nullptr);
}

// ---- attractor --------------------------------------------------------------------------------------------------------------
struct Automata final : IAttractorHost {
    bool occupantManager = true, knownTemplate = true;
    int anchors = 0, alive = 0, controllers = 0;
    std::vector<std::string> log;
    float last[3] = {0, 0, 0};
    bool HasOccupantManager() override { return occupantManager; }
    void* CreateAnchor() override { ++anchors; ++alive; log.push_back("anchor"); return this; }
    void* AddController(void*, const char* name) override {
        log.push_back(std::string("controller ") + name);
        if (!knownTemplate) return nullptr;
        ++controllers;
        return &controllers;
    }
    void RemoveController(void*) override { --controllers; log.push_back("remove controller"); }
    void AddGroupControllers(const char* group, void*) override { log.push_back(std::string("group ") + group); }
    void RemoveGroupControllers(const char* group, void*) override { log.push_back(std::string("remove group ") + group); }
    void MoveAnchor(void*, const float xyz[3], bool inManager) override {
        std::memcpy(last, xyz, sizeof last);
        log.push_back(inManager ? "move in manager" : "move");
    }
    void DestroyAnchor(void*) override { --alive; log.push_back("destroy"); }
};

void TestAttractor() {
    Automata automata;
    SetAttractorHost(&automata);
    auto* group = new cSC4AttractorDescription;
    group->AddRef();
    group->name = "volcano_disaster";
    group->flag = true;
    auto* effect = new cSC4AttractorEffect(group);
    effect->AddRef();

    automata.occupantManager = false;
    effect->Start(0);
    CHECK(!effect->IsStarted() && automata.log.empty());
    automata.occupantManager = true;

    effect->SetTransforms(kIdentity, At(100.0f, 20.0f, 300.0f));     // before the start: only remembered
    CHECK(automata.log.empty());
    effect->Start(0);
    CHECK(effect->IsStarted() && automata.log.size() == 3);
    if (automata.log.size() == 3) CHECK(automata.log[0] == "anchor" && automata.log[1] == "group volcano_disaster" && automata.log[2] == "move in manager");
    CHECK(automata.last[0] == 100.0f && automata.last[1] == 20.0f && automata.last[2] == 300.0f);
    effect->Start(0);                                                // already started
    CHECK(automata.log.size() == 3);
    effect->ApplyEffect(0.25f);
    effect->ApplyEffect(0.5f);
    CHECK(effect->Time() == 0.75f);
    effect->SetTransforms(At(8.0f, 0.0f, 0.0f), At(110.0f, 20.0f, 300.0f));
    CHECK(automata.log.back() == "move" && automata.last[0] == 118.0f);
    effect->Stop(0);
    CHECK(!effect->IsStarted() && automata.alive == 0);
    CHECK(automata.log.size() == 6);
    if (automata.log.size() == 6) CHECK(automata.log[4] == "remove group volcano_disaster" && automata.log[5] == "destroy");
    effect->Stop(0);
    CHECK(automata.log.size() == 6);
    effect->Start(0);                                                // the clock starts over
    CHECK(effect->Time() == 0.0f && automata.anchors == 2);
    effect->Release();                                               // destroyed while started: stopped first
    CHECK(automata.alive == 0 && automata.log.back() == "destroy");
    group->Release();

    automata.log.clear();
    auto* single = new cSC4AttractorDescription;
    single->AddRef();
    single->name = "riot_police_repulsor";
    effect = new cSC4AttractorEffect(single);
    effect->AddRef();
    effect->Start(0);
    CHECK(effect->IsStarted() && automata.controllers == 1 && automata.log[1] == "controller riot_police_repulsor");
    effect->Stop(0);
    CHECK(automata.controllers == 0 && automata.alive == 0);
    automata.knownTemplate = false;                                  // a template nobody knows: not started
    effect->Start(0);
    CHECK(!effect->IsStarted() && automata.alive == 1);
    effect->Release();
    CHECK(automata.alive == 0);
    single->Release();
    SetAttractorHost(nullptr);
}

// ---- sequence ---------------------------------------------------------------------------------------------------------------
struct Item final : ISequenceItemEffect {
    explicit Item(std::string n) : name(std::move(n)) { ++alive; }
    ~Item() override { --alive; }
    void AddRef() override { ++refs; }
    void Release() override { if (--refs == 0) delete this; }
    void Start(int transition) override { started = true; startTransition = transition; }
    void Stop(int transition) override { stopTransition = transition; if (transition != 0) started = false; }   // a soft stop fades
    bool IsStarted() override { return started; }
    void SetEffectTransform(const cS3DTransform& t) override { effect = t; }
    void SetSourceTransform(const cS3DTransform& t) override { source = t; }
    std::string name;
    int refs = 1, startTransition = -1, stopTransition = -1;
    bool started = false;
    cS3DTransform effect, source;
    static inline int alive = 0;
};
struct Effects final : ISequenceHost {
    std::vector<Item*> made;
    std::vector<std::string> asked;
    ISequenceItemEffect* CreateEffect(const char* name) override {
        asked.push_back(name);
        if (std::string(name) == "missing") return nullptr;
        auto* item = new Item(name);
        item->AddRef();                                              // the test keeps one to look at it
        made.push_back(item);
        return item;
    }
    double RandomRange(double low, double high) override { return (low + high) * 0.5; }
    ~Effects() override { for (Item* item : made) item->Release(); }
};

cSC4SequenceDescription* Steps(std::uint32_t flags) {
    auto* description = new cSC4SequenceDescription;
    description->AddRef();
    description->value = flags;
    description->items.resize(3);
    description->items[0].range = cS3DVector2{1.0f, 1.0f};
    description->items[0].name = "first";
    description->items[1].range = cS3DVector2{1.0f, 3.0f};           // a pause, 2 s by the fake's random
    description->items[2].range = cS3DVector2{1.0f, 1.0f};
    description->items[2].name = "last";
    return description;
}

void TestSequence() {
    {
        Effects effects;
        SetSequenceHost(&effects);
        cSC4SequenceDescription* description = Steps(0);
        auto* sequence = new cSC4SequenceEffect(description);
        sequence->AddRef();
        CHECK(!sequence->IsStarted());
        sequence->SetTransforms(At(1.0f, 2.0f, 3.0f), At(10.0f, 20.0f, 30.0f));
        sequence->Start(0);
        CHECK(sequence->IsStarted() && effects.made.empty());
        sequence->ApplyEffect(0.1f);                                  // the first item begins on the first tick
        CHECK(effects.made.size() == 1 && sequence->Item() == 0 && sequence->Playing());
        if (effects.made.size() == 1) {
            Item* first = effects.made[0];
            CHECK(first->name == "first" && first->started && first->startTransition == 0);
            CHECK(first->source.translation[0] == 1.0f && first->effect.translation[2] == 30.0f);
        }
        sequence->ApplyEffect(0.8f);                                  // 0.9 s: still the first
        CHECK(effects.made.size() == 1 && sequence->Item() == 0);
        sequence->ApplyEffect(0.2f);                                  // 1.1 s: the pause; the first effect fades out
        CHECK(sequence->Item() == 1 && !sequence->Playing() && effects.made.size() == 1);
        CHECK(effects.made[0]->stopTransition == 0 && sequence->FadingCount() == 1);
        sequence->ApplyEffect(1.0f);                                  // 2.1 s: the pause lasts until 3
        CHECK(sequence->Item() == 1);
        effects.made[0]->started = false;                             // the faded effect is done
        sequence->ApplyEffect(1.0f);                                  // 3.1 s: the last item
        CHECK(sequence->Item() == 2 && effects.made.size() == 2 && effects.made[1]->name == "last");
        sequence->ApplyEffect(1.0f);                                  // 4.1 s: past the end, no loop: stopped softly
        CHECK(effects.made[1]->stopTransition == 0);
        CHECK(!sequence->Playing());
        CHECK(sequence->IsStarted() && sequence->FadingCount() == 1); // the faded first effect has not been swept yet
        sequence->ApplyEffect(1.0f);                                  // a stopped sequence sweeps on its ticks
        CHECK(!sequence->IsStarted() && sequence->FadingCount() == 0);
        CHECK(effects.made.size() == 2);
        sequence->Release();
        description->Release();
    }
    CHECK(Item::alive == 0);

    {   // a faded effect keeps the sequence "started" until it is done, and is dropped then
        Effects effects;
        SetSequenceHost(&effects);
        cSC4SequenceDescription* description = Steps(0);
        description->items.resize(2);
        auto* sequence = new cSC4SequenceEffect(description);
        sequence->AddRef();
        sequence->Start(0);
        sequence->ApplyEffect(0.1f);
        sequence->ApplyEffect(1.0f);                                  // the pause: "first" fades
        sequence->ApplyEffect(2.5f);                                  // the end
        CHECK(sequence->IsStarted() && sequence->FadingCount() == 1); // still fading
        sequence->ApplyEffect(0.1f);
        CHECK(sequence->FadingCount() == 1);
        effects.made[0]->started = false;
        sequence->ApplyEffect(0.1f);
        CHECK(sequence->FadingCount() == 0 && !sequence->IsStarted());
        sequence->Release();
        description->Release();
    }
    CHECK(Item::alive == 0);

    {   // loop, cut items, the items' start transition
        Effects effects;
        SetSequenceHost(&effects);
        cSC4SequenceDescription* description = Steps(kSequenceLoop | kSequenceCutItems | kSequenceStartTransition);
        auto* sequence = new cSC4SequenceEffect(description);
        sequence->AddRef();
        sequence->Start(0);
        sequence->ApplyEffect(0.1f);
        CHECK(effects.made[0]->startTransition == 1);
        sequence->ApplyEffect(1.0f);                                  // the pause: the first effect is cut, nothing fades
        CHECK(effects.made[0]->stopTransition == 1 && sequence->FadingCount() == 0);
        sequence->ApplyEffect(2.0f);                                  // the last
        sequence->ApplyEffect(1.0f);                                  // around again
        CHECK(sequence->Item() == 0 && sequence->IsStarted() && effects.made.size() == 3 && effects.made[2]->name == "first");
        CHECK(effects.made[1]->stopTransition == 1);
        // stopped hard, started with a transition: it goes on where it was; a plain start begins again
        sequence->Stop(1);
        CHECK(!sequence->IsStarted() && !effects.made[2]->started);
        sequence->Start(1);
        sequence->ApplyEffect(1.0f);
        CHECK(sequence->Item() == 1);
        sequence->Stop(1);
        sequence->Start(0);
        sequence->ApplyEffect(0.1f);
        CHECK(sequence->Item() == 0 && effects.made.back()->name == "first");
        sequence->Release();
        description->Release();
    }
    CHECK(Item::alive == 0);

    {   // an effect nobody knows plays nothing; a sequence without items never starts
        Effects effects;
        SetSequenceHost(&effects);
        cSC4SequenceDescription* description = Steps(0);
        description->items[0].name = "missing";
        auto* sequence = new cSC4SequenceEffect(description);
        sequence->AddRef();
        sequence->Start(0);
        sequence->ApplyEffect(0.1f);
        CHECK(effects.asked.size() == 1 && !sequence->Playing() && sequence->IsStarted());
        sequence->Release();
        description->Release();
        auto* empty = new cSC4SequenceDescription;
        empty->AddRef();
        sequence = new cSC4SequenceEffect(empty);
        sequence->AddRef();
        sequence->Start(0);
        CHECK(!sequence->IsStarted());
        sequence->Release();
        empty->Release();
    }
    SetSequenceHost(nullptr);
}

}  // namespace

int main() {
    TestScrubberFilter();
    TestScrubberDemolish();
    TestScrubberActions();
    TestScrubberPausesAndBlob();
    TestAttractor();
    TestSequence();
    std::printf(failures ? "%d FAILURES\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}

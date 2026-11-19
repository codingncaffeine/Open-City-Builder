// cGZScriptTerp — test. Exercises the interpreter end to end: COM lifetime/QueryInterface, target binding
// (the two QueryInterface'd sub-targets), the context, the vocabulary stack and its forwarders, the
// tokenizer, the eleven-state parser (tags, numeric and vocabulary-resolved names, quoted/unquoted values,
// literal text, the unknown-property and closing-tag quirks, the error paths), re-entrancy, ExecuteScriptFile
// and ExecuteScriptResource through a fake persistence service, and the destructor's reference-count quirks.
//
// Build (from the repo root):
//   /usr/bin/clang++ -std=c++20 -Wall -Wextra -O1 -fsanitize=address,undefined -Isrc
//       -Iscripts/tests/stubs -Iscripts/tests scripts/tests/gz_scriptterp_test.cpp src/gz/ScriptTerp.cpp
//       src/rz/cRZString.cpp src/rz/cRZVariant.cpp src/rz/cRZFile.cpp src/rz/RZConvert.cpp
//       src/rz/cRZSysServPtr.cpp -o /tmp/terp_test
//
// The seams stand in for two unrecovered neighbours: IScriptTerpHost supplies cGZScriptVocab (the keyword
// table, 13 symbols) and the RZ service resolver supplies cIGZPersistResourceManager for the resource entry
// point. Both fakes behave like the real objects where the interpreter can observe them (the vocabulary is
// case-insensitive and hands out counted references; the record is a cIGZPersistDBRecord exposing the
// 0xc56b8f08 sized-record view, and CloseDBRecord closes and releases it like the real manager does).
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "fake_resourcemanager.h"
#include "gz/ScriptTerp.h"
#include "gz/cIGZPersistDBSerialRecord.h"
#include "gz/cIGZPersistRecord.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"
#include "rz/cRZSysServPtr.h"

static int gFailures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            ++gFailures;                                                              \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        if (!((a) == (b))) {                                                                              \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                            \
            ++gFailures;                                                                                  \
        }                                                                                                 \
    } while (0)

namespace {

// ---------------------------------------------------------------------------------------------------------
// Value rendering (the script's output is observed as text).
// ---------------------------------------------------------------------------------------------------------
std::string TextOf(const cIGZString& text) { return std::string(text.Data(), text.Strlen()); }

std::string VariantText(const cIGZVariant* value) {
    if (!value) return "<null>";
    switch (value->GetType()) {
    case cIGZVariant::kTypeUint32: return "u32:" + std::to_string(value->GetValUint32());
    case cIGZVariant::kTypeRZChar | cIGZVariant::kTypeArrayFlag: {
        cRZString text;
        value->GetValString(text);
        return "str:" + TextOf(text);
    }
    default: return "type:" + std::to_string(value->GetType());
    }
}

// ---------------------------------------------------------------------------------------------------------
// Fake vocabulary: cGZScriptVocab (unrecovered). Case-insensitive map, counted values, counted reference in
// GetTranslation — the contract the interpreter relies on. The interpreter owns the instance the host
// creates, so Release deletes it here.
// ---------------------------------------------------------------------------------------------------------
struct FakeVocab final : IScriptVocab {
    struct Entry {
        std::string name;
        cIGZVariant* value;
    };
    std::vector<Entry> entries;
    std::uint32_t refs = 1;

    static bool SameName(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    }

    Entry* Find(const std::string& name) {
        for (Entry& entry : entries) {
            if (SameName(entry.name, name)) return &entry;
        }
        return nullptr;
    }

    // Inserts an already-referenced value (the map consumes that reference). Like the original, an existing
    // name is NOT replaced: AddTranslation returns false and keeps the old value (0x10054e1cf).
    bool Insert(const std::string& name, cIGZVariant* value) {
        if (Find(name)) return false;
        entries.push_back(Entry{name, value});
        return true;
    }

    // cIGZUnknown
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (!out || (iid != kGZIID_cIGZUnknown && iid != kGZIID_cIGZScriptVocab)) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        const std::uint32_t remaining = --refs;
        if (remaining == 0) delete this;
        return remaining;
    }
    ~FakeVocab() override {
        for (Entry& entry : entries) entry.value->Release();
    }

    // IScriptVocab
    bool AddTranslation(const cIGZString& name, cIGZVariant* value) override {
        if (Find(TextOf(name))) return false;   // keeps the first entry, the caller keeps its reference
        value->AddRef();
        return Insert(TextOf(name), value);
    }
    bool AddTranslation(const cIGZString& name, std::uint32_t value) override {
        if (Find(TextOf(name))) return false;
        cRZVariant* stored = new cRZVariant(value);
        stored->AddRef();
        return Insert(TextOf(name), stored);
    }
    bool RemoveTranslation(const cIGZString& name) override {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!SameName(entries[i].name, TextOf(name))) continue;
            entries[i].value->Release();
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    }
    bool GetTranslation(const cIGZString& name, cIGZVariant** out) override {
        Entry* entry = Find(TextOf(name));
        if (!entry) return false;
        entry->value->AddRef();
        *out = entry->value;
        return true;
    }
    void Clear() override {
        for (Entry& entry : entries) entry.value->Release();
        entries.clear();
    }

    std::size_t Size() const { return entries.size(); }
};

// ---------------------------------------------------------------------------------------------------------
// Fake target: implements all three target interfaces the way cGZUIScriptService does (three sub-vtables,
// one reference count), records every callback and can push a shared vocabulary for the script's duration.
// The test owns it; Release only counts references here (the interpreter never deletes its target).
// ---------------------------------------------------------------------------------------------------------
struct FakeTarget final : cIGZScriptTarget, cIGZScriptInterpretingTarget, cIGZScriptCompilingTarget {
    struct Event {
        enum Kind { kBegin, kEnd, kLiteral, kBeginTag, kEndTag, kProperty, kTag } kind;
        std::uint32_t id = 0;
        std::string text;
        bool success = false;
        std::vector<std::pair<std::uint32_t, std::string>> params;
    };
    std::vector<Event> events;
    std::uint32_t refs = 1;

    IScriptVocab* sharedVocab = nullptr;    // pushed in ScriptBegin, popped in ScriptEnd (the service's job)
    cGZScriptTerp* reenterTerp = nullptr;   // when set, ScriptLiteral runs a nested script
    std::string reenterScript;
    bool nestedResult = true;
    int nestedAttempts = 0;
    std::uint32_t paramRefsInScriptTag = 0;   // first parameter's reference count inside ScriptTag

    // cIGZUnknown (one counter for the three sub-objects)
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (!out) return false;
        if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZScriptTarget) {
            *out = static_cast<cIGZScriptTarget*>(this);
        } else if (iid == kGZIID_cIGZScriptInterpretingTarget) {
            *out = static_cast<cIGZScriptInterpretingTarget*>(this);
        } else if (iid == kGZIID_cIGZScriptCompilingTarget) {
            *out = static_cast<cIGZScriptCompilingTarget*>(this);
        } else {
            return false;
        }
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs ? --refs : 0; }

    // cIGZScriptTarget
    bool ScriptBegin(cGZScriptTerp* terp) override {
        events.push_back(Event{Event::kBegin, 0, std::string(), false, {}});
        if (sharedVocab) terp->PushVocabulary(sharedVocab);
        return true;
    }
    bool ScriptEnd(cGZScriptTerp* terp, bool success) override {
        Event event;
        event.kind = Event::kEnd;
        event.success = success;
        events.push_back(event);
        if (sharedVocab) terp->PopVocabulary();
        return true;
    }
    bool ScriptLiteral(cGZScriptTerp*, cIGZString* text) override {
        Event event;
        event.kind = Event::kLiteral;
        event.text = TextOf(*text);
        events.push_back(event);
        if (reenterTerp) {
            ++nestedAttempts;
            nestedResult = reenterTerp->ExecuteScriptString(cRZString(reenterScript.c_str()));
        }
        return true;
    }

    // cIGZScriptInterpretingTarget
    bool BeginExecuteTag(cGZScriptTerp*, std::uint32_t tag) override {
        Event event;
        event.kind = Event::kBeginTag;
        event.id = tag;
        events.push_back(event);
        return true;
    }
    bool EndExecuteTag(cGZScriptTerp*, std::uint32_t tag) override {
        Event event;
        event.kind = Event::kEndTag;
        event.id = tag;
        events.push_back(event);
        return true;
    }
    bool ApplyProperty(cGZScriptTerp*, std::uint32_t id, cIGZVariant* value) override {
        Event event;
        event.kind = Event::kProperty;
        event.id = id;
        event.text = VariantText(value);
        events.push_back(event);
        return true;
    }

    // cIGZScriptCompilingTarget
    bool ScriptTag(cGZScriptTerp*, std::uint32_t tag, tScriptPropertyArray& parameters) override {
        Event event;
        event.kind = Event::kTag;
        event.id = tag;
        for (const tScriptProperty& property : parameters) {
            event.params.emplace_back(property.id, VariantText(property.value));
        }
        if (!parameters.empty() && parameters.front().value) {
            // AddRef reports the new count: the array holds exactly one reference while ScriptTag runs.
            paramRefsInScriptTag = parameters.front().value->AddRef();
            parameters.front().value->Release();
        }
        events.push_back(event);
        return true;
    }

    std::string Describe() const {
        std::string out;
        for (const Event& event : events) {
            if (!out.empty()) out += " | ";
            switch (event.kind) {
            case Event::kBegin: out += "begin"; break;
            case Event::kEnd: out += event.success ? "end ok" : "end error"; break;
            case Event::kLiteral: out += "literal:" + event.text; break;
            case Event::kBeginTag: out += "begintag:" + std::to_string(event.id); break;
            case Event::kEndTag: out += "endtag:" + std::to_string(event.id); break;
            case Event::kProperty: out += "prop:" + std::to_string(event.id) + "=" + event.text; break;
            case Event::kTag: {
                out += "tag:" + std::to_string(event.id) + "[";
                for (std::size_t i = 0; i < event.params.size(); ++i) {
                    if (i) out += ",";
                    out += std::to_string(event.params[i].first) + "=" + event.params[i].second;
                }
                out += "]";
                break;
            }
            }
        }
        return out;
    }
};

// ---------------------------------------------------------------------------------------------------------
// Fake host: creates the interpreter's own vocabulary (operator new(0x28) + cGZScriptVocab::cGZScriptVocab
// @ 0x10054ddf4 in the original).
// ---------------------------------------------------------------------------------------------------------
struct FakeHost final : IScriptTerpHost {
    std::vector<std::pair<std::string, std::uint32_t>> seed;
    FakeVocab* created = nullptr;

    IScriptVocab* CreateVocabulary() override {
        FakeVocab* vocab = new FakeVocab();
        for (const auto& entry : seed) vocab->AddTranslation(cRZString(entry.first.c_str()), entry.second);
        created = vocab;
        return vocab;
    }
};

// ---------------------------------------------------------------------------------------------------------
// Fake persistence: a database record with the sized-record view (iid 0xc56b8f08) and a manager that opens
// it. Both are deleted through Release (the interpreter owns neither: cRZSysServPtr owns the manager and
// CloseDBRecord the record). The counters are members of the test, not of the objects, because the objects
// are gone by the time the call returns.
// ---------------------------------------------------------------------------------------------------------
struct FakeRecord final : cIGZPersistDBRecord, cIGZPersistDBSerialRecordStub {
    static int alive;
    static int reads;

    std::uint32_t refs = 1;   // the caller's reference (a fresh record, like cIGZPersistDBSegment::OpenRecord)
    std::string text;
    cGZPersistResourceKey key;
    bool giveSerialView = true;
    bool* releasedCell = nullptr;

    FakeRecord() { ++alive; }
    ~FakeRecord() { --alive; }

    // cIGZUnknown (one counter for both views)
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (!out) return false;
        if (iid == kGZIID_cIGZPersistDBSerialRecord) {
            if (!giveSerialView) return false;
            *out = static_cast<cIGZPersistDBSerialRecord*>(this);
        } else if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZPersistDBRecord) {
            *out = static_cast<cIGZPersistDBRecord*>(this);
        } else {
            return false;
        }
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        const std::uint32_t remaining = --refs;
        if (remaining == 0) {
            if (releasedCell) *releasedCell = true;
            delete this;
        }
        return remaining;
    }

    // cIGZPersistDBRecord
    cGZPersistResourceKey GetKey() const override { return key; }
    std::uint32_t GetAccessFlags() const override { return 1; }
    bool Close() override { return true; }

    // cIGZPersistDBSerialRecord (the sized-record view)
    cIGZPersistDBRecord* AsIGZPersistDBRecord() override { return static_cast<cIGZPersistDBRecord*>(this); }
    std::uint32_t GetSize() override { return static_cast<std::uint32_t>(text.size()); }   // slot 0x1a0
    bool GetFieldVoid(void* destination, std::uint32_t byteCount) override {               // slot 0xd8
        ++reads;
        std::memcpy(destination, text.data(), std::min<std::size_t>(byteCount, text.size()));
        return true;
    }
};
int FakeRecord::alive = 0;
int FakeRecord::reads = 0;

struct FakeManager final : FakeResourceManager {
    std::uint32_t refs = 1;
    bool open = true;
    std::string recordText;
    bool giveSerialView = true;
    // Observation cells owned by the test (the manager is deleted by cRZSysServPtr before the call returns).
    int* opens = nullptr;
    int* closes = nullptr;
    bool* recordReleased = nullptr;
    cGZPersistResourceKey* lastOpen = nullptr;
    bool* deletedFlag = nullptr;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        const std::uint32_t remaining = --refs;
        if (remaining == 0) {
            if (deletedFlag) *deletedFlag = true;
            delete this;
        }
        return remaining;
    }

    bool OpenDBRecord(const cGZPersistResourceKey& key, cIGZPersistDBRecord** out, bool) override {
        if (opens) ++*opens;
        if (lastOpen) *lastOpen = key;
        if (!open) return false;
        FakeRecord* record = new FakeRecord();   // the caller's reference
        record->text = recordText;
        record->key = key;
        record->giveSerialView = giveSerialView;
        record->releasedCell = recordReleased;
        *out = static_cast<cIGZPersistDBRecord*>(record);
        return true;
    }
    // Same as cGZPersistResourceManager::CloseDBRecord (0x1004f8f6): close, then release the record.
    bool CloseDBRecord(const cGZPersistResourceKey&, cIGZPersistDBRecord** out) override {
        if (closes) ++*closes;
        const bool ok = (*out)->Close();
        (*out)->Release();
        *out = nullptr;
        return ok;
    }
};

FakeManager* gResolvedManager = nullptr;

bool ResolveService(std::uint32_t serviceId, std::uint32_t interfaceId, void** out) {
    if (!gResolvedManager || serviceId != kGZSID_PersistResourceManager ||
        interfaceId != kGZIID_PersistResourceManager) {
        return false;
    }
    *out = static_cast<cIGZPersistResourceManager*>(gResolvedManager);
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Per-test fixture: installs the host, creates a fresh interpreter and a recording target.
// ---------------------------------------------------------------------------------------------------------
struct Fixture {
    FakeHost host;
    cGZScriptTerp* terp = nullptr;
    FakeTarget* target = nullptr;

    explicit Fixture(std::vector<std::pair<std::string, std::uint32_t>> words = {}) {
        host.seed = std::move(words);
        SetScriptTerpHost(&host);
        terp = new cGZScriptTerp();
        target = new FakeTarget();   // the test's reference; the interpreter takes a second one
    }
    ~Fixture() {
        delete terp;      // releases the target (leaving the test's reference)
        delete target;
        SetScriptTerpHost(nullptr);
    }

    // The script keyword table the scripts below are written in (the real one is cGZLegacyControl's).
    static std::vector<std::pair<std::string, std::uint32_t>> Words() {
        return {{"GZWinBtn", 0x2000}, {"GZWinGen", 0x2001}, {"id", 0x100}, {"area", 0x101},
                {"parent", 0x102},   {"caption", 0x105}, {"Base", 11}};
    }

    void Attach() { terp->SetScriptTarget(target); }
    bool RunScript(const char* script) { return terp->ExecuteScriptString(cRZString(script)); }
    std::string Run(const char* script) {
        terp->ExecuteScriptString(cRZString(script));
        return target->Describe();
    }
    void Reset() {
        target->events.clear();
        target->paramRefsInScriptTag = 0;
    }
};

// ---------------------------------------------------------------------------------------------------------
// 1. COM lifetime and QueryInterface
// ---------------------------------------------------------------------------------------------------------
void TestLifetime() {
    SetScriptTerpHost(nullptr);   // no host: the vocabulary stack stays empty
    cGZScriptTerp* terp = new cGZScriptTerp();
    void* out = nullptr;
    CHECK(terp->QueryInterface(kGZIID_cIGZUnknown, &out));
    CHECK(out == static_cast<cIGZScriptTerp*>(terp));
    CHECK(terp->QueryInterface(kGZIID_cIGZScriptTerp, &out));
    CHECK(out == static_cast<cIGZScriptTerp*>(terp));
    CHECK(!terp->QueryInterface(0x12345678, &out));
    CHECK(!terp->QueryInterface(kGZIID_cIGZUnknown, nullptr));   // the original would dereference it

    // The constructor starts the count at 0; the two QueryInterfaces brought it to 2.
    CHECK_EQ(terp->AddRef(), 3u);
    CHECK_EQ(terp->Release(), 2u);
    CHECK_EQ(terp->Release(), 1u);
    CHECK_EQ(terp->Release(), 0u);   // deletes at 1 (the original compares against 2)

    // Without a host there is no vocabulary: the forwarders fail instead of dereferencing null.
    cGZScriptTerp* bare = new cGZScriptTerp();
    CHECK(!bare->AddToken(cRZString("x"), 1u));
    CHECK(!bare->RemoveToken(cRZString("x")));
    CHECK(!bare->RemoveAllTokens());
    CHECK(!bare->PopVocabulary());
    cIGZVariant* value = nullptr;
    CHECK(!bare->GetLocalTranslation(cRZString("x"), &value));
    delete bare;
}

// ---------------------------------------------------------------------------------------------------------
// 2. The context: borrowed from GetContext, released on replacement, never by the destructor
// ---------------------------------------------------------------------------------------------------------
void TestContext() {
    Fixture fixture;
    cRZVariant* first = new cRZVariant(std::uint32_t(7));
    fixture.terp->SetContext(first);
    CHECK(fixture.terp->GetContext() == first);
    CHECK_EQ(first->AddRef(), 2u);   // the test's creation reference + the interpreter's
    first->Release();

    // Replacing releases the old context (count 1 -> 0 -> destroyed) and references the new one.
    cRZVariant* second = new cRZVariant(std::uint32_t(8));
    fixture.terp->SetContext(second);
    CHECK(fixture.terp->GetContext() == second);
    fixture.terp->SetContext(nullptr);
    CHECK(fixture.terp->GetContext() == nullptr);

    // Quirk: the destructor never releases +0x40 (0x10054e818 does not touch it). Keep a reference of our
    // own so the count can be observed after the interpreter is gone.
    cRZVariant* kept = new cRZVariant(std::uint32_t(9));
    kept->AddRef();   // the test's reference (count 1)
    fixture.terp->SetContext(kept);   // count 2
    delete fixture.terp;
    fixture.terp = nullptr;
    CHECK_EQ(kept->AddRef(), 3u);   // still 2 before this AddRef: the interpreter did not release it
    kept->Release();
    kept->Release();
    kept->Release();   // the count was 1 after the two above: the test's own reference
}

// ---------------------------------------------------------------------------------------------------------
// 3. The vocabulary stack and the token forwarders
// ---------------------------------------------------------------------------------------------------------
void TestVocabularyStack() {
    Fixture fixture(Fixture::Words());
    FakeVocab* base = fixture.host.created;
    CHECK(base != nullptr);
    CHECK_EQ(base->Size(), 7u);
    CHECK_EQ(base->refs, 1u);   // the creation reference, owned by the interpreter

    // AddToken(name, variant): the vocabulary takes over the reference of the temporary variant.
    cRZVariant* value = new cRZVariant(std::uint32_t(42));
    CHECK(fixture.terp->AddToken(cRZString("Word"), value));
    CHECK_EQ(value->AddRef(), 2u);
    value->Release();
    CHECK_EQ(base->Size(), 8u);
    // The map is case-insensitive (cGZScriptVocab::l_stricmp @ 0x10054ddb0).
    CHECK_EQ(VariantText(base->Find("WORD")->value), "u32:42");

    // AddToken(name, uint) creates the cRZVariant itself.
    CHECK(fixture.terp->AddToken(cRZString("Number"), 7u));
    CHECK_EQ(VariantText(base->Find("number")->value), "u32:7");
    // Quirk: an existing name is not replaced. cGZScriptVocab::AddTranslation keeps the first entry and the
    // variant AddToken just allocated is never referenced (0x10054e1cf skips its AddRef), so it leaks.
    CHECK(!fixture.terp->AddToken(cRZString("Word"), 43u));
    CHECK_EQ(VariantText(base->Find("word")->value), "u32:42");
    CHECK_EQ(base->Size(), 9u);

    CHECK(fixture.terp->RemoveToken(cRZString("wOrD")));
    CHECK(base->Find("word") == nullptr);
    CHECK(fixture.terp->RemoveAllTokens());
    CHECK_EQ(base->Size(), 0u);

    // GetLocalTranslation hands out a counted reference and searches the stack from the top.
    base->AddTranslation(cRZString("OnlyBase"), 5u);
    base->AddTranslation(cRZString("Base"), 11u);   // RemoveAllTokens dropped the seeded entries
    cIGZVariant* found = nullptr;
    CHECK(fixture.terp->GetLocalTranslation(cRZString("onlybase"), &found));
    CHECK_EQ(VariantText(found), "u32:5");
    CHECK_EQ(found->AddRef(), 3u);   // the map's + the lookup's + this AddRef
    found->Release();
    found->Release();
    CHECK(!fixture.terp->GetLocalTranslation(cRZString("missing"), &found));

    // Push takes a reference, pop drops it, and the interpreter's own vocabulary is never popped.
    FakeVocab* pushed = new FakeVocab();   // the test's reference
    pushed->AddTranslation(cRZString("OnlyBase"), 99u);
    CHECK(fixture.terp->PushVocabulary(pushed));
    CHECK_EQ(pushed->refs, 2u);
    CHECK(!fixture.terp->PushVocabulary(nullptr));
    cIGZVariant* shadowed = nullptr;
    CHECK(fixture.terp->GetLocalTranslation(cRZString("OnlyBase"), &shadowed));
    CHECK_EQ(VariantText(shadowed), "u32:99");   // the top of the stack wins
    shadowed->Release();
    CHECK(fixture.terp->PopVocabulary());
    CHECK_EQ(pushed->refs, 1u);
    CHECK(!fixture.terp->PopVocabulary());   // the bottom vocabulary stays
    CHECK(fixture.terp->GetLocalTranslation(cRZString("OnlyBase"), &shadowed));
    CHECK_EQ(VariantText(shadowed), "u32:5");
    shadowed->Release();
    pushed->Release();   // count 1 -> 0 -> deleted

    // A vocabulary pushed by the target in ScriptBegin is used for that script only (cGZUIScriptService
    // pushes the shared window-script vocabulary this way).
    FakeVocab* service = new FakeVocab();
    service->AddTranslation(cRZString("Base"), 0x2000u);
    fixture.target->sharedVocab = service;
    fixture.Attach();
    CHECK_EQ(fixture.Run("<Base>"), "begin | begintag:8192 | endtag:8192 | tag:8192[] | end ok");
    fixture.target->sharedVocab = nullptr;   // ScriptBegin pushed it, ScriptEnd popped it
    fixture.Reset();
    CHECK_EQ(fixture.Run("<Base>"), "begin | begintag:11 | endtag:11 | tag:11[] | end ok");
    CHECK_EQ(service->refs, 1u);
    service->Release();
}

// ---------------------------------------------------------------------------------------------------------
// 4. Word resolution: GetUint32, parseOpenTag/parseCloseTag in isolation
// ---------------------------------------------------------------------------------------------------------
void TestResolution() {
    Fixture fixture(Fixture::Words());
    std::uint32_t value = 0;
    CHECK(fixture.terp->GetUint32(cRZString("id"), value));
    CHECK_EQ(value, 0x100u);
    CHECK(fixture.terp->GetUint32(cRZString("ID"), value));   // case-insensitive through the vocabulary
    CHECK_EQ(value, 0x100u);
    CHECK(fixture.terp->GetUint32(cRZString("0x2000"), value));
    CHECK_EQ(value, 0x2000u);
    // RZConvertToUintPtrA switches to base 16 as soon as an A-F/a-f appears: "12abc" is 0x12abc, "5g" is 5.
    CHECK(fixture.terp->GetUint32(cRZString("12abc"), value));
    CHECK_EQ(value, 0x12abcu);
    CHECK(fixture.terp->GetUint32(cRZString("5g"), value));
    CHECK_EQ(value, 5u);
    // Only a leading ASCII digit starts a number (the ctype _CTYPE_D gate @ 0x10054f717).
    CHECK(!fixture.terp->GetUint32(cRZString("+5"), value));
    CHECK(!fixture.terp->GetUint32(cRZString("-5"), value));
    CHECK(fixture.terp->GetUint32(cRZString("0x"), value));   // only the first byte is gated: "0x" is 0
    CHECK_EQ(value, 0u);
    CHECK(!fixture.terp->GetUint32(cRZString(""), value));
    CHECK(!fixture.terp->GetUint32(cRZString("xyz"), value));
    // A vocabulary entry whose value is not a uint32 is not an id (0x10054f6da).
    FakeVocab* base = fixture.host.created;
    base->AddTranslation(cRZString("StrTag"), new cRZVariant(cRZString("text")));   // the map takes the reference
    CHECK(!fixture.terp->GetUint32(cRZString("StrTag"), value));
    base->RemoveTranslation(cRZString("StrTag"));

    // The tag helpers on their own: BeginExecuteTag, then ScriptTag/EndExecuteTag around the property list.
    fixture.Attach();
    cRZString tag("GZWinGen");
    CHECK(fixture.terp->parseOpenTag(&tag));
    CHECK_EQ(fixture.target->Describe(), "begintag:8193");
    CHECK(fixture.terp->parseCloseTag());
    CHECK_EQ(fixture.target->Describe(), "begintag:8193 | endtag:8193 | tag:8193[]");
    // An unresolved tag name leaves no tag open and reports failure.
    cRZString unknown("nope");
    CHECK(!fixture.terp->parseOpenTag(&unknown));
    CHECK(fixture.terp->parseCloseTag());   // nothing open: no callbacks
    CHECK_EQ(fixture.target->Describe(), "begintag:8193 | endtag:8193 | tag:8193[]");
}

// ---------------------------------------------------------------------------------------------------------
// 5. A full window script: tags, properties, literal text
// ---------------------------------------------------------------------------------------------------------
void TestFullScript() {
    Fixture fixture(Fixture::Words());
    fixture.Attach();
    const char* script = "<GZWinBtn id=1 area=\"0,0,10,20\">Hi there<GZWinGen parent=7>tail";
    CHECK_EQ(fixture.Run(script),
             "begin | begintag:8192 | prop:256=u32:1 | prop:257=str:0,0,10,20 | endtag:8192 | "
             "tag:8192[256=u32:1,257=str:0,0,10,20] | literal:Hi there | begintag:8193 | "
             "prop:258=u32:7 | endtag:8193 | tag:8193[258=u32:7] | literal:tail | end ok");
    CHECK(fixture.RunScript(script));
    // The parameter array holds one reference per variant while ScriptTag runs.
    CHECK_EQ(fixture.target->paramRefsInScriptTag, 2u);   // 1 array reference + the AddRef inside ScriptTag

    // An unquoted value: numeric words become u32 variants, other words string variants
    // (parseParmValue @ 0x10054f538).
    fixture.Reset();
    CHECK_EQ(fixture.Run("<GZWinBtn id=abc caption=17>"),
             "begin | begintag:8192 | prop:256=str:abc | prop:261=u32:17 | endtag:8192 | "
             "tag:8192[256=str:abc,261=u32:17] | end ok");

    // Digit-led words resolve as ids, hexadecimal included.
    fixture.Reset();
    CHECK_EQ(fixture.Run("<12abc>"), "begin | begintag:76476 | endtag:76476 | tag:76476[] | end ok");
}

// ---------------------------------------------------------------------------------------------------------
// 6. Unknown properties, unknown tags (the closing-tag quirk)
// ---------------------------------------------------------------------------------------------------------
void TestUnknownNames() {
    Fixture fixture(Fixture::Words());
    fixture.Attach();
    // A property name the vocabulary does not know swallows its "=value": no callback, no parameter.
    CHECK_EQ(fixture.Run("<GZWinBtn foo=\"bar\" id=2>"),
             "begin | begintag:8192 | prop:256=u32:2 | endtag:8192 | tag:8192[256=u32:2] | end ok");
    fixture.Reset();
    CHECK_EQ(fixture.Run("<GZWinBtn foo id=3>"),
             "begin | begintag:8192 | prop:256=u32:3 | endtag:8192 | tag:8192[256=u32:3] | end ok");
    fixture.Reset();
    // A tag name that does not resolve is skipped up to '>', which is why "</GZWinBtn>" is harmless:
    // '/' is part of the word, so a closing tag is not a tag at all.
    CHECK_EQ(fixture.Run("<GZWinBtn id=4></GZWinBtn>"),
             "begin | begintag:8192 | prop:256=u32:4 | endtag:8192 | tag:8192[256=u32:4] | end ok");
    fixture.Reset();
    CHECK_EQ(fixture.Run("<+5>"), "begin | end ok");   // "+5" is not a number: the tag is skipped
    fixture.Reset();
    CHECK_EQ(fixture.Run("<>"), "begin | end ok");     // an empty tag is ignored
    fixture.Reset();
    CHECK_EQ(fixture.Run("a<b"), "begin | literal:a | end ok");   // "<b" cannot open a tag: skipped
}

// ---------------------------------------------------------------------------------------------------------
// 7. Quoted strings, literals and the tokenizer's edge cases
// ---------------------------------------------------------------------------------------------------------
void TestQuotedAndLiteral() {
    Fixture fixture(Fixture::Words());
    fixture.Attach();
    // An empty quoted value is an empty string.
    CHECK_EQ(fixture.Run("<GZWinBtn 256=\"\">"),
             "begin | begintag:8192 | prop:256=str: | endtag:8192 | tag:8192[256=str:] | end ok");
    fixture.Reset();
    // Quotes protect the delimiters: ">" and "<" inside a value stay in the value.
    CHECK_EQ(fixture.Run("<GZWinBtn 256=\"a > b < c\">"),
             "begin | begintag:8192 | prop:256=str:a > b < c | endtag:8192 | "
             "tag:8192[256=str:a > b < c] | end ok");
    fixture.Reset();
    // Text outside tags is reported verbatim, delimiters and all, up to the next '<'.
    CHECK_EQ(fixture.Run("x = y > z"), "begin | literal:x = y > z | end ok");
    fixture.Reset();
    // An unterminated quote at the very end stops the token stream without an error (state 5 is not the error
    // state) and the pending tag is never closed: no EndExecuteTag/ScriptTag.
    CHECK(fixture.RunScript("<GZWinBtn 256=\""));
    CHECK_EQ(fixture.target->Describe(), "begin | begintag:8192 | end ok");
    fixture.Reset();
    // A quote where the tag name goes is not a word: the tag cannot open, the script fails.
    CHECK(!fixture.RunScript("<\"x\">"));
    CHECK_EQ(fixture.target->Describe(), "begin | end error");
    fixture.Reset();
    // A quoted string where a property name is expected is an error too.
    CHECK(!fixture.RunScript("<GZWinBtn \"x\">"));
    CHECK_EQ(fixture.target->Describe(), "begin | begintag:8192 | end error");
    fixture.Reset();
    CHECK(!fixture.RunScript("<GZWinBtn ="));
    CHECK_EQ(fixture.target->Describe(), "begin | begintag:8192 | end error");
    // The interpreter is usable again after an error.
    fixture.Reset();
    CHECK(fixture.RunScript("<GZWinBtn id=8>"));
}

// ---------------------------------------------------------------------------------------------------------
// 8. Re-entrancy: a callback may not start another script
// ---------------------------------------------------------------------------------------------------------
void TestReentrancy() {
    Fixture fixture(Fixture::Words());
    fixture.Attach();
    fixture.target->reenterTerp = fixture.terp;
    fixture.target->reenterScript = "<GZWinGen id=9>";
    CHECK_EQ(fixture.Run("<GZWinBtn>text"),
             "begin | begintag:8192 | endtag:8192 | tag:8192[] | literal:text | end ok");
    CHECK_EQ(fixture.target->nestedAttempts, 1);
    CHECK(!fixture.target->nestedResult);   // ExecuteScript refuses while a script is running
    fixture.target->reenterTerp = nullptr;

    // The refused call still overwrote +0x50 with the nested script's pointer, as the original does; the next
    // script simply sets it again.
    fixture.Reset();
    CHECK(fixture.RunScript("<GZWinGen>"));
    CHECK_EQ(fixture.target->Describe(), "begin | begintag:8193 | endtag:8193 | tag:8193[] | end ok");
}

// ---------------------------------------------------------------------------------------------------------
// 9. ExecuteScriptFile
// ---------------------------------------------------------------------------------------------------------
void TestScriptFile() {
    Fixture fixture(Fixture::Words());
    fixture.Attach();
    const char* path = "/tmp/gz_scriptterp_test_script.txt";
    std::FILE* file = std::fopen(path, "wb");
    CHECK(file != nullptr);
    if (file) {
        const char* text = "<GZWinBtn id=1>file text";
        std::fwrite(text, 1, std::strlen(text), file);
        std::fclose(file);
    }
    CHECK(fixture.terp->ExecuteScriptFile(cRZString(path)));
    CHECK_EQ(fixture.target->Describe(),
             "begin | begintag:8192 | prop:256=u32:1 | endtag:8192 | tag:8192[256=u32:1] | "
             "literal:file text | end ok");
    std::remove(path);

    // A missing file and an empty file are both rejected without touching the target.
    fixture.Reset();
    CHECK(!fixture.terp->ExecuteScriptFile(cRZString("/tmp/gz_scriptterp_test_missing.txt")));
    CHECK_EQ(fixture.target->Describe(), "");
    file = std::fopen(path, "wb");
    CHECK(file != nullptr);
    if (file) std::fclose(file);
    CHECK(!fixture.terp->ExecuteScriptFile(cRZString(path)));
    CHECK_EQ(fixture.target->Describe(), "");
    std::remove(path);

    // +0x50 is left dangling after the buffer is deleted (quirk); a following string script still works.
    CHECK(fixture.RunScript("<GZWinBtn>"));
    CHECK_EQ(fixture.target->Describe(), "begin | begintag:8192 | endtag:8192 | tag:8192[] | end ok");
}

// ---------------------------------------------------------------------------------------------------------
// 10. ExecuteScriptResource (the persistence seam)
// ---------------------------------------------------------------------------------------------------------
void TestScriptResource() {
    Fixture fixture(Fixture::Words());
    fixture.Attach();
    const cGZPersistResourceKey key{0x11111111u, 0x22222222u, 0x33333333u};

    // No service: the original dereferences the null manager (0x10054f031); the port reports failure.
    SetRZServiceResolver(ResolveService);
    CHECK(!fixture.terp->ExecuteScriptResource(key));

    // OpenDBRecord -> the record, the sized-record view, the script, CloseDBRecord: both objects are gone by
    // the time the call returns, so all observations go through cells owned by the test.
    bool managerDeleted = false;
    bool recordReleased = false;
    int opens = 0;
    int closes = 0;
    cGZPersistResourceKey lastOpen{};
    FakeManager* manager = new FakeManager();   // the reference cRZSysServPtr ends up releasing
    manager->opens = &opens;
    manager->closes = &closes;
    manager->recordReleased = &recordReleased;
    manager->lastOpen = &lastOpen;
    manager->deletedFlag = &managerDeleted;
    manager->recordText = "<GZWinBtn id=2>from a record";
    gResolvedManager = manager;
    CHECK(fixture.terp->ExecuteScriptResource(key));
    CHECK_EQ(fixture.target->Describe(),
             "begin | begintag:8192 | prop:256=u32:2 | endtag:8192 | tag:8192[256=u32:2] | "
             "literal:from a record | end ok");
    CHECK_EQ(opens, 1);
    CHECK_EQ(closes, 1);
    CHECK(lastOpen == key);
    CHECK(recordReleased);        // read once, released by CloseDBRecord
    CHECK_EQ(FakeRecord::alive, 0);
    CHECK_EQ(FakeRecord::reads, 1);
    CHECK(managerDeleted);        // cRZSysServPtr's destructor released the service
    gResolvedManager = nullptr;

    // OpenDBRecord failing: no close, no reads, no callbacks.
    fixture.Reset();
    managerDeleted = false;
    opens = 0;
    closes = 0;
    manager = new FakeManager();
    manager->open = false;
    manager->opens = &opens;
    manager->closes = &closes;
    manager->deletedFlag = &managerDeleted;
    gResolvedManager = manager;
    CHECK(!fixture.terp->ExecuteScriptResource(key));
    CHECK_EQ(opens, 1);
    CHECK_EQ(closes, 0);
    CHECK_EQ(fixture.target->Describe(), "");
    CHECK(managerDeleted);
    gResolvedManager = nullptr;

    // The record view without the sized-record interface (iid 0xc56b8f08): the script is not run and the
    // record is still closed and released — the original skips its own Release (0x10054f0d3) but
    // CloseDBRecord releases the caller's reference anyway.
    fixture.Reset();
    managerDeleted = false;
    recordReleased = false;
    opens = 0;
    closes = 0;
    const int readsBefore = FakeRecord::reads;
    manager = new FakeManager();
    manager->giveSerialView = false;
    manager->recordText = "<GZWinBtn>";
    manager->opens = &opens;
    manager->closes = &closes;
    manager->recordReleased = &recordReleased;
    manager->deletedFlag = &managerDeleted;
    gResolvedManager = manager;
    CHECK(!fixture.terp->ExecuteScriptResource(key));
    CHECK_EQ(fixture.target->Describe(), "");
    CHECK_EQ(opens, 1);
    CHECK_EQ(closes, 1);
    CHECK(recordReleased);
    CHECK_EQ(FakeRecord::alive, 0);
    CHECK_EQ(FakeRecord::reads, readsBefore);
    CHECK(managerDeleted);
    gResolvedManager = nullptr;
    SetRZServiceResolver(nullptr);
}

// ---------------------------------------------------------------------------------------------------------
// 11. The name/value state distinction and a cross-script state leak
// ---------------------------------------------------------------------------------------------------------
void TestPropertyStates() {
    Fixture fixture(Fixture::Words());
    fixture.Attach();
    // The same id twice: the pending value is flushed on the next name, so both are reported.
    CHECK_EQ(fixture.Run("<GZWinBtn id=1 id=2>"),
             "begin | begintag:8192 | prop:256=u32:1 | prop:256=u32:2 | endtag:8192 | "
             "tag:8192[256=u32:1,256=u32:2] | end ok");
    fixture.Reset();
    // A name with no '=' value: the next word is taken as the next name, and the nameless property is
    // flushed with a null value.
    CHECK_EQ(fixture.Run("<GZWinBtn id 5>"),
             "begin | begintag:8192 | prop:256=<null> | prop:5=<null> | endtag:8192 | "
             "tag:8192[256=<null>,5=<null>] | end ok");
    fixture.Reset();
    CHECK_EQ(fixture.Run("<GZWinBtn id>"),
             "begin | begintag:8192 | prop:256=<null> | endtag:8192 | tag:8192[256=<null>] | end ok");
    fixture.Reset();
    // '=' where a name is expected is an error (state 3 -> 10).
    CHECK(!fixture.RunScript("<GZWinBtn =1>"));
    CHECK_EQ(fixture.target->Describe(), "begin | begintag:8192 | end error");

    // Quirk: ClearParams does not reset the pending-name flag (+0x68), so a script that stops before the
    // value leaks that name into the next script, where the first property name flushes it as a null.
    fixture.Reset();
    CHECK(fixture.RunScript("<GZWinBtn 256"));
    CHECK_EQ(fixture.target->Describe(), "begin | begintag:8192 | end ok");
    fixture.Reset();
    CHECK(fixture.RunScript("<GZWinBtn 257=5>"));
    CHECK_EQ(fixture.target->Describe(),
             "begin | begintag:8192 | prop:256=<null> | prop:257=u32:5 | endtag:8192 | "
             "tag:8192[256=<null>,257=u32:5] | end ok");
}

}  // namespace

int main() {
    TestLifetime();
    TestContext();
    TestVocabularyStack();
    TestResolution();
    TestFullScript();
    TestUnknownNames();
    TestQuotedAndLiteral();
    TestReentrancy();
    TestScriptFile();
    TestScriptResource();
    TestPropertyStates();
    if (gFailures != 0) {
        std::printf("%d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("gz_scriptterp: all checks passed\n");
    return 0;
}

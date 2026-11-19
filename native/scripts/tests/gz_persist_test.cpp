#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "fake_resourcemanager.h"
#include "gz/PersistASCIIWriter.h"
#include "gz/PersistBinaryReader.h"
#include "gz/PersistBinaryWriter.h"
#include "gz/PersistSurveyorWriter.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZString.h"
#include "rz/cRZSysServPtr.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
// ---- a small object graph -------------------------------------------------------------------------------------
struct Res;

struct KeyHolder final : cIGZPersistResource {
    cGZPersistResourceKey key;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cGZPersistResourceKey GetKey() const override { return key; }
    bool SetKey(const cGZPersistResourceKey& k) override { key = k; return true; }
    std::uint32_t GetType() const override { return key.type; }
    std::uint32_t GetGroup() const override { return key.group; }
    std::uint32_t GetInstance() const override { return key.instance; }
    bool SetType(std::uint32_t v) override { key.type = v; return true; }
    bool SetGroup(std::uint32_t v) override { key.group = v; return true; }
    bool SetInstance(std::uint32_t v) override { key.instance = v; return true; }
    std::uint32_t GetRefCount() const override { return 1; }
};

cGZPersistTypeInfo Info(const char* name, std::uint32_t id, std::uint32_t version) {
    cGZPersistTypeInfo info;
    info.name = cRZAutoRefCount<cIGZString>(new cRZString(name));
    info.classId = id;
    info.version = version;
    return info;
}

struct Obj : cIGZPersistSerializableReferent {
    int refs = 0;
    cIGZPersistResource2* owner = nullptr;
    std::uint32_t classId = 0x501, value = 0;
    float f = 0;
    std::string text;
    cIGZPersistSerializableReferent* link = nullptr;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cIGZPersistSerializableReferent && iid != kGZIID_cIGZPersistSerializable2) return false;
        *out = static_cast<cIGZPersistSerializableReferent*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }   // the test owns the objects
    void Read(cIGZPersistReader& r) override {
        value = r.ReadUint32("value");
        f = r.ReadFloat32("f");
        cRZString s;
        r.ReadString("text", &s);
        text = s.Data();
        void* p = nullptr;
        r.ReadObjectRef("link", kGZIID_cIGZPersistSerializableReferent, &p);
        link = static_cast<cIGZPersistSerializableReferent*>(p);
        if (link) link->Release();
    }
    cGZPersistTypeInfo GetTypeInfo() const override { return Info("Obj", classId, 3); }
    void Write(cIGZPersistWriter& w) const override {
        w.WriteUint32("value", value);
        w.WriteFloat32("f", f);
        cRZString s(text.c_str());
        w.WriteString("text", &s);
        w.WriteObjectRef("link", link);
    }
    cIGZPersistResource2* GetResource() const override { return owner; }
};

struct Res final : cIGZPersistResource2 {
    int refs = 0;
    KeyHolder holder;
    std::vector<Obj*> objects;
    cIGZPersistSerializableReferent* first = nullptr;
    int slot9Calls = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cIGZPersistSerializableReferent && iid != kGZIID_cIGZPersistSerializable2) return false;
        *out = static_cast<cIGZPersistSerializableReferent*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void Read(cIGZPersistReader& r) override {
        void* p = nullptr;
        r.ReadObjectRef("first", kGZIID_cIGZPersistSerializableReferent, &p);
        first = static_cast<cIGZPersistSerializableReferent*>(p);
        if (first) first->Release();
    }
    cGZPersistTypeInfo GetTypeInfo() const override { return Info("Res", 0x500, 1); }
    void Write(cIGZPersistWriter& w) const override { w.WriteObjectRef("first", first); }
    cIGZPersistResource2* GetResource() const override { return const_cast<Res*>(this); }
    cIGZPersistResource* AsResource() override { return &holder; }
    const cIGZPersistResource* AsResource() const override { return &holder; }
    void Slot9() override { ++slot9Calls; }
    int IndexOfObject(const cIGZPersistSerializableReferent* object) const override {
        for (std::size_t i = 0; i < objects.size(); ++i) if (objects[i] == object) return static_cast<int>(i);
        return -1;
    }
};

// ---- services ---------------------------------------------------------------------------------------------------
struct Host final : IPersistHost {
    std::map<std::uint32_t, std::function<cIGZPersistSerializableReferent*()>> factories;
    std::vector<std::string> diagnostics;
    int surveyors = 0;
    bool GetCOMObject(std::uint32_t classId, std::uint32_t iid, void** out) override {
        if (classId == kGZCLSID_cGZPersistSurveyorWriter) {
            ++surveyors;
            auto* w = new cGZPersistSurveyorWriter();
            w->AddRef();
            const bool ok = w->QueryInterface(iid, out);
            w->Release();
            return ok;
        }
        auto it = factories.find(classId);
        if (it == factories.end()) return false;
        cIGZPersistSerializableReferent* object = it->second();
        return object->QueryInterface(iid, out);   // one reference for the caller
    }
    void Diagnostic(const char* text) override { diagnostics.push_back(text); }
};

struct Manager final : FakeResourceManager {
    std::map<std::uint32_t, cIGZPersistResource2*> loadable;     // by instance
    std::vector<std::string> log;
    bool registered = false;
    bool HasRegisteredResource(const cGZPersistResourceKey&) override { log.push_back("has"); return registered; }
    void RegisterResource(const cGZPersistResourceKey& key, cIGZPersistResource&) override { log.push_back("register " + std::to_string(key.instance)); }
    bool UnregisterResource(const cGZPersistResourceKey& key) override { log.push_back("unregister " + std::to_string(key.instance)); return true; }
    bool GetResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t, cIGZUnknown*) override {
        log.push_back("get " + std::to_string(key.instance) + " iid " + std::to_string(iid));
        auto it = loadable.find(key.instance);
        if (it == loadable.end()) return false;
        it->second->AddRef();
        *out = it->second;
        return true;
    }
};
Manager* g_manager = nullptr;
bool Resolve(std::uint32_t service, std::uint32_t iid, void** out) {
    if (service != 0x056b906e || iid != 0x656b8efc) return false;
    *out = static_cast<cIGZPersistResourceManager*>(g_manager);
    return true;
}

// ---- a stream that renders numbers as text (what the ASCII writer expects) -----------------------------------------
struct TextOut final : cIGZOStream {
    std::string text;
    int flushes = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void Flush() override { ++flushes; }
    bool SetSint8(std::int8_t v) override { text += std::to_string(v); return true; }
    bool SetUint8(std::uint8_t v) override { text += std::to_string(v); return true; }
    bool SetSint16(std::int16_t v) override { text += std::to_string(v); return true; }
    bool SetUint16(std::uint16_t v) override { text += std::to_string(v); return true; }
    bool SetSint32(std::int32_t v) override { text += std::to_string(v); return true; }
    bool SetUint32(std::uint32_t v) override { text += std::to_string(v); return true; }
    bool SetSint64(std::int64_t v) override { text += std::to_string(v); return true; }
    bool SetUint64(std::uint64_t v) override { text += std::to_string(v); return true; }
    bool SetFloat32(float v) override { text += std::to_string(int(v * 100)) + "c"; return true; }
    bool SetFloat64(double v) override { text += std::to_string(int(v * 100)) + "c"; return true; }
    bool SetRZCharStr(const char* v) override { text += v; return true; }
    bool SetGZStr(const cIGZString& v) override { text += v.Data(); return true; }
    bool SetGZSerializable(const cIGZSerializable&) override { return false; }
    bool SetVoid(const void* p, std::uint32_t n) override { text.append(static_cast<const char*>(p), n); return true; }
    bool GetError() const override { return false; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

// Streams are reference counted and the writers / readers release what they were given: keep them on the heap with one
// reference of the test's own.
template <class T, class... Args> T* Heap(Args&&... args) {
    T* t = new T(std::forward<Args>(args)...);
    t->AddRef();
    return t;
}
std::uint32_t U32(const std::vector<unsigned char>& b, std::size_t at) {
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | std::uint32_t(b[at + 3]) << 24;
}
}  // namespace

int main() {
    Host host;
    Manager manager;
    g_manager = &manager;
    SetPersistHost(&host);
    SetRZServiceResolver(&Resolve);

    // Graph: root R { A -> B -> D1 }, D1 lives in the dependent resource D.
    Res r, d;
    r.holder.key = {0x10, 1, 2};
    d.holder.key = {0x11, 3, 4};
    Obj a, b, d1;
    a.owner = b.owner = &r;
    d1.owner = &d;
    a.classId = 0x501; b.classId = 0x502; d1.classId = 0x503;
    a.value = 7; a.f = 1.5f; a.text = "alpha"; a.link = &b;
    b.value = 9; b.f = 0.25f; b.text = "beta"; b.link = &d1;
    d1.value = 11; d1.text = "gamma";
    d.objects = {&d1};
    r.objects = {&a, &b};
    r.first = &a;

    // ---- the surveyor
    {
        auto* s = new cGZPersistSurveyorWriter();
        s->AddRef();
        void* raw = nullptr;
        CHECK(s->QueryInterface(kGZIID_cIGZPersistSurveyorWriter, &raw) && raw == static_cast<cIGZPersistSurveyorWriter*>(s)); s->Release();
        CHECK(s->QueryInterface(kGZIID_cIGZPersistWriter, &raw)); s->Release();
        CHECK(!s->QueryInterface(0x1234, &raw) && !s->QueryInterface(1, nullptr));
        cIGZOStream* nullStream = s->OutputStream();
        CHECK(nullStream != nullptr && nullStream->SetUint32(5) && s->OutputStream() == nullStream);   // the shared null stream
        s->WriteRoot(&r, nullptr);
        CHECK(s->Resource() == &r && s->NumObjectRefs() == 3 && s->NumDependentResources() == 1);
        CHECK(s->ObjectRef(0) == static_cast<cIGZPersistSerializableReferent*>(&r) && s->ObjectRef(1) == &a && s->ObjectRef(2) == &b);
        CHECK(s->IndexOfObjectRef(&b) == 2 && s->IndexOfObjectRef(&d1) == -1 && s->DependentResource(0) == &d);
        CHECK(s->IndexOfDependentResource(&d) == 0 && s->IndexOfDependentResource(&r) == -1);
        s->WriteBool("x", true); s->WriteUint32("x", 3); s->WriteString("x", "s"); s->WriteTypeInfo(Info("T", 1, 1));   // no effect
        s->WriteObject("o", nullptr);
        s->WriteObjectRef("n", static_cast<const cIGZPersistSerializableReferent*>(nullptr));
        CHECK(s->NumObjectRefs() == 3);
        s->WriteObjectRef("via-unknown", static_cast<const cIGZUnknown*>(static_cast<cIGZPersistSerializableReferent*>(&a)));
        s->WriteObject("a", &a);                                                  // writes a's contents: link B is already known
        s->Clear();
        CHECK(s->Resource() == nullptr && s->NumObjectRefs() == 0 && s->NumDependentResources() == 0);
        auto* sink = Heap<PortMemoryOStream>();
        s->WriteRoot(&r, sink);
        CHECK(s->OutputStream() == sink);
        CHECK(r.refs == 2 && a.refs == 1);                                        // the surveyor holds references
        s->Release();
        CHECK(r.refs == 0 && a.refs == 0 && b.refs == 0 && d.refs == 0);
        sink->Release();
    }

    // ---- the binary writer
    auto* streamPtr = Heap<PortMemoryOStream>();
    PortMemoryOStream& stream = *streamPtr;
    {
        auto* w = new cGZPersistBinaryWriter();
        w->AddRef();
        void* raw = nullptr;
        CHECK(w->QueryInterface(kGZIID_cIGZPersistWriter, &raw) && raw == static_cast<cIGZPersistWriter*>(w)); w->Release();
        CHECK(!w->QueryInterface(kGZIID_cIGZPersistSurveyorWriter, &raw));
        w->WriteRoot(&r, streamPtr);
        CHECK(host.surveyors >= 1 && d.slot9Calls == 1 && r.slot9Calls == 0);
        w->Release();
    }
    const auto& bytes = stream.bytes();
    CHECK(U32(bytes, 0) == 1 && U32(bytes, 4) == 3 && U32(bytes, 8) == 4 && U32(bytes, 12) == 0x11);   // one dependent: group, instance, type
    CHECK(U32(bytes, 16) == 3 && U32(bytes, 20) == 0x500 && U32(bytes, 24) == 0x501 && U32(bytes, 28) == 0x502);   // three objects
    CHECK(bytes[32] == 1 && bytes[33] == 0 && U32(bytes, 34) == 1);              // R.first -> object 1 of this resource
    CHECK(U32(bytes, 38) == 7);                                                  // A.value follows

    // ---- reading it back
    {
        Res r2, d2;
        r2.holder.key = {0x10, 1, 2};
        d2.holder.key = {0x11, 3, 4};
        manager.loadable[4] = &d2;
        Obj a2, b2;
        host.factories[0x501] = [&] { return static_cast<cIGZPersistSerializableReferent*>(&a2); };
        host.factories[0x502] = [&] { return static_cast<cIGZPersistSerializableReferent*>(&b2); };
        auto* inPtr = Heap<PortMemoryIStream>(bytes.data(), bytes.size());
        PortMemoryIStream& in = *inPtr;
        auto* rd = new cGZPersistBinaryReader();
        rd->AddRef();
        void* raw = nullptr;
        CHECK(rd->QueryInterface(kGZIID_cIGZPersistReader, &raw) && raw == static_cast<cIGZPersistReader*>(rd)); rd->Release();
        rd->ReadRoot(&r2, inPtr);
        CHECK(manager.log.size() >= 4 && manager.log[0] == "has" && manager.log[1] == "register 2");
        CHECK(manager.log.back() == "unregister 2");
        bool asked = false;
        for (const auto& l : manager.log) if (l == "get 4 iid " + std::to_string(kGZIID_cIGZPersistResource2Loaded)) asked = true;
        CHECK(asked);
        CHECK(r2.first == static_cast<cIGZPersistSerializableReferent*>(&a2));
        CHECK(a2.value == 7 && a2.f == 1.5f && a2.text == "alpha" && a2.link == static_cast<cIGZPersistSerializableReferent*>(&b2));
        CHECK(b2.value == 9 && b2.f == 0.25f && b2.text == "beta");
        CHECK(b2.link == static_cast<cIGZPersistSerializableReferent*>(&d2));       // kept quirk: a reference into another resource resolves to the resource
        CHECK(in.RemainingBytes() == 0);
        CHECK(host.diagnostics.empty());
        // a resource the manager already knows is not registered or unregistered again
        manager.log.clear();
        manager.registered = true;
        auto* again = Heap<PortMemoryIStream>(bytes.data(), bytes.size());
        rd->ReadRoot(&r2, again);
        CHECK(manager.log[0] == "has" && manager.log.back() != "unregister 2" && manager.log[1] != "register 2");
        // a dependent that cannot be loaded leaves a null slot and a message
        manager.loadable.clear();
        auto* missing = Heap<PortMemoryIStream>(bytes.data(), bytes.size());
        rd->ReadRoot(&r2, missing);
        CHECK(host.diagnostics.size() == 1 && host.diagnostics[0].find("Cannot load dependent resource") == 0);
        CHECK(b2.link == nullptr);
        rd->Release();
        inPtr->Release();
        again->Release();
        missing->Release();
    }

    // ---- scalar / array / string / object API of the binary writer and reader
    {
        Res solo;
        solo.holder.key = {0x10, 1, 2};
        struct Probe final : Obj {
            void Write(cIGZPersistWriter& w) const override {
                w.WriteBool("b", true); w.WriteSint8("s8", -5); w.WriteUint8("u8", 250); w.WriteSint16("s16", -300); w.WriteUint16("u16", 60000);
                w.WriteSint32("s32", -70000); w.WriteUint32("u32", 4000000000u); w.WriteFloat32("f32", 2.5f); w.WriteFloat64("f64", -0.125);
                const bool bools[3] = {true, false, true};
                const std::int8_t i8[2] = {-1, 2};
                const std::uint8_t u8[2] = {3, 4};
                const std::int16_t i16[2] = {-6, 7};
                const std::uint16_t u16[2] = {8, 9};
                const std::int32_t i32[2] = {-10, 11};
                const std::uint32_t u32[2] = {12, 13};
                const float f32[2] = {1.0f, 2.0f};
                const double f64[2] = {3.0, 4.0};
                w.WriteBoolArray("ba", bools, 3); w.WriteSint8Array("i8", i8, 2); w.WriteUint8Array("u8a", u8, 2); w.WriteSint16Array("i16", i16, 2);
                w.WriteUint16Array("u16a", u16, 2); w.WriteSint32Array("i32", i32, 2); w.WriteUint32Array("u32a", u32, 2);
                w.WriteFloat32Array("f32a", f32, 2); w.WriteFloat64Array("f64a", f64, 2);
                cRZString s("hello");
                w.WriteString("gz", &s); w.WriteString("c", "world");
                w.WriteObject("null", nullptr);
                w.WriteTypeInfo(Info("Named", 0x1234, 6));
            }
            void Read(cIGZPersistReader& r) override {
                CHECK(r.ReadBool("b") && r.ReadSint8("s8") == -5 && r.ReadUint8("u8") == 250 && r.ReadSint16("s16") == -300 && r.ReadUint16("u16") == 60000);
                CHECK(r.ReadSint32("s32") == -70000 && r.ReadUint32("u32") == 4000000000u && r.ReadFloat32("f32") == 2.5f && r.ReadFloat64("f64") == -0.125);
                bool bools[3]; std::int8_t i8[2]; std::uint8_t u8[2]; std::int16_t i16[2]; std::uint16_t u16[2]; std::int32_t i32[2]; std::uint32_t u32[2]; float f32[2]; double f64[2];
                r.ReadBoolArray("ba", bools, 3); r.ReadSint8Array("i8", i8, 2); r.ReadUint8Array("u8a", u8, 2); r.ReadSint16Array("i16", i16, 2);
                r.ReadUint16Array("u16a", u16, 2); r.ReadSint32Array("i32", i32, 2); r.ReadUint32Array("u32a", u32, 2);
                r.ReadFloat32Array("f32a", f32, 2); r.ReadFloat64Array("f64a", f64, 2);
                CHECK(bools[0] && !bools[1] && bools[2] && i8[0] == -1 && i8[1] == 2 && u8[1] == 4 && i16[0] == -6 && u16[1] == 9);
                CHECK(i32[0] == -10 && u32[1] == 13 && f32[1] == 2.0f && f64[0] == 3.0);
                cRZString s, c;
                r.ReadString("gz", &s);
                CHECK(std::string(s.Data()) == "hello");
                char buffer[16] = {};
                r.InputStream()->GetRZCharStr(buffer, 15);
                CHECK(std::string(buffer) == "world");
                void* none = reinterpret_cast<void*>(1);
                r.ReadObject("null", kGZIID_cIGZPersistSerializable2, &none);
                CHECK(none == nullptr);
                const cGZPersistTypeInfo info = r.ReadTypeInfo();
                CHECK(std::string(info.name->Data()) == "Named" && info.classId == 0x1234 && info.version == 6);
                r.SkipBytes("skip", 0);
            }
        } probe;
        probe.owner = &solo;
        solo.objects = {&probe};
        solo.first = nullptr;
        // Resource R has first == null: object table [R, probe]? the probe is only reachable through a reference
        solo.first = &probe;
        host.factories[0x501] = [&] { return static_cast<cIGZPersistSerializableReferent*>(&probe); };
        auto* outPtr = Heap<PortMemoryOStream>();
        PortMemoryOStream& out = *outPtr;
        auto* w = new cGZPersistBinaryWriter();
        w->AddRef();
        w->WriteRoot(&solo, outPtr);
        w->Release();
        manager.registered = false;
        auto* inPtr2 = Heap<PortMemoryIStream>(out.bytes().data(), out.bytes().size());
        PortMemoryIStream& in = *inPtr2;
        auto* rd = new cGZPersistBinaryReader();
        rd->AddRef();
        rd->ReadRoot(&solo, inPtr2);
        CHECK(in.RemainingBytes() == 0);
        rd->Release();
        inPtr2->Release();
        outPtr->Release();
    }

    // ---- an object written through WriteObject, and one that is null
    {
        struct Holder final : Obj {
            void Write(cIGZPersistWriter& w) const override { w.WriteObject("a", this); w.WriteObject("none", nullptr); }
        } holder;
        auto* out = Heap<PortMemoryOStream>();
        auto* s = new cGZPersistSurveyorWriter();
        s->AddRef();
        auto* w = new cGZPersistBinaryWriter();
        w->AddRef();
        Res root;
        root.holder.key = {1, 2, 3};
        root.first = nullptr;
        w->WriteRoot(&root, out);
        CHECK(out->bytes().size() > 0);
        w->Release();
        s->Release();
        out->Release();
    }

    // ---- the ASCII writer
    {
        TextOut text;
        auto* w = new cGZPersistASCIIWriter();
        w->AddRef();
        void* raw = nullptr;
        CHECK(w->QueryInterface(kGZIID_cIGZPersistWriter, &raw) && raw == static_cast<cIGZPersistWriter*>(w)); w->Release();
        w->WriteRoot(&r, &text);
        const std::string expected =
            "Resource Res G:0x00000001 I:0x00000002 T:0x00000010\n"
            "   Dep. Resource #0 Res G:0x00000003 I:0x00000004 T:0x00000011\n"
            "   Object #0 Res\n"
            "   Object #1 Obj\n"
            "   Object #2 Obj\n"
            "\n"
            "   Object #0 Res\n"
            "   {\n"
            "      cIGZPersistSerializableReferent* first = Object #1 Obj GZCLSID: 0x00000501\n"
            "   }\n"
            "\n"
            "   Object #1 Obj\n"
            "   {\n"
            "      Uint32 value = 7\n"
            "      Float32 f = 150c\n"
            "      RZString text = alpha\n"
            "      cIGZPersistSerializableReferent* link = Object #2 Obj GZCLSID: 0x00000502\n"
            "   }\n"
            "\n"
            "   Object #2 Obj\n"
            "   {\n"
            "      Uint32 value = 9\n"
            "      Float32 f = 25c\n"
            "      RZString text = beta\n"
            "      cIGZPersistSerializableReferent* link = Object ID 0 Obj in dep. resource #0 G:0x00000003 I:0x00000004 T:0x00000011\n"
            "   }\n"
            "\n"
            "\n";
        if (text.text != expected) std::printf("--- got ---\n%s--- want ---\n%s", text.text.c_str(), expected.c_str());
        CHECK(text.text == expected);
        CHECK(text.flushes == 1 && d.slot9Calls == 2);
        w->Release();

        // scalars and arrays
        TextOut t2;
        auto* w2 = new cGZPersistASCIIWriter();
        w2->AddRef();
        Res plain;
        plain.holder.key = {5, 6, 7};
        Obj* prober = nullptr;
        (void)prober;
        struct Prober final : Obj {
            void Write(cIGZPersistWriter& wr) const override {
                wr.WriteBool("flag", false); wr.WriteSint8("s8", -1); wr.WriteUint8("u8", 2); wr.WriteSint16("s16", -3); wr.WriteUint16("u16", 4);
                wr.WriteSint32("s32", -5); wr.WriteUint32("u32", 6); wr.WriteFloat32("f32", 0.5f); wr.WriteFloat64("f64", 0.25);
                const bool bs[2] = {true, false};
                const std::int8_t i8[2] = {1, -2};
                const std::uint32_t u32[3] = {1, 2, 3};
                wr.WriteBoolArray("bs", bs, 2); wr.WriteSint8Array("i8", i8, 2); wr.WriteUint32Array("u32a", u32, 3);
                cRZString s("txt");
                wr.WriteString("gz", &s); wr.WriteString("raw", "chars");
                wr.WriteObject("none", nullptr);
                wr.WriteObjectRef("nul", static_cast<const cIGZPersistSerializableReferent*>(nullptr));
                wr.WriteTypeInfo(Info("Named", 0x1234, 6));
            }
        } pr;
        pr.owner = &plain;
        plain.first = &pr;
        plain.objects = {&pr};
        w2->WriteRoot(&plain, &t2);
        CHECK(t2.text.find("      bool flag = false\n") != std::string::npos);
        CHECK(t2.text.find("      Sint8 s8 = -1\n") != std::string::npos && t2.text.find("      Uint16 u16 = 4\n") != std::string::npos);
        CHECK(t2.text.find("      Float64 f64 = 25c\n") != std::string::npos);
        CHECK(t2.text.find("      bool bs[2] = true, false, \n") != std::string::npos);
        CHECK(t2.text.find("      Sint8 i8[2] = 1, -2, \n") != std::string::npos);
        CHECK(t2.text.find("      Uint32 u32a[3] = 1, 2, 3, \n") != std::string::npos);
        CHECK(t2.text.find("      RZString gz = txt\n") != std::string::npos && t2.text.find("      RZChar* raw = chars\n") != std::string::npos);
        CHECK(t2.text.find("      Object GZCLSID: 0x00000000\n") != std::string::npos);
        CHECK(t2.text.find("      cIGZPersistSerializableReferent* nul = 0\n") != std::string::npos);
        CHECK(t2.text.find("      TypeName: Named GZCLSID: 0x00001234 Version: 6\n") != std::string::npos);
        w2->Release();
    }

    streamPtr->Release();
    SetPersistHost(nullptr);
    SetRZServiceResolver(nullptr);
    std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "rz/cRZString.h"
#include "sc4/DBSegment.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {
constexpr std::uint32_t kNodeClass = 0xaaaa0001;
constexpr std::uint32_t kLeafClass = 0xaaaa0002;

// A serializable with a value and an optional child reference written through the class-object layer.
class Node : public cIGZSerializable {
public:
    explicit Node(std::uint32_t classId) : classId_(classId) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZSerializable) {
            *out = static_cast<cIGZSerializable*>(this);
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        delete this;
        return 0;
    }
    bool Write(cIGZOStream& s) const override {
        s.SetUint32(value);
        s.SetFloat32(weight);
        return SC4WriteGZSerializable(s, child) && !s.GetError();
    }
    bool Read(cIGZIStream& s) override {
        s.GetUint32(value);
        s.GetFloat32(weight);
        void* p = nullptr;
        if (!SC4ReadGZSerializable(s, kGZIID_cIGZSerializable, &p, false)) return false;
        child = static_cast<Node*>(static_cast<cIGZSerializable*>(p));
        return !s.GetError();
    }
    std::uint32_t GetGZCLSID() const override { return classId_; }

    std::uint32_t value = 0;
    float weight = 0;
    Node* child = nullptr;   // borrowed
private:
    std::uint32_t classId_;
    std::uint32_t refs_ = 0;
};

std::uint32_t g_created = 0;
class Factory : public ISerializableFactory {
public:
    cIGZSerializable* CreateSerializable(std::uint32_t classId) override {
        if (classId != kNodeClass && classId != kLeafClass) return nullptr;
        ++g_created;
        auto* n = new Node(classId);
        n->AddRef();
        return n;
    }
};
}  // namespace

int main() {
    Factory factory;
    SetSerializableFactory(&factory);
    std::string path = "/tmp/sc4_dbsegment_test_" + std::to_string(getpid()) + ".sc4";
    const cGZPersistResourceKey rootKey{0x10101010, 0x20202020, 7};

    std::uint32_t leafId = 0;
    {
        cSC4DBSegment* seg = new cSC4DBSegment();
        seg->AddRef();
        CHECK(seg->Init());
        cRZString p(path.c_str());
        CHECK(seg->SetPath(p));
        CHECK(seg->Open(false, true));
        CHECK(seg->GetCOMSerializer()->GetMode() == 2);

        Node leaf(kLeafClass);
        leaf.AddRef();
        leaf.value = 0xbeef;
        leaf.weight = 0.5f;
        Node root(kNodeClass);
        root.AddRef();
        root.value = 1234;
        root.weight = 2.25f;
        root.child = &leaf;
        leafId = leaf.value;

        cISC4DBSegmentOStream* os = nullptr;
        CHECK(seg->OpenOStream(rootKey, &os, true));
        os->SetUint32(0x5ca1ab1e);
        CHECK(SC4WriteGZSerializable(*os, &root));
        CHECK(seg->CloseOStream(os));
        os->Release();
        CHECK(seg->Close());
        seg->Release();
    }
    CHECK(access(path.c_str(), F_OK) == 0);

    {
        cSC4DBSegment* seg = new cSC4DBSegment();
        seg->AddRef();
        seg->Init();
        cRZString p(path.c_str());
        seg->SetPath(p);
        CHECK(seg->Open(true, false));
        CHECK(seg->GetCOMSerializer()->GetMode() == 1);

        CHECK(seg->LoadClassObjects(0x48));   // class table + every class's objects, created via the factory
        CHECK(g_created == 2);
        cISC4DBSegmentIStream* is = nullptr;
        CHECK(seg->OpenIStream(rootKey, &is));
        std::uint32_t magic = 0;
        is->GetUint32(magic);
        CHECK(magic == 0x5ca1ab1e);
        void* raw = nullptr;
        CHECK(SC4ReadGZSerializable(*is, kGZIID_cIGZSerializable, &raw, false));
        auto* root = static_cast<Node*>(static_cast<cIGZSerializable*>(raw));
        CHECK(root && root->GetGZCLSID() == kNodeClass);
        CHECK(seg->CloseIStream(is));
        is->Release();

        CHECK(root->value == 1234 && root->weight == 2.25f);
        CHECK(root->child && root->child->GetGZCLSID() == kLeafClass);
        CHECK(root->child->value == leafId && root->child->weight == 0.5f);
        CHECK(g_created == 2);   // references resolved to the already-created objects
        CHECK(!seg->LoadClassObjects(0x48));  // only once
        if (root && root->child) root->child->Release();   // Read() took a reference to the child
        if (root) root->Release();
        CHECK(seg->Close());
        seg->Release();
    }
    unlink(path.c_str());
    std::printf(failures ? "FAILED (%d)\n" : "PASS sc4_dbsegment\n", failures);
    return failures != 0;
}

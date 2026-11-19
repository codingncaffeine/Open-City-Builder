// cSC4TextDecalOccupant: decal geometry, overlay bookkeeping, flag gating, message handling and the version-3 record.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/TextDecalOccupant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct Refd : cIGZUnknown {};

struct Texture final : ITextTexture {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void GetSize(std::int32_t* wh) override { wh[0] = 200; wh[1] = 100; }
};

struct Overlay final : ITextDecalOverlay {
    int refs = 1, nextId = 7;
    std::vector<std::string> log;
    float lastWidth = 0, lastTurns = 0;
    std::map<int, float> alpha;
    std::map<int, float> color0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::int32_t AddDecal(float width, float turns, ITextTexture*, const float*) override {
        lastWidth = width; lastTurns = turns; log.push_back("add"); return nextId++;
    }
    void MoveDecal(std::int32_t, const float* p) override { log.push_back("move"); lastWidth = p[0]; }
    void RemoveDecal(std::int32_t) override { log.push_back("remove"); }
    void SetDecalAlpha(std::int32_t id, float a) override { log.push_back("alpha"); alpha[id] = a; }
    void SetDecalColor(std::int32_t id, const float* c) override { log.push_back("color"); color0[id] = c[0]; }
};

struct ViewSet final : ITextDecalViewSet {
    int refs = 1;
    bool contains = true;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool ContainsOccupant(cISC4Occupant*) override { return contains; }
};

struct Server final : cIGZMessageServer2 {
    std::vector<std::pair<char, std::uint32_t>> notes;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 2; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t t) override { notes.push_back({'+', t}); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t t) override { notes.push_back({'-', t}); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct Host final : ITextDecalHost {
    Server server;
    Overlay overlays[4];
    ViewSet view;
    Texture* lastTexture = nullptr;
    std::vector<Texture*> made;                 // every texture handed out; the test frees them at the end
    bool allow = true;
    int rotation = 1;
    std::vector<std::uint32_t> overlayRequests;
    int textures = 0;
    cIGZMessageServer2* Messages() override { return &server; }
    cISC4OccupantManager* OccupantManager() override { return nullptr; }
    ITextDecalViewSet* AcquireViewSet() override { view.AddRef(); return &view; }
    bool PropDeveloperAllowsDecals() override { return allow; }
    ITextDecalOverlay* OverlayManager(std::uint32_t i) override { overlayRequests.push_back(i); return &overlays[i]; }
    bool CreateTextTexture(const cRZString&, ITextTexture*& out) override { out = lastTexture = new Texture; made.push_back(lastTexture); ++textures; return true; }
    float TerrainHeight(float x, float) override { return 50.0f + x; }
    int ViewRotation() override { return rotation; }
    void PositionToCell(float, float, int& a, int& b) override { a = b = 0; }
};

void Count(Overlay& o, const char* what, int n) {
    int c = 0;
    for (auto& s : o.log) c += s == what;
    if (c != n) { std::printf("FAIL: overlay log count of %s is %d, expected %d\n", what, c, n); ++failures; }
}
}  // namespace

int main() {
    Host host;
    SetTextDecalHost(&host);
    auto* d = new cSC4TextDecalOccupant();
    d->AddRef();

    // defaults and identity
    CHECK(d->GetGZCLSID() == kGZCLSID_cSC4TextDecalOccupant && d->TextHeight() == 1.0f && d->Alpha() == 1.0f);
    CHECK(d->Orientation() == 1 && !d->Dynamic() && d->DecalIdA() == -1 && d->DecalIdB() == -1);
    CHECK(d->Color()[0] == 255.0f && d->Color()[2] == 255.0f && d->DecalWidth() == 100.0f);
    float lo[3], hi[3];
    d->GetBoundingBox(lo, hi);
    CHECK(hi[0] == 0.1f && hi[1] == 0.1f && hi[2] == 0.1f && lo[0] == 0.0f);
    void* iface = nullptr;
    CHECK(d->QueryInterface(kGZIID_cISC4TextDecalOccupant, &iface) && iface == static_cast<cISC4TextDecalOccupant*>(d));
    static_cast<cISC4TextDecalOccupant*>(iface)->Release();
    CHECK(static_cast<cISC4TextDecalOccupant*>(d)->AsOccupant() == static_cast<cISC4Occupant*>(d));

    // Init: prop type, overlay managers 0 (A static) and 1 (B static), flag 2 granted, three notifications
    CHECK(d->Init());
    CHECK(d->GetPropType() == kPropTypeTextDecal);
    CHECK(host.overlayRequests.size() >= 2 && host.overlayRequests[0] == 1 && host.overlayRequests[1] == 0);
    CHECK(d->IsFlagSet(kTextDecalFlagShown));
    CHECK(host.server.notes.size() == 3 && host.server.notes[0].second == 0xa6b79602 && host.server.notes[2].second == 0x26d31ec1);
    CHECK(host.view.refs == 1);                 // the acquired reference was released

    // text -> texture -> decals: width = 200/100 * 1 * 16 = 32; orientation 1 keeps the extents
    cRZString text("Main St");
    d->SetText(text);
    CHECK(host.textures == 1 && d->Texture() == host.lastTexture);
    CHECK(d->DecalWidth() == 32.0f);
    CHECK(d->DecalIdA() == 7 && d->DecalIdB() == 7);
    CHECK(host.overlays[0].lastWidth == 32.0f && host.overlays[1].lastTurns == 0.0f);   // table[1][1] = 0
    d->GetBoundingBox(lo, hi);
    CHECK(lo[0] == -16.0f && hi[0] == 16.0f && lo[2] == -8.0f && hi[2] == 8.0f);
    CHECK(lo[1] == 50.0f - 0.1f && hi[1] == 50.0f + 0.1f);
    Count(host.overlays[0], "color", 1);
    Count(host.overlays[0], "alpha", 1);
    cRZString got;
    d->GetText(got);
    CHECK(got.IsEqualToStringRef(text, true));

    // orientation 0 swaps the extents and picks table[rotation 2][0] = 0.75
    host.rotation = 2;
    d->SetOrientation(0);
    d->GetBoundingBox(lo, hi);
    CHECK(lo[0] == -8.0f && hi[2] == 16.0f);
    CHECK(host.overlays[1].lastTurns == 0.75f);
    CHECK(host.overlays[0].lastTurns == 0.75f);   // the first manager is turned the same (i386 0x004c73f4)
    CHECK(d->DecalIdA() == 8);
    Count(host.overlays[0], "remove", 1);
    d->SetOrientation(0);                       // no change: nothing rebuilt
    CHECK(d->DecalIdA() == 8);
    d->SetOrientation(1);

    // height limits
    d->SetTextHeight(8.0f);  CHECK(d->TextHeight() == 1.0f);
    d->SetTextHeight(0.0f);  CHECK(d->TextHeight() == 1.0f);
    d->SetTextHeight(2.0f);  CHECK(d->TextHeight() == 2.0f && d->DecalWidth() == 64.0f);
    d->SetTextHeight(1.0f);

    // static colour / alpha rebuild the decal; tiny colour changes are ignored
    const int idBefore = d->DecalIdA();
    const float tiny[3] = {255.00005f, 255.0f, 255.0f};
    d->SetTextColor(tiny);
    CHECK(d->DecalIdA() == idBefore && d->Color()[0] == 255.0f);
    const float red[3] = {255.0f, 0.0f, 0.0f};
    d->SetTextColor(red);
    CHECK(d->DecalIdA() == idBefore + 1 && d->Color()[1] == 0.0f);
    d->SetAlpha(0.5f);
    CHECK(d->DecalIdA() == idBefore + 2 && host.overlays[0].alpha[d->DecalIdA()] == 0.5f);

    // position: static rebuilds around the new centre, bbox follows
    const float where[3] = {100.0f, 5.0f, 40.0f};
    d->SetPosition(where);
    CHECK(d->PositionX() == 100.0f && d->PositionZ() == 40.0f);
    d->GetBoundingBox(lo, hi);
    CHECK(lo[0] == 84.0f && hi[2] == 48.0f && lo[1] == 150.0f - 0.1f);

    // dynamic: new overlay managers (indices 2 and 3), moves/recolours do not rebuild
    host.overlayRequests.clear();
    d->SetDynamic(true);
    CHECK(d->Dynamic() && host.overlayRequests.size() == 2 && host.overlayRequests[0] == 3 && host.overlayRequests[1] == 2);
    const int dynId = d->DecalIdA();
    CHECK(dynId != -1);
    const float where2[3] = {1.0f, 0.0f, 2.0f};
    d->SetPosition(where2);
    CHECK(d->DecalIdA() == dynId);
    Count(host.overlays[2], "move", 1);
    Count(host.overlays[3], "move", 1);
    d->SetAlpha(0.25f);
    CHECK(d->DecalIdA() == dynId && host.overlays[2].alpha[dynId] == 0.25f);
    d->SetTextColor(tiny);                       // from red: differs
    CHECK(d->DecalIdA() == dynId && host.overlays[2].color0[dynId] == 255.00005f);
    d->SetDynamic(false);

    // messages
    cRZMessage2Standard msg;
    msg.SetType(kMessageTextDecalRefresh);
    const int idRefresh = d->DecalIdA();
    d->DoMessage(&msg);
    CHECK(d->DecalIdA() != -1 && d->DecalIdA() != idRefresh);
    ViewSet other;
    other.contains = false;
    ITextDecalViewSet* otherPtr = &other;
    msg.SetType(kMessageTextDecalVisibility);
    msg.SetVoid2(&otherPtr);
    d->DoMessage(&msg);
    CHECK(!d->IsFlagSet(kTextDecalFlagShown) && d->DecalIdA() == -1 && d->DecalIdB() == -1);
    other.contains = true;
    d->DoMessage(&msg);
    CHECK(d->IsFlagSet(kTextDecalFlagShown) && d->DecalIdA() != -1);
    host.allow = false;                          // the developer refuses: the flag is cleared
    d->DoMessage(&msg);
    CHECK(!d->IsFlagSet(kTextDecalFlagShown));
    // setting the flag directly is refused as well
    d->SetFlag(kTextDecalFlagShown);
    CHECK(!d->IsFlagSet(kTextDecalFlagShown));
    host.allow = true;
    d->SetFlag(kTextDecalFlagShown);
    CHECK(d->IsFlagSet(kTextDecalFlagShown) && d->DecalIdA() != -1);
    msg.SetType(kMessageTextDecalViewChanged);
    d->DoMessage(&msg);
    CHECK(d->IsFlagSet(kTextDecalFlagShown));

    // record round trip (version 3)
    const float c2[3] = {10.0f, 20.0f, 30.0f};
    d->SetTextColor(c2);
    d->SetAlpha(0.75f);
    d->SetOrientation(3);
    PortMemoryOStream os;
    CHECK(d->Write(os));
    const auto& bytes = os.bytes();
    CHECK(bytes[0] == 3 && bytes[1] == 0);
    auto* r = new cSC4TextDecalOccupant();
    r->AddRef();
    PortMemoryIStream is(bytes.data(), bytes.size());
    CHECK(r->Read(is));
    CHECK(r->Text().IsEqualToStringRef(text, true));
    CHECK(r->TextHeight() == 1.0f && r->DecalWidth() == d->DecalWidth() && r->PositionX() == 1.0f && r->PositionZ() == 2.0f);
    CHECK(r->Color()[0] == 10.0f && r->Color()[1] == 20.0f && r->Color()[2] == 30.0f && r->Alpha() == 0.75f);
    CHECK(r->Orientation() == 3 && r->GetPropType() == kPropTypeTextDecal);

    // older record: version 1 stores an integer height and an extra u32 before the position
    {
        PortMemoryOStream o1;
        o1.SetUint16(1);
        cSC4PropOccupant prop;
        prop.SetPropType(kPropTypeTextDecal);
        // the prop record writes its own version first; reuse the real one by writing a v1 body by hand
        PortMemoryOStream propOnly;
        prop.Write(propOnly);
        // drop the prop's 2-byte version header: the decal's own header precedes it
        o1.SetVoid(propOnly.bytes().data(), static_cast<std::uint32_t>(propOnly.bytes().size()));
        cRZString s("Ab");
        o1.SetGZStr(s);
        o1.SetUint32(99);             // unused
        o1.SetFloat32(3.0f); o1.SetFloat32(4.0f);
        o1.SetUint32(5);              // integer height
        o1.SetFloat32(7.0f); o1.SetFloat32(3.0f); o1.SetFloat32(4.0f);
        o1.SetFloat32(1.0f); o1.SetFloat32(2.0f); o1.SetFloat32(3.0f);
        o1.SetFloat32(0.5f);
        o1.SetUint8(2);
        auto* v1 = new cSC4TextDecalOccupant();
        v1->AddRef();
        PortMemoryIStream i1(o1.bytes().data(), o1.bytes().size());
        CHECK(v1->Read(i1));
        CHECK(v1->TextHeight() == 5.0f && v1->DecalWidth() == 7.0f && v1->Orientation() == 2 && v1->Alpha() == 0.5f);
        v1->Release();
    }

    // Shutdown: notifications removed, decals and overlays dropped
    CHECK(d->Shutdown());
    CHECK(host.server.notes.size() >= 6 && host.server.notes.back().first == '-');
    CHECK(d->DecalIdA() == -1 && d->DecalIdB() == -1 && d->Texture() == nullptr);

    r->Release();
    d->Release();
    for (Texture* t : host.made) { CHECK(t->refs == 0); delete t; }   // the occupant released each one
    std::puts(failures ? "sc4_textdecaloccupant: FAILED" : "ok");
    return failures != 0;
}

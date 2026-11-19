// cSC4ViewInputControlSignTool: placing, selecting, dragging and editing signposts through fake renderer / prop / clipboard /
// message-server services.
#include <cstdio>
#include <string>
#include <vector>

#include "sc4/ViewInputControlSignTool.h"
#include "vic_fakes.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

struct FakeOccupant final : cISC4Occupant {
    float pos[3] = {0, 0, 0};
    float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return 0; }
    void GetPosition(float* xyz) override { for (int i = 0; i < 3; ++i) xyz[i] = pos[i]; }
    void SetPosition(const float* xyz) override { for (int i = 0; i < 3; ++i) pos[i] = xyz[i]; }
    void GetBoundingBox(float* mn, float* mx) override { for (int i = 0; i < 3; ++i) { mn[i] = boxMin[i]; mx[i] = boxMax[i]; } }
    void GetBoundingCityCells(SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

struct FakeSign final : cISC4SignpostOccupant {
    FakeOccupant occupant;
    std::string text;
    bool dynamic = false, editing = false;
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4SignpostOccupant) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cISC4Occupant* AsOccupant() override { return &occupant; }
    void SetText(const cIGZString& t) override { text = t.ToChar(); }
    void GetText(cIGZString& t) override { t.AssignFromChars(text.data(), static_cast<std::uint32_t>(text.size())); }
    void SetDynamic(bool d) override { dynamic = d; }
    void SetEditMode(bool e) override { editing = e; }
};

struct FakePicked final : IPickedObject {
    cIGZUnknown* owner = nullptr;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZUnknown* GetOwner() override { return owner; }
};

struct FakeRenderer final : ISignToolRenderer {
    bool pickObjectResult = false;
    IPickedObject* pick = nullptr;
    bool terrainResult = true;
    float terrainPoint[3] = {0, 0, 0};
    int lastTerrainX = 0, lastTerrainY = 0;
    bool PickObject(int, int, cISC4OccupantFilter*, IPickedObject** out) override {
        *out = pick;
        return pickObjectResult;
    }
    bool PickTerrain(int x, int y, float out[3], bool, bool) override {
        lastTerrainX = x;
        lastTerrainY = y;
        for (int i = 0; i < 3; ++i) out[i] = terrainPoint[i];
        return terrainResult;
    }
};

struct FakeMessageServer final : cIGZMessageServer {
    int added = 0, removed = 0;
    cIGZMessageTarget* lastTarget = nullptr;
    std::uint32_t lastType = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(const cGZMessage&) override { return true; }
    bool MessagePost(const cGZMessage&, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget* t, std::uint32_t type) override { ++added; lastTarget = t; lastType = type; return true; }
    bool RemoveNotification(cIGZMessageTarget* t, std::uint32_t type) override { ++removed; lastTarget = t; lastType = type; return true; }
    bool GeneralMessagePostToTarget(const cGZMessage&, cIGZMessageTarget*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
};

struct FakeClipboard final : cIGZClipboardManager {
    std::string text;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    std::uint32_t GetClipboardCount() override { return 0; }
    cIGZClipboard* GetClipboard(std::uint32_t) override { return nullptr; }
    std::uint32_t GetClipboards(cIGZClipboard**, std::uint32_t&) override { return 0; }
    bool AddClipboard(cIGZClipboard*) override { return false; }
    bool RemoveClipboard(cIGZClipboard*) override { return false; }
    bool RemoveClipboard(std::uint32_t) override { return false; }
    bool IsClipboardDataTypeAvailable(std::uint32_t, std::uint32_t) override { return false; }
    std::uint32_t GetAvailableClipboardDataTypes(std::uint32_t*, std::uint32_t&, std::uint32_t) override { return 0; }
    std::uint32_t GetClipboardWithDataTypeAvailable(std::uint32_t) override { return 0; }
    bool GetClipboardData(std::uint32_t, std::uint32_t, void**, std::uint32_t) override { return false; }
    bool SetClipboardData(cIGZClipboardData*, bool, std::uint32_t) override { return false; }
    bool ClearClipboard(std::uint32_t, std::uint32_t) override { return false; }
    bool GetClipboardText(cIGZString& out, std::uint32_t id) override {
        if (id != 0 || text.empty()) return false;
        out.AssignFromChars(text.data(), static_cast<std::uint32_t>(text.size()));
        return true;
    }
    bool SetClipboardText(const cIGZString&, bool, std::uint32_t) override { return false; }
    bool GetClipboardText(void*, std::uint32_t&, std::uint32_t, std::uint32_t) override { return false; }
    bool SetClipboardText(void*, std::uint32_t, std::uint32_t, bool, std::uint32_t) override { return false; }
};

struct FakeHost final : ISignToolHost {
    FakeRenderer renderer;
    FakeMessageServer server;
    FakeClipboard clipboard;
    bool hasServer = true, hasClipboard = true;
    float altitude = 3.0f, sea = 10.0f;
    int propDevMode = -1;
    std::vector<cISC4Occupant*> added, removed;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sounds;
    std::vector<FakeSign*> created;
    float projectedX = 100.0f, projectedY = 200.0f;
    ISignToolRenderer* Renderer() override { return &renderer; }
    float TerrainAltitude(float, float) override { return altitude; }
    float SeaLevel() override { return sea; }
    void AddProp(cISC4Occupant* o) override { added.push_back(o); }
    void RemoveProp(cISC4Occupant* o) override { removed.push_back(o); }
    void SetPropDeveloperMode(int m) override { propDevMode = m; }
    cISC4SignpostOccupant* CreateSignpost() override { created.push_back(new FakeSign()); return created.back(); }
    void ProjectToViewport(float p[3]) override { p[0] = projectedX; p[1] = projectedY; }
    void PlaySound(std::uint32_t g, std::uint32_t id) override { sounds.emplace_back(g, id); }
    cIGZMessageServer* MessageServer() override { return hasServer ? &server : nullptr; }
    cIGZClipboardManager* ClipboardManager() override { return hasClipboard ? &clipboard : nullptr; }
};

struct ViewHost final : IViewInputHost {
    vic_fakes::Mgr mgr;
    IViewInputView* View3D() override { return nullptr; }
    cIGZWinMgr* WindowManager() override { return &mgr; }
    void PlaySound(int, std::uint32_t) override {}
};

}  // namespace

int main() {
    ViewHost viewHost;
    SetViewInputHost(&viewHost);
    FakeHost host;
    SetSignToolHost(&host);

    auto* tool = new cSC4ViewInputControlSignTool();
    tool->AddRef();
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kIdle && tool->Selection() == nullptr);
    CHECK(!tool->OnCharacter('a'));   // not editing: ignored

    // Init: ids, services, notification
    CHECK(tool->Init());
    CHECK(tool->GetID() == 0x8b4ce5a0);
    CHECK(host.propDevMode == 1 && host.server.added == 1 && host.server.lastType == kSignToolMessage);
    CHECK(host.clipboard.refs == 2 && tool->CodePage() == 8);
    CHECK(tool->Init() && host.server.added == 1);   // second Init does nothing
    void* iface = nullptr;
    CHECK(tool->QueryInterface(kGZIID_cIGZMessageTargetLegacy, &iface) && iface == static_cast<cIGZMessageTarget*>(tool));
    CHECK(host.server.lastTarget == static_cast<cIGZMessageTarget*>(tool));
    tool->Release();   // the QueryInterface reference
    CHECK(tool->QueryInterface(1, &iface) && iface == static_cast<cSC4ViewInputControl*>(tool));
    tool->Release();
    CHECK(!tool->QueryInterface(0x1234, &iface));

    // --- place a new sign on the terrain: y is max(altitude, sea level)
    host.renderer.pickObjectResult = false;
    host.renderer.terrainPoint[0] = 5; host.renderer.terrainPoint[1] = 0; host.renderer.terrainPoint[2] = 7;
    CHECK(tool->OnMouseDownL(10, 20, 0));
    CHECK(host.created.size() == 1);
    FakeSign* sign = host.created[0];
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kEditing && tool->Selection() == sign);
    CHECK(sign->occupant.pos[0] == 5 && sign->occupant.pos[1] == 10 && sign->occupant.pos[2] == 7);   // sea level wins
    CHECK(host.added.size() == 1 && host.added[0] == &sign->occupant);
    CHECK(sign->editing && sign->dynamic && sign->text.empty());
    CHECK(sign->refs == 1);   // the creation reference is released again; the selection holds the only one (the prop manager is faked)
    CHECK(host.sounds.size() == 1 && host.sounds[0].second == kSignToolSoundPlace);

    // typing (code page 8: UTF-8)
    CHECK(tool->OnCharacter('a') && tool->OnCharacter('b'));
    CHECK(sign->text == "ab" && tool->Text().Strlen() == 2);
    CHECK(tool->OnCharacter('\x01') && sign->text == "ab");        // control characters are ignored
    CHECK(tool->OnCharacter('\xc3') && sign->text == "ab");         // lead byte is held back
    CHECK(tool->OnCharacter('\xa9') && sign->text == "ab\xc3\xa9");
    CHECK(tool->OnCharacter('\b') && sign->text == "ab");           // removes the whole two-byte character
    CHECK(tool->OnCharacter('\b') && sign->text == "a");
    CHECK(tool->OnCharacter('\x1b') && sign->text == "a");          // Escape does nothing here
    // the glyph limit
    for (int i = 0; i < 50; ++i) tool->OnCharacter('x');
    CHECK(sign->text.size() == 0x21);
    // Escape restores the text saved at edit start and answers false
    CHECK(!tool->OnKeyDown(0x1b, 0));
    CHECK(sign->text.empty() && tool->Text().Strlen() == 0);
    // paste: newlines are dropped but count as glyphs
    host.clipboard.text = "p\nq";
    CHECK(tool->OnKeyDown(0x56, 2) && sign->text == "pq");
    CHECK(tool->OnKeyDown(0x56, 3) && sign->text == "pq");          // wrong modifiers: no paste
    CHECK(tool->OnKeyDown(0x41, 0));                                // any key is swallowed while editing
    // Enter ends the edit and keeps the sign (it has text)
    CHECK(tool->OnCharacter('\r'));
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kIdle && tool->Selection() == nullptr);
    CHECK(!sign->editing && !sign->dynamic && host.removed.empty() && sign->refs == 0);

    // --- hover selects a sign, an empty hover deselects
    FakePicked picked;
    picked.owner = sign;
    host.renderer.pick = &picked;
    host.renderer.pickObjectResult = true;
    CHECK(tool->OnMouseMove(1, 2, 0));
    CHECK(tool->Selection() == sign && sign->dynamic && picked.refs == 0);   // pick result released on the hover path
    picked.refs = 1;
    host.renderer.pickObjectResult = false;
    host.renderer.pick = nullptr;
    CHECK(tool->OnMouseMove(1, 2, 0) && tool->Selection() == nullptr && !sign->dynamic);

    // --- click on the sign: pressed; the offset is the projected sign position minus the click
    host.renderer.pick = &picked;
    host.renderer.pickObjectResult = true;
    host.sounds.clear();
    CHECK(tool->OnMouseDownL(30, 40, 0));
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kPressed && tool->Selection() == sign);
    CHECK(host.sounds.size() == 1 && host.sounds[0].second == kSignToolSoundGrab);
    CHECK(picked.refs == 1);   // quirk: the original never releases the pick result on this path
    // a plain click edits the sign
    CHECK(tool->OnMouseUpL(30, 40, 0));
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kEditing && sign->editing);
    CHECK(tool->Text().ToChar() == std::string("pq") && tool->SavedText().ToChar() == std::string("pq"));
    // a click while editing only ends the edit mode
    CHECK(tool->OnMouseDownL(0, 0, 0));
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kIdle && !sign->editing && sign->dynamic && tool->Selection() == sign);

    // --- drag
    host.renderer.pickObjectResult = true;
    tool->OnMouseDownL(30, 40, 0);
    host.renderer.terrainPoint[0] = 50; host.renderer.terrainPoint[1] = 60; host.renderer.terrainPoint[2] = 70;
    CHECK(tool->OnMouseMove(35, 45, 0));
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kDragging);
    CHECK(host.renderer.lastTerrainX == (100 - 30) + 35 && host.renderer.lastTerrainY == (200 - 40) + 45);
    CHECK(sign->occupant.pos[0] == 50 && sign->occupant.pos[2] == 70);
    host.sounds.clear();
    CHECK(tool->OnMouseUpL(35, 45, 0));   // the sign has text: the drag simply ends
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kIdle && host.sounds.size() == 1 && host.sounds[0].second == kSignToolSoundPlace);

    // --- Delete removes the selected sign from the prop manager
    CHECK(!tool->OnKeyDown(0x41, 0));
    CHECK(tool->OnKeyDown(0x2e, 0));
    CHECK(tool->Selection() == nullptr && host.removed.size() == 1 && host.removed[0] == &sign->occupant);
    CHECK(!tool->OnKeyDown(0x2e, 0));   // nothing selected any more

    // --- an empty sign that is deselected is removed from the prop manager
    host.renderer.pickObjectResult = false;
    host.renderer.pick = nullptr;
    host.removed.clear();
    CHECK(tool->OnMouseDownL(1, 1, 0));
    FakeSign* second = host.created[1];
    CHECK(tool->Selection() == second);
    CHECK(tool->OnMouseDownL(1, 1, 0));   // ends editing
    CHECK(tool->GetState() == cSC4ViewInputControlSignTool::kIdle);
    host.renderer.pickObjectResult = false;
    CHECK(tool->OnMouseMove(1, 1, 0));    // hover over nothing: the empty sign is dropped
    CHECK(tool->Selection() == nullptr && host.removed.size() == 1 && host.removed[0] == &second->occupant);

    // --- no terrain under the cursor: nothing happens, the click is not handled
    host.renderer.terrainResult = false;
    CHECK(!tool->OnMouseDownL(1, 1, 0) && host.created.size() == 2);
    host.renderer.terrainResult = true;

    // --- DoMessage: screen position of the edited sign
    cGZMessage message;
    message.type = kSignToolMessage;
    CHECK(!tool->DoMessage(message) && message.param1 == 0);   // not editing
    CHECK(tool->OnMouseDownL(1, 1, 0));
    FakeSign* third = host.created[2];
    third->occupant.boxMin[0] = 0; third->occupant.boxMax[0] = 4; third->occupant.boxMin[2] = 10; third->occupant.boxMax[2] = 20;
    third->occupant.boxMax[1] = 9;
    host.projectedX = 321.9f; host.projectedY = 12.5f;
    CHECK(!tool->DoMessage(message));   // always false
    CHECK(message.param1 == 1 && message.param2 == 321 && message.param3 == 12);
    cGZMessage other;
    other.type = 5;
    CHECK(!tool->DoMessage(other) && other.param1 == 0);

    // --- Shutdown while editing
    CHECK(tool->Shutdown());
    CHECK(!third->editing && tool->Selection() == nullptr && host.server.removed == 1 && host.clipboard.refs == 1);
    CHECK(tool->Shutdown());   // idempotent
    tool->Release();

    for (FakeSign* s : host.created) { CHECK(s->refs == 0); delete s; }   // Release only counts down
    SetSignToolHost(nullptr);
    SetViewInputHost(nullptr);
    std::printf(failures ? "viewinputcontrol_signtool: %d FAILURES\n" : "viewinputcontrol_signtool: ok\n", failures);
    return failures ? 1 : 0;
}

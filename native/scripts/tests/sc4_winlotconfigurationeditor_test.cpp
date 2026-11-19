// cSC4WinLotConfigurationEditor — behaviour of the recovered window plumbing and the editing path:
// reference counting / QueryInterface, the static family-name tables, cursor loading, the identity lookup,
// CreateNewLotConfiguration / ReadLotConfigurationExemplar, the caption, the object geometry (hit test, bounds,
// move / rotate / mirror / duplicate / delete, fences, foundation), the message routing, the keyboard tools, the
// save / close flows and the object-name helpers.
//
// The unrecovered neighbours (cursor loader, resource manager, lot manager write/read, message box, the family
// selectors, the menus, the grid used by the dialogs) are replaced by a fake IWinLotConfigurationEditorHost; the
// window manager is the shared fake_winmgr.h one.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sc4/LotConfigurationManager.h"
#include "sc4/WinLotConfigurationEditor.h"

#include "gz/SmallObjects.h"

#include "fake_winmgr.h"

namespace {

using sc4::cSC4LotConfiguration;
using sc4::cSC4LotConfigurationManager;
using sc4::cSC4WinLotConfigurationEditor;
using sc4::IWinLotConfigurationEditorHost;

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
}
#define CHECK(expr) Check((expr), #expr, __LINE__)
#define CHECK_NEAR(a, b) Check(std::fabs(double(a) - double(b)) < 1e-4, #a " ~= " #b, __LINE__)

// A COM object with a high, never-dropping reference count: the identity holder the editor keeps no reference to.
struct StickyObject : cIGZUnknown {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs ? --refs : 0; }
    std::uint32_t refs = 1000;
};

struct FakeCursorObject final : cIGZCursor {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 0x834c471e) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs <= 1) {
            delete this;
            return 0;
        }
        return --refs;
    }
    std::uint32_t refs = 1;
};

struct FakeHost : IWinLotConfigurationEditorHost {
    struct Message {
        std::string text, title;
        std::uint32_t style;
    };
    std::vector<Message> messages;
    std::uint32_t answer = sc4::kMsgBoxOK;
    std::vector<std::uint32_t> writtenKeys, savedKeys;
    int chooserClosed = 0;
    std::vector<int> selectedKinds;
    float cameraTarget[3] = {0, 0, 0};
    int cameraCalls = 0;
    bool shiftDown = false;
    std::uint32_t familyAnswer = 7;
    int popupAnswer = -1;
    std::vector<std::string> popupLabels;
    std::vector<std::uint32_t> gridSorts;
    std::vector<char> gridLetters;
    int foundationMenus = 0, fenceMenus = 0, objectMenus = 0, areaMenus = 0, outsideMenus = 0, foundationUICalls = 0;
    std::uint32_t foundationUI = 0;
    bool writeResult = true;
    bool foundationHasSwitch = false;
    std::uint32_t nextId = 0x9000;
    cIGZWin* registered = nullptr;

    cSC4LotConfigurationManager* manager = nullptr;
    StickyObject exemplarObject, holderObject, networkObject;
    cSC4LotConfiguration* nextConfiguration = nullptr;   // transferred to the editor by ReadLotConfiguration

    cIGZCursor* CreateCursor(std::uint32_t group, std::uint32_t instance) override {
        cursorRequests.push_back({group, instance});
        return new FakeCursorObject();
    }
    cIGZFont* CreateStatusFont() override { return nullptr; }
    cIGZBuffer* CreateViewBuffer(int, int) override { return nullptr; }
    std::uint32_t MessageBox(const cIGZString& text, const cIGZString& title, std::uint32_t style) override {
        messages.push_back({text.ToChar(), title.ToChar(), style});
        return answer;
    }
    void RegisterWindow(cIGZWin* window) override { registered = window; }
    bool GetIdentityInfoForExemplarKey(const cGZPersistResourceKey& key,
                                       sc4::cSC4LotConfigurationIdentityInfo& out) override {
        if (key.type != 0x6534284a) return false;
        out.name.AssignFromCString("Test Family Lot");
        out.exemplar = &exemplarObject;
        out.propertyHolder = &holderObject;
        return true;
    }
    std::string DataDirectory() override { return "Data/"; }
    cSC4LotConfigurationManager* LotConfigurationManager() override { return manager; }
    cIGZUnknown* CreateNetworkObject(int) override { return &networkObject; }
    bool WriteLotConfiguration(const cGZPersistResourceKey& key, cSC4LotConfiguration*, cIGZPersistDBSegment*) override {
        writtenKeys.push_back(key.instance);
        return writeResult;
    }
    bool ReadLotConfiguration(const cGZPersistResourceKey&, cSC4LotConfiguration*& out) override {
        if (!nextConfiguration) return false;
        out = nextConfiguration;
        nextConfiguration = nullptr;
        return true;
    }
    void LotConfigurationWritten(const cGZPersistResourceKey& key) override { savedKeys.push_back(key.instance); }
    void ChooserClosed(sc4::cSC4WinLotConfigurationEditor*) override { ++chooserClosed; }
    int PopupStringListMenu(cIGZWin*, int, int, const std::vector<cRZString>& items) override {
        popupLabels.clear();
        for (const cRZString& item : items) popupLabels.push_back(item.str());
        return popupAnswer;
    }
    void DialogGridSort(cIGZWin*, std::uint32_t command) override { gridSorts.push_back(command); }
    void DialogGridScrollToLetter(cIGZWin*, char letter) override { gridLetters.push_back(letter); }
    bool SelectFamilies(int kind, std::vector<std::uint32_t>& inOut) override {
        selectedKinds.push_back(kind);
        inOut = {familyAnswer++};
        return true;
    }
    void FoundationMenu(sc4::cSC4WinLotConfigurationEditor*, int, int) override { ++foundationMenus; }
    void FenceMenu(sc4::cSC4WinLotConfigurationEditor*, int, int) override { ++fenceMenus; }
    void LotConfigurationObjectMenu(sc4::cSC4WinLotConfigurationEditor*, int, int,
                                    sc4::cSC4LotConfigurationObject*) override {
        ++objectMenus;
    }
    void EditAreaMenu(sc4::cSC4WinLotConfigurationEditor*, int, int) override { ++areaMenus; }
    void OutsideAreaMenu(sc4::cSC4WinLotConfigurationEditor*, int, int) override { ++outsideMenus; }
    void SetFoundationUI(sc4::cSC4WinLotConfigurationEditor*, std::uint32_t current,
                         sc4::cSC4LotConfigurationObject*) override {
        foundationUI = current;
        ++foundationUICalls;
    }
    bool IsFoundationType(std::uint32_t id) override { return id == 0xf00d; }
    bool IsBuildingType(std::uint32_t id) override { return id == 0xb001; }
    bool BuildingName(std::uint32_t id, cRZString& out) override {
        if (id != 0xb001) return false;
        out.AssignFromCString("Test Building");
        return true;
    }
    bool BuildingsInFamily(std::uint32_t family, std::vector<std::uint32_t>& out) override {
        if (family != 0x16) return false;
        out = {0xb001};
        return true;
    }
    bool IsPropType(std::uint32_t id) override { return id == 0xb002; }
    bool PropName(std::uint32_t id, cRZString& out) override {
        if (id != 0xb002) return false;
        out.AssignFromCString("Test Prop");
        return true;
    }
    bool PropsInFamily(std::uint32_t family, std::vector<std::uint32_t>& out) override {
        if (family != 0x17) return false;
        out = {0xb002};
        return true;
    }
    bool RetainingWallExemplarKey(std::uint32_t id, cGZPersistResourceKey& out) override {
        if (id != 0xaa01) return false;
        out = cGZPersistResourceKey{0xa52160f5, 0xa8fbd372, 0xaa01};
        return true;
    }
    bool ExemplarName(std::uint32_t, std::uint32_t instance, cRZString& out) override {
        if (instance == 0xaa01) {
            out.AssignFromCString("Test Retaining Wall");
        } else if (instance == 0xf00d) {
            out.AssignFromCString("Test Foundation");
        } else {
            out.AssignFromCString("Test Exemplar");
        }
        return true;
    }
    bool ExemplarHasProperty(std::uint32_t, std::uint32_t instance, std::uint32_t propertyId) override {
        return instance == 0xf00d && propertyId == 0x68fcff37 && foundationHasSwitch;
    }
    std::uint32_t NewObjectId() override { return nextId++; }
    bool IsKeyDown(std::uint32_t key) override { return key == 0x20 && shiftDown; }
    float TerrainHeight(int tileX, int tileZ) override { return 12.0f + float(tileX) + float(tileZ); }
    void SetCameraTargetPosition(float x, float y, float z) override {
        cameraTarget[0] = x;
        cameraTarget[1] = y;
        cameraTarget[2] = z;
        ++cameraCalls;
    }

    struct CursorRequest {
        std::uint32_t group, instance;
    };
    std::vector<CursorRequest> cursorRequests;
};

// ---- fixture ----------------------------------------------------------------------------------------------------

struct Fixture {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    FakeHost host;
    cSC4LotConfigurationManager* manager = nullptr;
    sc4::cSC4WinLotConfigurationEditor* editor = nullptr;

    Fixture() {
        SetWinGraphics(&graphics);
        manager = new cSC4LotConfigurationManager();
        host.manager = manager;
        sc4::SetWinLotConfigurationEditorHost(&host);
        editor = new sc4::cSC4WinLotConfigurationEditor(&mgr);
        editor->AddRef();   // the window manager / factory holds the first reference
        editor->SetArea(0, 0, 400, 300);
        editor->Init();
    }
    ~Fixture() {
        sc4::SetWinLotConfigurationEditorHost(nullptr);
        if (editor) {
            editor->Shutdown();
            editor->Release();
            editor = nullptr;
        }
        SetWinGraphics(nullptr);
    }
    sc4::cSC4LotConfiguration* Config() { return editor->LotConfiguration(); }
    sc4::cSC4LotConfigurationObject& AddObject(std::uint8_t type, float x, float z, float halfX = 8.0f,
                                               float halfZ = 8.0f) {
        sc4::cSC4LotConfigurationObject object;
        object.type = type;
        object.x = x;
        object.z = z;
        object.minX = x - halfX;
        object.maxX = x + halfX;
        object.minZ = z - halfZ;
        object.maxZ = z + halfZ;
        object.id = 0x1234;
        Config()->GetLotConfigurationObjectArray().push_back(object);
        return Config()->GetLotConfigurationObjectArray().back();
    }
    void NewLot(std::uint8_t sizeX = 2, std::uint8_t sizeZ = 2) {
        editor->CreateNewLotConfiguration();
        editor->SetLotSizeForTest(sizeX, sizeZ);
    }
};

// ---- tests ------------------------------------------------------------------------------------------------------

void TestLifecycle() {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    FakeHost host;
    SetWinGraphics(&graphics);
    sc4::SetWinLotConfigurationEditorHost(&host);
    auto* editor = new sc4::cSC4WinLotConfigurationEditor(&mgr);
    editor->AddRef();
    editor->SetArea(0, 0, 400, 300);
    CHECK(editor->Init());
    CHECK(host.registered == editor);
    CHECK(host.cursorRequests.size() == 7);
    CHECK(host.cursorRequests[0].group == sc4::kLotEditorCursorGroup);
    CHECK(host.cursorRequests[0].instance == 0x4a5cdc3f);
    CHECK(host.cursorRequests[1].instance == 0x4a5cdc3a);

    // QueryInterface: the message-filter view, the window views and the gen view.
    cIGZUnknown* unknown = nullptr;
    CHECK(editor->QueryInterface(gz::kGZIID_cIGZWinSpyMessageFilter, reinterpret_cast<void**>(&unknown)));
    CHECK(unknown == static_cast<cIGZWinMessageFilter*>(editor));
    CHECK(editor->QueryInterface(kGZIID_cIGZWin, reinterpret_cast<void**>(&unknown)));
    CHECK(unknown == static_cast<cIGZWin*>(editor));
    CHECK(editor->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&unknown)));
    CHECK(unknown == static_cast<gz::cIGZWinGen*>(editor));
    CHECK(!editor->QueryInterface(0x12345678, reinterpret_cast<void**>(&unknown)));
    for (int i = 0; i < 3; ++i) editor->Release();   // the three successful QueryInterface calls

    // Init twice is a no-op; the static init ran once.
    const std::size_t cursorCount = host.cursorRequests.size();
    CHECK(editor->Init());
    CHECK(host.cursorRequests.size() == cursorCount);

    // Family-name callbacks are the ini enumeration entry points (process-wide tables).
    sc4::cSC4WinLotConfigurationEditor::AddBuildingFamilyName(cRZString("22"), cRZString("Houses"), nullptr);
    sc4::cSC4WinLotConfigurationEditor::AddPropFamilyName(cRZString("77"), cRZString("Trees"), nullptr);
    sc4::cSC4WinLotConfigurationEditor::AddFloraFamilyName(cRZString("99"), cRZString("Bushes"), nullptr);
    CHECK(sc4::EditorBuildingFamilyNames().at(22).str() == "Houses");
    CHECK(sc4::EditorPropFamilyNames().at(77).str() == "Trees");
    CHECK(sc4::EditorFloraFamilyNames().at(99).str() == "Bushes");

    // Shutdown releases the cursors and the manager; Release destroys the window.
    editor->Shutdown();
    editor->Release();
    sc4::SetWinLotConfigurationEditorHost(nullptr);
    SetWinGraphics(nullptr);
}

void TestTitleAndIdentity() {
    Fixture f;
    f.NewLot();
    CHECK(f.editor->TitleBuffer().str() == "Lot Editor -- New Lot");
    f.editor->SetDataHasChanged(true);
    CHECK(f.editor->TitleBuffer().str() == "Lot Editor -- New Lot *");
    CHECK(f.editor->DataHasChanged());
    CHECK(static_cast<cIGZWin*>(f.editor)->GetCaption().Data() == std::string("Lot Editor -- New Lot *"));

    // Exemplar identity: key copied, name from the host, one reference on the exemplar; the property holder keeps
    // the original's unbalanced count (AddRef then Release).
    sc4::cSC4LotConfigurationIdentityInfo info;
    CHECK(f.editor->GetIdentityInfoForLotConfigurationExemplarKey(
        cGZPersistResourceKey{0x6534284a, 0xa8fbd372, 0x77}, info));
    CHECK(info.id == 0x77);
    CHECK(info.key.group == 0xa8fbd372);
    CHECK(info.name.str() == "Test Family Lot");
    CHECK(info.exemplar == &f.host.exemplarObject);
    CHECK(f.host.exemplarObject.refs == 1001);            // the editor's reference
    CHECK(f.host.holderObject.refs == 1000);              // net nothing, as in the original
    CHECK(f.editor->IsEditingExemplar(0x77) == false);    // only Open writes the editor's identity
    info.exemplar->Release();

    // Type lookup goes through the manager map (id -> configuration) and builds the exemplar key.
    cSC4LotConfigurationManager::GetLotConfigurationMap()[0x42] = f.Config();
    f.Config()->AddRef();
    sc4::cSC4LotConfigurationIdentityInfo byType;
    CHECK(f.editor->GetIdentityInfoForLotConfigurationType(0x42, byType));
    CHECK(byType.key.instance == f.Config()->GetID());
    CHECK(byType.name.str() == "Test Family Lot");
    if (byType.exemplar) byType.exemplar->Release();
    cSC4LotConfigurationManager::GetLotConfigurationMap().erase(0x42);
    f.Config()->Release();
}

void TestMouseRouting() {
    Fixture f;
    f.NewLot();
    // Without a view child the editor forwards everything to cGZWinGen.
    CHECK(f.editor->GZOnMouseDownL(1, 2, 0));
    CHECK(f.editor->GZOnMouseMove(3, 4, 0));
    CHECK(f.editor->GZOnMouseUpL(5, 6, 0));

    // A right click outside the view opens the area menu through the host seam.
    f.editor->GZOnMouseDownR(1, 2, 0);
    CHECK(f.host.outsideMenus == 1);
    f.editor->DoEditAreaMenu(3, 4);
    CHECK(f.host.areaMenus == 1);

    // The editing entry points take view coordinates directly.
    auto& object = f.AddObject(0, 8, 8);
    sc4::cSC4LotConfigurationObject* hit = nullptr;
    CHECK(f.editor->DoHitTest(8, 8, 0, &hit) == 100);
    CHECK(hit == &object);
    CHECK(f.editor->DoEditMouseDownL(8, 8, 0));
    CHECK(f.editor->SelectedObject() == &object);
    f.editor->GZOnKeyDown(0x1b, 0);   // Escape: cancels the tool
    CHECK(f.editor->ToolMode() == 0);
}

void TestHitTest() {
    Fixture f;
    f.NewLot();   // 2x2 tiles = 32 x 32 world units
    auto& building = f.AddObject(0, 8, 8);
    sc4::cSC4LotConfigurationObject* hit = nullptr;

    CHECK(f.editor->DoHitTest(8, 8, 0, &hit) == 100);
    CHECK(hit == &building);
    CHECK(f.editor->DoHitTest(24, 24, 0, &hit) == 1);    // empty lot
    CHECK(hit == nullptr);
    CHECK(f.editor->DoHitTest(16, 28, 0, &hit) == 3);    // fence line at x = 16
    CHECK(f.editor->DoHitTest(34, 8, 0, &hit) == 2);     // border band behind the edge
    CHECK(f.editor->DoHitTest(40, 8, 0, &hit) == 0);     // outside

    // Overlap: a second object under the same pixel; Control asks which one.
    auto& prop = f.AddObject(1, 9, 9);
    f.host.popupAnswer = 1;
    CHECK(f.editor->DoHitTest(8, 8, 2, &hit) == 101);
    CHECK(hit == &prop);
    CHECK(f.host.popupLabels.size() == 2);
    CHECK(f.host.popupLabels[0] == "0: building");
    CHECK(f.host.popupLabels[1] == "1: prop");
    f.host.popupAnswer = -1;
    CHECK(f.editor->DoHitTest(8, 8, 2, &hit) == 0);

    // The selected object wins without Control even when it is under another object.
    f.editor->SetSelectedLotConfigurationObject(&prop);
    CHECK(f.editor->SelectedIndex() == 1);
    CHECK(f.editor->DoHitTest(8, 8, 0, &hit) == 101);
    CHECK(hit == &prop);
}

void TestMovementAndBounds() {
    Fixture f;
    f.NewLot();
    auto& object = f.AddObject(0, 8, 8);
    f.editor->SetSelectedLotConfigurationObject(&object);

    CHECK(f.editor->IsObjectWithinLotBounds(&object, true));
    // Outside: shift left until minX < 0.
    object.minX = -2.0f;
    CHECK(!f.editor->IsObjectWithinLotBounds(&object, true));
    object.minX = 0.0f;

    // Arrow-key movement is half a world unit (a quarter with Shift, x4 with Control).
    f.editor->GZOnKeyDown(0x27, 0);
    CHECK_NEAR(object.x, 8.5);
    CHECK_NEAR(object.minX, 0.5);
    CHECK_NEAR(object.maxX, 16.5);
    f.editor->GZOnKeyDown(0x25, 2);   // Shift+left: 0.25
    CHECK_NEAR(object.x, 8.25);
    f.editor->GZOnKeyDown(0x28, 1);   // Control+down: 2.0 (z was pulled to 8.05 by the 0.05 margin)
    CHECK_NEAR(object.z, 10.05);

    // The clamps keep a margin of 0.05 from the lot edges.
    f.editor->MoveObject(&object, 100.0f, 100.0f);
    CHECK_NEAR(object.maxX, 31.95f);
    CHECK_NEAR(object.maxZ, 31.95f);
    CHECK_NEAR(object.x, 23.95f);
    CHECK_NEAR(object.z, 23.95f);
    f.editor->MoveObjectToWithinLotBounds(&object, true);
    CHECK_NEAR(object.maxX, 31.95f);
    CHECK_NEAR(object.maxZ, 31.95f);
    CHECK(f.editor->DataHasChanged());

    // Moving left of the origin clamps minX to 0.05 for non-flora objects (asymmetric) ...
    object.x = 2.0f;
    object.minX = 0.0f;
    object.maxX = 16.0f;
    f.editor->MoveObjectToWithinLotBounds(&object, true);
    CHECK_NEAR(object.minX, 0.05f);
    CHECK_NEAR(object.x, 2.05f);
    // ... and x itself for flora (symmetric).
    object.x = 0.0f;
    object.minX = -8.0f;
    object.maxX = 8.0f;
    f.editor->MoveObjectToWithinLotBounds(&object, false);
    CHECK_NEAR(object.x, 0.05f);

    // Shift changes a prop's elevation instead of moving it.
    auto& prop = f.AddObject(1, 8, 8);
    f.host.shiftDown = true;
    f.editor->MoveObject(&prop, std::uint32_t(0x26), std::uint32_t(0));
    CHECK_NEAR(prop.y, 0.5);
    CHECK_NEAR(prop.z, 8.05);   // the margin clamp moved it off the edge first
    f.host.shiftDown = false;
}

void TestObjectOperations() {
    Fixture f;
    f.NewLot();

    // Rotate: the orientation wraps and the bounds are re-centred around the object.
    auto* object = &f.AddObject(0, 8, 8);
    object->orientation = 3;
    f.editor->RotateObject(object, 0, 0);
    CHECK(object->orientation == 0);
    CHECK_NEAR(object->minX, 0.0);
    CHECK_NEAR(object->maxX, 16.0);

    // The extents swap axes: a 16 x 8 object becomes 8 x 16 around the same centre.
    object->minX = 0.0f;
    object->maxX = 16.0f;
    object->minZ = 4.0f;
    object->maxZ = 12.0f;
    f.editor->RotateObject(object, 0, 0);
    CHECK_NEAR(object->minX, 4.0);
    CHECK_NEAR(object->maxX, 12.0);
    CHECK_NEAR(object->minZ, 0.0);
    CHECK_NEAR(object->maxZ, 16.0);

    // Mirror only applies to the base texture (type 2). Adding it reallocated the object vector, so the first
    // object is re-fetched (the original keeps raw pointers into it as well).
    auto* base = &f.AddObject(2, 8, 8);
    object = &f.Config()->GetLotConfigurationObjectArray()[0];
    f.editor->MirrorObject(base, 0, 0);
    CHECK(base->mirrored == 1);
    f.editor->MirrorObject(object, 0, 0);
    CHECK(object->mirrored == 0);

    // Duplicate inserts a copy at the front with a fresh id, offset by 4 units. (Inserting reallocates the vector,
    // so the object pointer is re-fetched afterwards - the original keeps raw pointers into the same vector.)
    const std::uint32_t firstId = f.host.nextId;
    f.editor->DuplicateObject(base, 0, 0);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 3);
    CHECK(f.editor->SelectedObject() == &f.Config()->GetLotConfigurationObjectArray().front());
    CHECK(f.Config()->GetLotConfigurationObjectArray().front().id == firstId);
    CHECK_NEAR(f.Config()->GetLotConfigurationObjectArray().front().x, 12.0);
    CHECK(f.editor->DataHasChanged());

    // Foundations (type 3) and unknown types are never duplicated.
    f.AddObject(3, 8, 8);
    f.AddObject(7, 8, 8);
    const std::uint32_t count = f.Config()->GetLotConfigurationObjectCount();
    f.editor->DuplicateObject(&f.Config()->GetLotConfigurationObjectArray()[3], 0, 0);
    f.editor->DuplicateObject(&f.Config()->GetLotConfigurationObjectArray()[4], 0, 0);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == count);

    // Delete asks first; "No" keeps the object, "Yes" erases it.
    auto* prop = &f.AddObject(1, 20, 20);
    const std::uint32_t withProp = f.Config()->GetLotConfigurationObjectCount();
    f.editor->SetSelectedLotConfigurationObject(prop);
    f.host.answer = sc4::kMsgBoxNo;
    f.editor->DeleteLotConfigurationObject(prop, true);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == withProp);
    CHECK(f.host.messages.back().text == "Are you sure you want to delete this prop?");
    CHECK(f.host.messages.back().style == sc4::kMsgBoxStyleYesNo);
    f.host.answer = sc4::kMsgBoxYes;
    f.editor->DeleteLotConfigurationObject(prop, true);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == withProp - 1);
    CHECK(f.editor->SelectedObject() == nullptr);
    auto* first = &f.Config()->GetLotConfigurationObjectArray().front();
    f.editor->DeleteLotConfigurationObject(first, false);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == withProp - 2);
}

void TestFencesAndFoundation() {
    Fixture f;
    f.NewLot();
    // A click near the tile boundary x = 16 (z = 8) adds the vertical segment {1, 0, 1, 1}.
    f.editor->ToggleFence(16, 8, 0);
    const auto& segments = f.Config()->GetFenceSegmentPositionArray();
    CHECK(segments.size() == 4);
    CHECK(segments[0] == 1);
    CHECK(segments[1] == 0);
    CHECK(segments[2] == 1);
    CHECK(segments[3] == 1);
    f.editor->ToggleFence(16, 8, 0);   // toggles it off again
    CHECK(f.Config()->GetFenceSegmentPositionArray().empty());

    // A click near the tile boundary z = 16 (x = 8) adds the horizontal segment {0, 1, 1, 1}.
    f.editor->ToggleFence(8, 16, 0);
    CHECK(f.Config()->GetFenceSegmentPositionArray().size() == 4);
    CHECK(f.Config()->GetFenceSegmentPositionArray()[0] == 0);
    CHECK(f.Config()->GetFenceSegmentPositionArray()[1] == 1);
    CHECK(f.Config()->GetFenceSegmentPositionArray()[2] == 1);
    CHECK(f.Config()->GetFenceSegmentPositionArray()[3] == 1);

    sc4::SC4FenceSegment extra{0, 0, 1, 0};
    f.editor->ToggleFence(extra);
    CHECK(f.Config()->GetFenceSegmentPositionArray().size() == 8);
    f.editor->ToggleFence(extra);
    CHECK(f.Config()->GetFenceSegmentPositionArray().size() == 4);

    // Foundation: one at most, replaced when a different one is chosen, removed when add is false.
    CHECK(f.editor->LotFoundationIsPresent() == 0);
    f.editor->AddLotFoundation(0xf00d, true);
    CHECK(f.editor->LotFoundationIsPresent() == 0xf00d);
    f.editor->AddLotFoundation(0xf00d, true);   // same id: nothing changes
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 1);
    f.editor->AddLotFoundation(0x1111, true);   // replaced
    CHECK(f.editor->LotFoundationIsPresent() == 0x1111);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 1);
    f.editor->AddLotFoundation(0x1111, false);
    CHECK(f.editor->LotFoundationIsPresent() == 0);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 0);
}

void TestAddingObjects() {
    Fixture f;
    f.NewLot();
    f.host.nextId = 0x5000;

    // Building tool: the family dialog supplies the families, the manager's footprint drives the bounds.
    f.editor->GZOnKeyDown('B', 0);
    CHECK(f.editor->ToolMode() == 1);
    CHECK(f.host.selectedKinds.back() == 1);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 0);
    f.editor->DoEditMouseDownL(8, 8, 0);   // empty lot with a tool: create
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 1);
    const auto& created = f.Config()->GetLotConfigurationObjectArray().front();
    CHECK(created.type == 0);
    CHECK(created.id == 0x5000);
    CHECK_NEAR(created.x, 8.0);
    CHECK(f.editor->SelectedObject() == &created);
    CHECK(f.editor->DataHasChanged());

    // Flora tool and prop tool switch the cursor and the mode.
    f.editor->GZOnKeyDown('F', 0);
    CHECK(f.editor->ToolMode() == 2);
    f.editor->GZOnKeyDown('P', 0);
    CHECK(f.editor->ToolMode() == 3);
    f.editor->GZOnKeyDown('T', 0);
    CHECK(f.editor->ToolMode() == 5);

    // Fence mode is entered with F plus a modifier; clicking a fence line toggles it.
    f.editor->GZOnKeyDown('F', 1);
    CHECK(f.editor->ToolMode() == 8);
    f.editor->DoEditMouseDownL(16, 8, 0);
    CHECK(f.Config()->GetFenceSegmentPositionArray().size() == 4);

    // Escape returns to the no-tool mode.
    f.editor->GZOnKeyDown(0x1b, 0);
    CHECK(f.editor->ToolMode() == 0);

    // Base texture: created at the back of the list (type 2).
    f.editor->GZOnKeyDown('T', 0);
    f.editor->AddNewLotConfigurationObject(24, 24, 0);
    const auto& objects = f.Config()->GetLotConfigurationObjectArray();
    CHECK(objects.back().type == 2);
    CHECK_NEAR(objects.back().x, 23.95);
}

void TestMessagesAndTools() {
    Fixture f;
    f.NewLot();

    // Command from the title bar's close button closes the editor.
    cGZMessage close;
    close.type = kGZMsgCommand;
    close.param1 = gz::kWinCommandSender;
    close.param2 = gz::kWinGenCloseId;
    CHECK(f.editor->DoMessage(close));
    CHECK(f.editor->DoMessage(static_cast<cGZMessage&>(close)));
    cGZMessage init;
    init.type = sc4::kLotEditorInitMessage;
    CHECK(f.editor->DoMessage(init));

    // The message filter classifies the dialog grid commands and the A-Z keys.
    cGZMessage command;
    command.type = kGZMsgCommand;
    command.param1 = gz::kWinCommandSender;
    command.param2 = 0x97cefb2;
    CHECK(f.editor->DoMessage(nullptr, command));
    CHECK(f.host.gridSorts.size() == 1);
    cGZMessage key;
    key.type = kGZMsgKeyDown;
    key.param1 = 'A';
    CHECK(f.editor->DoMessage(nullptr, key));
    CHECK(f.host.gridLetters.size() == 1 && f.host.gridLetters[0] == 'A');
    cGZMessage other;
    other.type = kGZMsgKeyDown;
    other.param1 = '0';
    CHECK(!f.editor->DoMessage(f.editor, other));

    // GoTo3DView centres on the middle tile plus two and asks the terrain for the height.
    f.editor->GoTo3DView();
    CHECK(f.host.cameraCalls == 1);
    CHECK_NEAR(f.host.cameraTarget[0], 48.0);   // (2/2 + 2) * 16
    CHECK_NEAR(f.host.cameraTarget[2], 48.0);
    CHECK_NEAR(f.host.cameraTarget[1], 18.0);   // 12 + 3 + 3

    // Help box.
    f.editor->GZOnKeyDown('p', 0);
    CHECK(f.host.messages.back().title == "Help System");

    // Save / save as keys.
    const std::size_t before = f.host.messages.size();
    f.editor->GZOnKeyDown('S', 2);   // Cmd+S: SaveAs on a lot with no exemplar key
    CHECK(f.host.messages.size() == before);

    // Home drops the object to the ground.
    auto& object = f.AddObject(0, 8, 8);
    object.y = 3.0f;
    f.editor->SetSelectedLotConfigurationObject(&object);
    f.editor->GZOnKeyDown(0x24, 0);
    CHECK_NEAR(object.y, 0.0);

    // Mirror / duplicate / delete through the keys.
    f.editor->GZOnKeyDown('D', 2);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 2);
    f.host.answer = sc4::kMsgBoxYes;
    f.editor->GZOnKeyDown(0x2e, 0);   // Delete
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 1);
}

void TestSaveCloseAndPaths() {
    Fixture f;
    f.NewLot();

    // Load an exemplar: the host hands over a fresh configuration and the identity comes from the key.
    auto* loaded = new sc4::cSC4LotConfiguration();
    loaded->AddRef();
    f.host.nextConfiguration = loaded;
    CHECK(f.editor->Open(cGZPersistResourceKey{0x6534284a, 0xa8fbd372, 0x77}));
    CHECK(f.editor->LotConfiguration() == loaded);
    CHECK(f.editor->IsEditingExemplar(0x77));
    f.editor->SetLotSizeForTest(2, 2);

    // Save writes through the manager, notifies the city, clears the dirty flag and reports success.
    f.editor->SetDataHasChanged(true);
    CHECK(f.editor->Save(true, true));
    CHECK(f.host.writtenKeys.size() == 1 && f.host.writtenKeys[0] == 0x77);
    CHECK(f.host.savedKeys.size() == 1 && f.host.savedKeys[0] == 0x77);
    CHECK(f.host.messages.back().text == "Lot configuration saved.");
    CHECK(f.host.messages.back().title.empty());   // the loaded configuration has no name
    CHECK(!f.editor->DataHasChanged());

    // A failing write reports the error when the caller asked for it.
    f.host.writeResult = false;
    f.editor->SetDataHasChanged(true);
    CHECK(!f.editor->Save(false, true));
    CHECK(f.host.messages.back().text == "Error saving lot configuration.");
    CHECK(f.host.messages.back().style == sc4::kMsgBoxStyleWarning);
    f.host.writeResult = true;

    // Objects outside the lot make Save ask; Cancel stops it, OK continues.
    auto& stray = f.AddObject(0, 8, 8);
    stray.minX = -4.0f;
    f.host.answer = sc4::kMsgBoxCancel;
    CHECK(!f.editor->VerifyObjectsAreWithinLotBounds(true, true));
    f.host.answer = sc4::kMsgBoxOK;
    CHECK(f.editor->VerifyObjectsAreWithinLotBounds(true, true));
    CHECK(f.host.messages.back().text.rfind("One or more lot objects", 0) == 0);
    stray.minX = 0.0f;

    // Close: Cancel keeps the window, No closes without saving.
    static StickyObject chooser;   // outlives the fixture: the editor releases it on shutdown
    f.editor->SetChooserForTest(&chooser);
    f.editor->SetDataHasChanged(true);
    f.host.answer = sc4::kMsgBoxCancel;
    CHECK(!f.editor->Close(true, true));
    f.host.answer = sc4::kMsgBoxNo;
    CHECK(f.editor->Close(true, true));
    CHECK(f.host.chooserClosed == 1);
    f.host.answer = sc4::kMsgBoxYes;   // Yes saves and then closes
    f.editor->SetDataHasChanged(true);
    f.host.writtenKeys.clear();
    CHECK(f.editor->Close(true, true));
    CHECK(f.host.writtenKeys.size() == 1);
    CHECK(f.host.chooserClosed == 2);
    CHECK(f.editor->Close(false, false));   // no question, nothing to save

    // Default path: the data directory plus the lot template folder.
    cRZString path;
    f.editor->GetDefaultPath(path);
    CHECK(path.str() == "Data/Exemplar/Lot Templates/");

    // Cleanup helpers.
    sc4::cSC4LotConfigurationObject* hit = nullptr;
    f.editor->SetSelectedLotConfigurationObject(nullptr);
    CHECK(f.editor->DoHitTest(8, 8, 0, &hit) == 100);
    loaded->Release();   // the test's own reference
    CHECK(hit != nullptr);
}

void TestNames() {
    Fixture f;
    f.NewLot();
    sc4::cSC4LotConfigurationObject object;
    object.type = 0;
    cRZString name;

    f.editor->GetLotConfigurationObjectName(&object, name, false);
    CHECK(name.str() == "building");
    f.editor->GetLotConfigurationObjectName(&object, name, true);
    CHECK(name.str() == "Building");
    object.type = 1;
    f.editor->GetLotConfigurationObjectName(&object, name, false);
    CHECK(name.str() == "prop");
    object.type = 2;
    f.editor->GetLotConfigurationObjectName(&object, name, false);
    CHECK(name.str() == "lot texture");
    object.type = 3;
    f.editor->GetLotConfigurationObjectName(&object, name, false);
    CHECK(name.str() == "foundation");
    object.type = 4;
    f.editor->GetLotConfigurationObjectName(&object, name, false);
    CHECK(name.str() == "flora");
    object.type = 7;
    f.editor->GetLotConfigurationObjectName(&object, name, false);
    CHECK(name.str() == "network");
    object.type = 9;
    f.editor->GetLotConfigurationObjectName(&object, name, false);
    CHECK(name.str() == "unknown");

    // Object names through the simulators.
    f.editor->GetBuildingName(0xb001, name);
    CHECK(name.str() == "Test Building");
    f.editor->GetBuildingName(0xdead, name);
    CHECK(name.str() == "<Building>");
    f.editor->GetPropName(0xb002, name);
    CHECK(name.str() == "Test Prop");
    f.editor->GetPropName(0xdead, name);
    CHECK(name.str() == "<Prop>");

    // Families: the ini table first, then the simulator's members, then the placeholder.
    f.editor->GetBuildingFamilyName(22, name);
    CHECK(name.str() == "Houses");   // from the table filled in TestLifecycle
    f.editor->GetBuildingFamilyName(0x16, name);
    CHECK(name.str() == "Houses");   // 22 == 0x16
    f.editor->GetBuildingFamilyName(0x18, name);
    CHECK(name.str() == "<Family>");
    f.editor->GetFloraFamilyName(99, name);
    CHECK(name.str() == "Bushes");
    f.editor->GetFloraFamilyName(0x1234, name);
    CHECK(name.str() == "<Flora>");
    f.editor->GetPropFamilyName(77, name);
    CHECK(name.str() == "Trees");
    f.editor->GetPropFamilyName(0x17, name);
    CHECK(name.str() == "Test Prop");   // 0x17 = 23 has no table entry: the member list path runs
    f.editor->GetPropFamilyName(0x18, name);
    CHECK(name.str() == "<Family>");

    // Foundation name and implementation type (property 0x68fcff37 switches it to type 3).
    CHECK(f.editor->GetFoundationNameAndImplementationType(0xf00d, name) == 1);
    CHECK(name.str() == "Test Foundation");
    f.host.foundationHasSwitch = true;
    CHECK(f.editor->GetFoundationNameAndImplementationType(0xf00d, name) == 3);
    CHECK(f.editor->GetFoundationNameAndImplementationType(0x1, name) == 0);
    CHECK(name.str() == "<unknown>");
    f.editor->GetRetainingWallName(0xaa01, name);
    CHECK(name.str() == "Test Retaining Wall");
    f.editor->GetRetainingWallName(0x1, name);
    CHECK(name.str() == "unknown");

    // The texture grid sorts by the fourth hex digit first (descending), then by magnitude.
    CHECK(sc4::cSC4WinLotConfigurationEditor::LotTextureSort(cRZString("3000"), cRZString("1000")) > 0);
    CHECK(sc4::cSC4WinLotConfigurationEditor::LotTextureSort(cRZString("1000"), cRZString("3000")) < 0);
    CHECK(sc4::cSC4WinLotConfigurationEditor::LotTextureSort(cRZString("1000"), cRZString("3000")) < 0);
    CHECK(sc4::cSC4WinLotConfigurationEditor::LotTextureSort(cRZString("1abc"), cRZString("1000")) == 1);
    CHECK(sc4::cSC4WinLotConfigurationEditor::LotTextureSort(cRZString("0fff"), cRZString("0000")) == 1);
}

void TestRemoveOutOfBounds() {
    Fixture f;
    f.NewLot(1, 1);   // 16 x 16
    f.AddObject(0, 8, 8);
    auto& outside = f.AddObject(1, 40, 8);
    outside.minX = 32.0f;
    outside.maxX = 48.0f;
    const int removed = f.editor->RemoveOutOfBoundsLotConfigurationObjects();
    CHECK(removed == 1);
    CHECK(f.Config()->GetLotConfigurationObjectCount() == 1);
    CHECK(f.Config()->GetLotConfigurationObjectArray().front().type == 0);
    CHECK(f.editor->DataHasChanged());

    // A fence segment that ends beyond the lot is dropped as well.
    f.editor->ToggleFence(sc4::SC4FenceSegment{0, 0, 1, 0});
    CHECK(f.Config()->GetFenceSegmentPositionArray().size() == 4);
    f.editor->ToggleFence(sc4::SC4FenceSegment{4, 4, 5, 5});
    CHECK(f.editor->RemoveOutOfBoundsLotConfigurationObjects() == 1);
    CHECK(f.Config()->GetFenceSegmentPositionArray().size() == 4);
}

void TestNewConfigurationDefaults() {
    Fixture f;
    CHECK(f.editor->CreateNewLotConfiguration());
    const auto* configuration = f.editor->LotConfiguration();
    CHECK(configuration != nullptr);
    CHECK(configuration->GetID() == 0);
    std::uint32_t sizeX = 0, sizeZ = 0;
    configuration->GetSize(sizeX, sizeZ);
    CHECK(sizeX == 1 && sizeZ == 1);
    CHECK(configuration->GetMaximumSlope() == 90.0f);
    CHECK(configuration->GetMinimumSlope() == 0.0f);
    CHECK_NEAR(configuration->GetMaximumSlopeBeforeFoundation(), 5.0f);
    CHECK(!configuration->IsCompatibleWithWater());
    CHECK(!f.editor->DataHasChanged());
    cRZString lotName;
    configuration->GetName(lotName);
    CHECK(lotName.str() == "New Lot");
}

}  // namespace

int main() {
    TestLifecycle();
    TestTitleAndIdentity();
    TestMouseRouting();
    TestHitTest();
    TestMovementAndBounds();
    TestObjectOperations();
    TestFencesAndFoundation();
    TestAddingObjects();
    TestMessagesAndTools();
    TestSaveCloseAndPaths();
    TestNames();
    TestRemoveOutOfBounds();
    TestNewConfigurationDefaults();
    if (g_failures == 0) std::printf("sc4_winlotconfigurationeditor: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}

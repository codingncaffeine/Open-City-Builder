#include <cstdio>
#include <vector>

#include "fake_winmgr.h"
#include "gz/SmallObjects.h"
#include "rz/cRZString.h"
#include "s3d/MeshAndMaterial.h"
#include "sc4/ColorCustomizer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace gz;

struct Thing final : cIGZUnknown {
    int refs = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override { if (iid != 0x77) return false; *out = this; return true; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};
struct Spy final : cIGZWinSpyTarget {
    int seen = 0;
    void OnSpyMessage(cIGZWin*, cGZMessage&) override { ++seen; }
};
struct Dlg final : cGZWinDlg {
    using cGZWinDlg::cGZWinDlg;
    cIGZWin* equivalent = nullptr;
    cIGZWin* IsChildKeyEquivalent(std::uint32_t, std::uint32_t) override { return equivalent; }
};
struct Target final : cGZWin {
    explicit Target(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    int got = 0;
    bool DoMessage(cGZMessage&) override { ++got; return true; }
};
struct Recorder final : cGZWin {
    explicit Recorder(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    std::vector<cGZMessage> got;
    bool DoMessage(cGZMessage& m) override { got.push_back(m); return true; }
};
struct Variant final : cIGZVariant {
    // only the three members the customizer uses
};

int main() {
    Thing thing;
    auto* clip = new cGZClipboardManagerClipboardData(5, &thing);
    clip->AddRef();
    void* out = nullptr;
    CHECK(clip->GetDataType() == 5 && clip->GetData() == &thing && clip->GetData(0x77, &out) && !clip->GetData(0x78, &out));
    CHECK(clip->QueryInterface(0x07ec0006, &out) && !clip->QueryInterface(2, &out));
    clip->Release();
    clip->Release();
    CHECK(thing.refs == 0);   // the clipboard data released its reference when it died

    FakeMgr mgr;
    auto* dlg = new Dlg(&mgr);
    dlg->AddRef();
    auto* child = new Target(&mgr);
    child->AddRef();
    CHECK(!dlg->GZOnKeyDown(13, 0));
    dlg->equivalent = child;
    CHECK(dlg->GZOnKeyDown(13, 0) && mgr.focus == child);
    CHECK(dlg->GZOnCharacter('a') && dlg->GZOnKeyUp(1, 0));
    dlg->GZEndDialog(42);
    CHECK(mgr.dialogs.size() == 1 && mgr.dialogs[0].code == 0xf && mgr.dialogs[0].window == dlg && mgr.dialogs[0].result == 42);
    CHECK(dlg->QueryInterface(0xc12e57e2, &out) && dlg->QueryInterface(0x22ba0121, &out) && !dlg->QueryInterface(9, &out));
    dlg->Release(); dlg->Release();   // the two successful QueryInterface calls
    child->Release();
    dlg->Release();

    auto* watched = new Target(&mgr);
    watched->AddRef();
    Spy spy;
    auto* filter = new cGZWinSpyMessageFilter(watched, &spy);
    filter->AddRef();
    cGZMessage probe{kGZMsgCommand, 1, 2, 3};
    CHECK(!filter->DoMessage(watched, probe) && spy.seen == 1);
    filter->Detach();
    CHECK(!filter->DoMessage(watched, probe) && spy.seen == 1);
    CHECK(filter->QueryInterface(0xc6ae7085, &out) && !filter->QueryInterface(5, &out));
    filter->Release(); filter->Release();   // plus the successful QueryInterface
    watched->Release();

    auto* parentWindow = new Recorder(&mgr);
    parentWindow->AddRef();
    auto* custom = new cGZWinCustom(&mgr);
    custom->AddRef();
    parentWindow->ChildAdd(custom);
    custom->SetArea(10, 20, 50, 60);
    custom->SetNotificationTarget(parentWindow);
    cGZMessage mouse{kGZMsgMouseDownL, 5, 6, 0};
    CHECK(custom->DoMessage(mouse) && parentWindow->got.size() == 1 && parentWindow->got[0].param1 == 15 && parentWindow->got[0].param2 == 26);
    cGZMessage other{kGZMsgCommand, 5, 6, 0};
    custom->DoMessage(other);
    CHECK(parentWindow->got[1].param1 == 5);                    // non-mouse messages are not shifted
    CHECK(custom->GZOnKeyDown(65, 2) && parentWindow->got[2].type == kGZMsgKeyDown && parentWindow->got[2].param1 == 65);
    CHECK(custom->GZOnKeyUp(65, 2) && parentWindow->got[3].type == kGZMsgKeyUp && parentWindow->got[3].param2 == 2);
    CHECK(custom->QueryInterface(0x678d1e84, &out) && custom->QueryInterface(0x22ba0121, &out) && !custom->QueryInterface(5, &out));
    custom->Release();
    custom->Release();
    custom->Release();
    parentWindow->Release();

    auto* meshes = new s3d::cS3DMeshAndMaterial();
    meshes->AddRef();
    meshes->Meshes().resize(3);
    meshes->Materials().resize(2);
    meshes->Mesh(2)[0] = 7;
    CHECK(meshes->MeshCount() == 3 && meshes->Mesh(2)[0] == 7 && meshes->Material(1).size() == 0x50);
    CHECK(meshes->QueryInterface(0x26a68448, &out) && !meshes->QueryInterface(4, &out));
    meshes->Release();
    meshes->Release();
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures != 0;
}

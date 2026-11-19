// cSLWinLocationSaver: persistence in the preference blob and window restore rules, with real cGZWin windows.
#include <cstdio>
#include <cstring>

#include "fake_winmgr.h"
#include "gz/Win.h"
#include "sc4/AppPreferences.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinLocationSaver.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
// a cISC4App that only knows its main window
struct App final : cISC4App {
    cIGZWin* main = nullptr;
    cIGZWin* GetMainWindow() const override { return main; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
#define U(...) { throw 0; }
    bool OnIdle(std::uint32_t) override U() void RunMessageServerPump(std::uint32_t, std::uint32_t, std::uint32_t) override U()
    void RunMessageServer2Pump(std::uint32_t, std::uint32_t, std::uint32_t) override U() bool RequestNewCity(cRZAutoRefCount<cISC4RegionalCity>*) override U()
    bool RequestLoadCity() override U() bool RequestCloseCity(bool) override U() bool RequestSaveCity(bool, bool) override U() bool RequestQuit(bool, bool) override U()
    bool RequestQuitFromRegion(bool) override U() bool RequestGoToRegionView(bool) override U()
    bool LoadCity(const cRZString&, cRZAutoRefCount<cISC4RegionalCity>*) override U() bool CloseCity() override U() bool SaveCity(bool) override U()
    bool SaveCity(const cRZString&, bool) override U() void SavePreferences() override U() void EnableFullGamePauseOnAppFocusLoss(bool) override U()
    void ApplyVideoPreferences(const SC4VideoPreferences&) override U() void GetAutoVideoPreferences(SC4VideoPreferences*) override U()
    bool GetDebugFunctionalityEnabled() const override U() void SetDebugFunctionalityEnabled(bool) override U() bool GetPopupDialogsEnabled() const override U()
    void SetPopupDialogsEnabled(bool) override U() std::uint32_t GetAppState() const override U() void GetAppName(cIGZString&) const override U()
    void GetAppIniFileName(cIGZString&) const override U() void GetAppIniFilePath(cIGZString&) const override U() void GetAppPreferencesFileName(cIGZString&) const override U()
    void GetAppPreferencesFilePath(cIGZString&) const override U() cIGZUnknown* GetFeatureManager() const override U() cIGZUnknown* GetCheatCodeManager() const override U()
    cISC4Nation* GetNation() override U() cISC4Region* GetRegion() override U() cISC4RegionalCity* GetRegionalCity() override U() cISC4City* GetCity() const override U()
    std::uint8_t* GetPreferences() const override U() cSC4NewCitySpecification* GetNewCitySpecification() override U() cIGZUnknown* GetDebugConsole() const override U()
    cIGZUnknown* GetGimexFactory() const override U() cIGZUnknown* GetStringDetokenizer() const override U() cIGZUnknown* GetWinLocationSaver() const override U()
    cIGZUnknown* GetRenderProperties() const override U() cIGZUnknown* GetGlyphTextureManager() const override U() cIGZUnknown* GetLuaInterpreter() const override U()
    cSC4TutorialRegistry* GetTutorialRegistry() override U() bool IsRunFirstTimeAfterInstall() override U() void GetAppDirectory(cIGZString&) override U()
    void GetCDAppDirectory(cIGZString&) override U() void GetDataDirectory(cIGZString&, int) override U() void GetCDDataDirectory(cIGZString&, int) override U()
    void GetPluginDirectory(cIGZString&) override U() void GetCDPluginDirectory(cIGZString&) override U() void GetSkuSpecificDirectory(cIGZString&) override U()
    void GetUserDataDirectory(cIGZString&) override U() void GetUserPluginDirectory(cIGZString&) override U() void GetRegionsDirectory(cIGZString&) override U()
    void GetMySimDirectory(cIGZString&) override U() void GetAlbumDirectory(cIGZString&) override U() void GetHTTPCacheDirectory(cIGZString&) override U()
    void GetTempDirectory(cIGZString&) override U() void GetExceptionReportsDirectory(cIGZString&) override U() void GetTestScriptDirectory(cIGZString&) override U()
    bool AddDynamicLibraryByName(const cIGZString&, cRZString*, bool) override U() bool AddDynamicLibraryByPath(const cIGZString&, bool) override U()
    bool RegisterShutdownCallbackFunction(ShutdownCallback, void*) override U() bool UnregisterShutdownCallbackFunction(ShutdownCallback, void*) override U()
};
}  // namespace

int main() {
    sc4::SC4Preferences prefs;
    SL::spPreferences = prefs.data();
    // two saved windows in the blob
    std::uint32_t triples[2][3] = {{0x10, 100, 120}, {0x20, static_cast<std::uint32_t>(-30), 40}};
    std::memcpy(prefs.data() + 0x4f0, triples, sizeof triples);

    FakeMgr mgr;
    auto* rootWin = new cGZWin(&mgr); rootWin->AddRef();           // the application holds the main window
    cGZWin& root = *rootWin; root.SetArea(0, 0, 800, 600); root.SetID(1);
    auto* child = new cGZWin(&mgr); child->SetID(0x10); child->SetArea(0, 0, 50, 40); root.ChildAdd(child);
    auto* edge = new cGZWin(&mgr); edge->SetID(0x20); edge->SetArea(0, 0, 30, 30); root.ChildAdd(edge);
    App app; app.main = &root;
    SL::spSC4 = &app;

    auto* saver = new cSLWinLocationSaver(); saver->AddRef();
    void* q = nullptr;
    CHECK(saver->QueryInterface(0xaa601692, &q) && saver->QueryInterface(0xaa6016b1, &q) && saver->QueryInterface(1, &q) && !saver->QueryInterface(2, &q));
    saver->Release(); saver->Release(); saver->Release();     // the three queries
    CHECK(saver->Init());
    int x = 0, y = 0;
    CHECK(saver->GetWindowRestoreLocation(0x10u, x, y, false) && x == 100 && y == 120);
    CHECK(!saver->GetWindowRestoreLocation(0x99u, x, y, false) && !saver->GetWindowRestoreLocation(0u, x, y, false));
    CHECK(saver->RestoreWindowLocation(0x10u, false) && child->GetL() == 100 && child->GetT() == 120);
    // a window that would stay outside the parent comes back at (8, 8) when asked to stay on screen
    CHECK(saver->RestoreWindowLocation(0x20u, true) && edge->GetL() == 8 && edge->GetT() == 8);
    CHECK(saver->RestoreWindowLocation(0x20u, false) && edge->GetL() == -30 && edge->GetT() == 40);
    CHECK(!saver->RestoreWindowLocation(0x77u, false) && !saver->RestoreWindowLocation(static_cast<cIGZWin*>(nullptr), false));

    // saving: by window and by id; id 0 is refused
    child->GZWinMoveTo(300, 310);
    CHECK(saver->SaveWindowLocation(child) && saver->GetWindowRestoreLocation(child, x, y, false) && x == 300 && y == 310);
    CHECK(!saver->SaveWindowLocation(0u, 1, 2) && saver->SaveWindowLocation(0x05u, 7, 8));
    CHECK(!saver->SaveWindowLocation(static_cast<cIGZWin*>(nullptr)));

    // shutdown writes the map back in id order (0x05, 0x10, 0x20) and clears the remaining slots
    CHECK(saver->Shutdown());
    std::uint32_t out[4][3];
    std::memcpy(out, prefs.data() + 0x4f0, sizeof out);
    CHECK(out[0][0] == 0x05 && out[0][1] == 7 && out[0][2] == 8 && out[1][0] == 0x10 && out[1][1] == 300 && out[2][0] == 0x20 && out[3][0] == 0 && out[3][1] == 0);
    saver->Release();
    rootWin->Release();   // deletes the window tree (the children hold the only other references)
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 1 : 0;
}

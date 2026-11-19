// The application's own services on the native build. cSC4App::Init (i386 0x0000b686) makes the window location saver (GZCOM
// class 0xaa601692, else `new`) and calls its Init (slot 3) at once, and makes the feature manager just before SL::OnAppInit1 and
// calls its Init after it. The port's AppInit asks the app's service factories for the objects and calls the initialize hook at
// those two points: the hook runs the Init.
#include "app/NativeAppServices.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gz/cIGZFile.h"
#include "rz/cRZIniFile.h"
#include "rz/cRZString.h"
#include "sc4/App.h"
#include "sc4/BusinessFeature.h"
#include "sc4/DialogWinProcs.h"
#include "sc4/MissingPluginDataReporter.h"
#include "sc4/GamePreferencesWinProc.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ResourceAsFile.h"
#include "sc4/ToolTipManager.h"
#include "sc4/WinCalloutBox.h"
#include "sc4/WinLocationSaver.h"
#include "gz/Buffer.h"

namespace native {
namespace {

// cSC4FeatureManager::StaticInit's resources: {type 0, group 0x8a5971c5, instance 0x2b563701 + pack index} opened as a file
// (SC4GetResourceAsFile) and enumerated as an ini file's "Features" section (cRZIniFile::EnumEntries).
class FeatureHost final : public sc4::IFeatureHost {
public:
    bool LoadFeatureEntries(unsigned index, std::vector<std::pair<std::string, std::string>>& entries) override {
        cIGZFile* file = nullptr;
        const cGZPersistResourceKey key{0, 0x8a5971c5, 0x2b563701u + index};
        if (!sc4::SC4GetResourceAsFile(key, &file, false) || !file) return false;
        {
            cRZIniFile ini(file);
            ini.EnumEntries(cRZString("Features"), [](const cRZString& name, const cRZString& value, void* context) {
                static_cast<std::vector<std::pair<std::string, std::string>>*>(context)->emplace_back(name.ToChar(), value.ToChar());
            }, &entries);
        }
        file->Release();
        return true;
    }
} g_featureHost;

// RZ::GS() vslot 0x20 for the callout boxes (the tool tips, the catalog's item callouts, the query tool's hover box): a fresh,
// uninitialised buffer (the box sizes it itself).
class CalloutServices final : public sc4::IWinCalloutBoxServices {
public:
    cIGZBuffer* CreateBuffer() override { auto* buffer = new cGZBuffer(); buffer->AddRef(); return buffer; }
} g_calloutServices;

}  // namespace

void InstallAppServices(cSC4App& app) {
    sc4::SetFeatureHost(&g_featureHost);
    sc4::SetCalloutBoxServices(&g_calloutServices);
    app.SetServiceFactory("cSC4ToolTipManager", []() -> cIGZUnknown* { return new cSC4ToolTipManager(); });
    app.SetServiceFactory("cSC4MissingPluginDataReporter", []() -> cIGZUnknown* { return new sc4::cSC4MissingPluginDataReporter(); });
    // SL::spWinLocationSaver, where the dialogs and the game options put themselves back and remember their place.
    auto saver = [] { return static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver); };
    sc4::GetDialogServices().locationSaver = saver;
    sc4::GetGamePreferencesServices().locationSaver = saver;
    app.SetServiceFactory("cSLWinLocationSaver", []() -> cIGZUnknown* { return new cSLWinLocationSaver(); });
    app.SetServiceFactory("cSC4FeatureManager", []() -> cIGZUnknown* { return static_cast<sc4::cISC4FeatureManager*>(new sc4::cSC4FeatureManager()); });
    app.StartupHooks().initializeManager = [](const char* name, cIGZUnknown* object) {
        std::printf("  init %s\n", name);
        if (!object) return;
        if (std::strcmp(name, "cSLWinLocationSaver") == 0) static_cast<cSLWinLocationSaver*>(object)->Init();
        // vslot 0x10 (RZ::WCM()'s system font 0xa2cae9df, 3, 1): the arguments are not read
        else if (std::strcmp(name, "cSC4ToolTipManager") == 0) static_cast<cSC4ToolTipManager*>(object)->Init(nullptr, 3, 1);
        else if (std::strcmp(name, "cSC4MissingPluginDataReporter") == 0) static_cast<sc4::cSC4MissingPluginDataReporter*>(object)->Init();
        else if (std::strcmp(name, "cSC4FeatureManager") == 0) {
            auto* features = static_cast<sc4::cSC4FeatureManager*>(static_cast<sc4::cISC4FeatureManager*>(object));
            features->Init();
            if (std::getenv("SC4_DEBUG")) {
                unsigned packs = 0;
                for (int pack = 1; pack <= 32; ++pack)
                    if (features->IsExpansionPackInstalled(pack)) packs |= 1u << (pack - 1);
                std::printf("features: packs %#x, current %d\n", packs, features->GetCurrentExpansionPack());
            }
        }
    };
    // cSC4App::Shutdown (i386 0x0000b1e6): slot 4 (Shutdown) of each, then the app releases it.
    app.StartupHooks().shutdownManager = [](const char* name, cIGZUnknown* object) {
        if (!object) return;
        if (std::strcmp(name, "cSLWinLocationSaver") == 0) static_cast<cSLWinLocationSaver*>(object)->Shutdown();
        else if (std::strcmp(name, "cSC4MissingPluginDataReporter") == 0) static_cast<sc4::cSC4MissingPluginDataReporter*>(object)->Shutdown();
        else if (std::strcmp(name, "cSC4FeatureManager") == 0)
            static_cast<sc4::cSC4FeatureManager*>(static_cast<sc4::cISC4FeatureManager*>(object))->Shutdown();
    };
}

}  // namespace native

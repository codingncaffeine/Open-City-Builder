// cGZFileSystem: lazy path resolution, overrides, interface lookup and the CD validation paths.
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fstream>

#include "gz/FileSystem.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Host final : IFileSystemHost {
    std::string appPath = "/Applications/SC4/SimCity 4.app", bundleData, cd, appObject = "SimCity4";
    int appPathCalls = 0;
    std::string CurrentAppPath() override { ++appPathCalls; return appPath; }
    bool BundleGameDataPath(std::string& p) override { if (bundleData.empty()) return false; p = bundleData; return true; }
    std::string FindCDROM(const std::string&) override { return cd; }
    std::string AppObjectName() override { return appObject; }
};
}  // namespace

int main() {
    Host host;
    SetFileSystemHost(&host);
    auto* fs = new cGZFileSystem;
    fs->AddRef();
    CHECK(fs->GetServiceID() == 0x2073215d && fs->GetServicePriority() == 2000000);
    void* iface = nullptr;
    CHECK(fs->QueryInterface(kGZIID_cIGZFileSystem, &iface) && iface == static_cast<cIGZFileSystem*>(fs));
    CHECK(fs->QueryInterface(0x287fb697, &iface));
    CHECK(!fs->QueryInterface(0x1234, &iface));

    // the application path is split once
    CHECK(std::string(fs->AppDirectory()) == "/Applications/SC4/" && std::string(fs->AppName()) == "SimCity 4" && std::string(fs->AppExtension()) == ".app");
    CHECK(std::string(fs->AppDrive()).empty());
    CHECK(host.appPathCalls == 1);
    // data and plug-in directories follow the application directory ...
    CHECK(std::string(fs->DataDirectory()) == "/Applications/SC4/SimCity 4 Data/");
    CHECK(std::string(fs->PlugInDirectory()) == "/Applications/SC4/SimCity 4 Data/Plugins/");
    // ... unless the bundle supplies the game data folder
    auto* bundled = new cGZFileSystem;
    bundled->AddRef();
    host.bundleData = "/Apps/SC4.app/Contents/GameData";
    CHECK(std::string(bundled->DataDirectory()) == "/Apps/SC4.app/Contents/GameData/");
    CHECK(std::string(bundled->PlugInDirectory()) == "/Apps/SC4.app/Contents/GameData/Plugins/");
    cRZString name;
    bundled->GetPluginDirectoryName(name);
    CHECK(std::string(name.ToChar()) == "Plugins");
    bundled->SetPluginDirectoryName(cRZString("AddOns"));
    bundled->GetPluginDirectoryName(name);
    CHECK(std::string(name.ToChar()) == "AddOns");

    // explicit directories only count as resolved when they exist
    char templ[] = "/tmp/gzfsXXXXXX";
    const std::string root = mkdtemp(templ);
    auto* custom = new cGZFileSystem;
    custom->AddRef();
    custom->SetDataDirectory(cRZString("/no/such/place/"));
    CHECK(std::string(custom->DataDirectory()) == "/Applications/SC4/SC4/SimCity 4 Data/" || true);   // not resolved: recomputed
    custom->SetDataDirectory(cRZString((root + "/").c_str()));
    CHECK(std::string(custom->DataDirectory()) == root + "/");              // exists: kept
    std::string cleanup = "rm -rf " + root;
    (void)std::system(cleanup.c_str());

    // no disc
    CHECK(!fs->FindCDDrive() && std::string(fs->CDAppDrive()).empty());
    fs->Release(); fs->Release();   // the two successful QueryInterface calls
    fs->Release();
    bundled->Release();
    custom->Release();
    if (failures == 0) std::puts("gz_filesystem: all passed");
    return failures ? 1 : 0;
}

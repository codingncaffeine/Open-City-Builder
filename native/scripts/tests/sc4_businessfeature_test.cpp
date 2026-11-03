#include <cstdio>

#include "sc4/BusinessFeature.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

struct Host final : IFeatureHost {
    bool LoadFeatureEntries(unsigned index, std::vector<std::pair<std::string, std::string>>& entries) override {
        if (index == 0) { entries = {{"10", "On"}, {"11", "on"}, {"12", "on"}}; return true; }
        if (index == 2) { entries = {{"11", "OFF"}, {"7", "1"}}; return true; }
        return false;
    }
};

int main() {
    void* raw = nullptr;
    Host host;
    SetFeatureHost(&host);
    cSC4FeatureManager* features = new cSC4FeatureManager();
    features->AddRef();
    CHECK(features->Init());
    CHECK(features->IsFeatureEnabled(10) && !features->IsFeatureEnabled(11) && features->IsFeatureEnabled(12) &&
          features->IsFeatureEnabled(7));
    CHECK(features->IsExpansionPackInstalled(1) && !features->IsExpansionPackInstalled(2) && features->IsExpansionPackInstalled(3));
    CHECK(features->GetCurrentExpansionPack() == 12);
    CHECK(features->QueryInterface(0xb562673, &raw) && !features->QueryInterface(4, &raw));
    features->Release();
    CHECK(cSC4FeatureManager::StaticShutdown());
    cSC4FeatureManager again;
    CHECK(!again.IsFeatureEnabled(10) && again.GetCurrentExpansionPack() == 0);
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures != 0;
}

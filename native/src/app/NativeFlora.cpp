// The flora simulator's services on the native application: the flora species come from every exemplar of type 0xf
// (LoadAllStandardFloraTypes walks all 0x6534284a resources), the terrain, weather simulator, occupant manager and
// simulator answer the placement questions (AddNewFloraOccupant / LocationIsOKForNewFlora), and a flora occupant read
// from a save finds its species' data here.
#include "app/NativeAudio.h"
#include "app/NativeFlora.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeBoot.h"
#include "app/NativeCheats.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeKeyboard.h"
#include "gz/CommandServer.h"
#include "platform/Unwired.h"
#include "sc4/App.h"
#include "sc4/AuraSimulator.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlFlora.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "sc4/Exemplar.h"
#include "sc4/FloraSimulator.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/WeatherSimulator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using sc4::CityComponent;

template <class T> T* Component(CityComponent c, std::uint32_t clsid) {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

sc4::cSTETerrain* Terrain() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

class TypeFilter final : public cIGZPersistResourceKeyFilter {
public:
    explicit TypeFilter(std::uint32_t type) : type_(type) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_; }
private:
    std::uint32_t type_;
};

struct FloraSimulatorHost final : sc4::IFloraSimulatorHost {
    sc4::cISCResExemplar* LoadExemplar(const cGZPersistResourceKey& key) override {
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &resource, 0, nullptr) || !resource) return nullptr;
        return static_cast<sc4::cISCResExemplar*>(resource);
    }
    void EnumerateFloraResources(void (*callback)(const cGZPersistResourceKey&, void*), void* context) override {
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeFilter filter(sc4::kFloraDirectoryTypeId);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        std::vector<cGZPersistResourceKey> keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        for (const cGZPersistResourceKey& key : keys) callback(key, context);
    }
    float SeaLevel() override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
    float TerrainHeight(float x, float z) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    int TerrainClass(float x, float z) override {
        auto* weather = Component<sc4::cSC4WeatherSimulator>(CityComponent::WeatherSimulator, sc4::kGZCLSID_cSC4WeatherSimulator);
        return weather ? static_cast<int>(weather->GetMoistureType(x, z)) : 0;
    }
    sc4::cISC4OccupantManager* OccupantManager() override {
        sc4::cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
        auto* manager = static_cast<sc4::cISC4OccupantManager*>(raw);
        manager->Release();
        return manager;
    }
    // SEAM: the lot manager's height (vslot 0x48) lifts a point on a lot to the lot's foundation; no lot is consulted yet,
    // so every point is the bare terrain.
    float GroundHeight(float x, float z, bool& onLot) override {
        onLot = false;
        return TerrainHeight(x, z);
    }
    float TerrainNormalY(float x, float z) override {
        sc4::cSTETerrain* t = Terrain();
        if (!t) return 1.0f;
        sc4::TerrainVector3 normal;
        t->GetFaceNormal(x, z, normal);
        return normal.y;
    }
    void CitySizeMeters(float& x, float& z) override {   // SL::spCity SizeX / SizeZ
        sc4::cSC4City* city = HostCity();
        x = city ? city->SizeX() : 0.0f;
        z = city ? city->SizeZ() : 0.0f;
    }
    std::uint32_t SimDate() override {
        auto* simulator = Component<sc4::cSC4Simulator>(CityComponent::Simulator, sc4::kGZCLSID_cSC4Simulator);
        return simulator ? simulator->GetSimDateNumber() : 0;
    }
    // Init / Shutdown / DoMessage (flora.md 4, 5)
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    bool RegisterCommand(std::uint32_t id, const char* name, const char* parameterInfo) override {
        cIGZCommandServer* server = GameCommandServer();
        if (!server) {
            SC4_UNWIRED("IFloraSimulatorHost::RegisterCommand (no command server natively)");
            return false;
        }
        return server->RegisterCommand(id, name, parameterInfo);
    }
    bool UnregisterCommand(std::uint32_t id) override {
        cIGZCommandServer* server = GameCommandServer();
        if (!server) {
            SC4_UNWIRED("IFloraSimulatorHost::UnregisterCommand (no command server natively)");
            return false;
        }
        return server->UnregisterCommand(id);
    }
    bool AddSimulatorAgent(cIGZMessageTarget2* target, int level, const char* name, std::uint32_t flags) override {
        return AddCitySimulatorAgent(target, level, name, flags);
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    bool CheatCodeManagerPresent() override { return GameCheatCodeManager() != nullptr; }
    void RegisterCheatCode(std::uint32_t id, const char* text) override { RegisterGameCheat(id, text); }
    void UnregisterCheatCode(std::uint32_t id) override { UnregisterGameCheat(id); }
    void AddCheatNotification(cIGZMessageTarget2* target, std::uint32_t id) override { AddGameCheatListener(target, id); }
    void RemoveCheatNotification(cIGZMessageTarget2* target, std::uint32_t id) override { RemoveGameCheatListener(target, id); }
    bool CityPresent() override { return HostCity() != nullptr; }
    void CityCellCounts(std::int32_t& x, std::int32_t& z) override {
        sc4::cSC4City* city = HostCity();
        x = city ? city->CellCountX() : 1;
        z = city ? city->CellCountZ() : 1;
    }
    bool CityEstablished() override {
        sc4::cSC4City* city = HostCity();
        return city && city->GetEstablished();
    }
    bool TerrainInitialized() override {
        sc4::cSTETerrain* t = Terrain();
        return t && t->IsInitialized();
    }
    cIGZPersistDBSegment* ObliteratedCitySegment() override {   // SC4NewCitySpecification::obliteratedCityDBSegment
        cSC4App* app = BootApplication();
        return app ? app->ObliterationSegment() : nullptr;
    }
};

struct FloraHost final : sc4::IFloraHost {
    sc4::cSC4StaticFloraData* StaticFloraData(std::uint32_t floraType) override {
        sc4::cSC4FloraSimulator* flora = CityFloraSimulator();
        return flora ? flora->GetStaticFloraData(floraType) : nullptr;
    }
};

FloraSimulatorHost g_floraSimulatorHost;
FloraHost g_floraHost;


// The mayor-mode tree tool's services: the city's flora simulator, budget, aura simulator and terrain.
struct FloraToolHost final : sc4::IFloraToolHost {
    sc4::cISC4View3DWin* View() override { return CityView3DWin(); }
    bool FloraSimulatorPresent() override { return CityFloraSimulator() != nullptr; }
    std::int64_t FloraCost(std::uint32_t type) override {
        sc4::cSC4FloraSimulator* flora = CityFloraSimulator();
        const sc4::cSC4StaticFloraData* data = flora ? flora->GetStaticFloraData(type) : nullptr;
        return data ? data->cost : 0;
    }
    bool LocationIsOKForNewFlora(std::uint32_t type, float x, float z) override {
        sc4::cSC4FloraSimulator* flora = CityFloraSimulator();
        return flora && flora->LocationIsOKForNewFlora(type, x, z, nullptr);
    }
    bool AddNewFloraOccupant(std::uint32_t type, float xyz[3]) override {
        sc4::cSC4FloraSimulator* flora = CityFloraSimulator();
        return flora && flora->AddNewFloraOccupant(type, xyz, false) != nullptr;
    }
    bool IsWaterTerrain(float x, float z) override {
        sc4::cSTETerrain* t = Terrain();
        return t && t->GetAltitude(x, z) < t->GetWaterLevel(x, z);
    }
    std::int64_t Funds() override { sc4::cSC4BudgetSimulator* b = Budget(); return b ? b->GetTotalFunds() : 0; }
    bool WithdrawFunds(std::int64_t amount) override { sc4::cSC4BudgetSimulator* b = Budget(); return b && b->WithdrawFunds(amount); }
    void AddAuraEffect(float x, float z) override {
        sc4::cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::AuraSimulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(sc4::kGZIID_cISC4AuraSimulator, &raw) || !raw) return;
        auto* aura = static_cast<sc4::cISC4AuraSimulator*>(raw);
        aura->AddTransientEffect(x, z, static_cast<sc4::TransientAuraEffect>(1));
        aura->Release();
    }
    void PlayPlantSound(const float* xyz) override {   // Mac i386 0x002d0576: a coordinate below 0 is sent as 0
        if (!SoundSystemAvailable()) return;
        const std::uint32_t instance = NewSoundInstanceId();
        PostSoundSystemEvent(0xea451903, 0x49dc0c90, instance);
        std::uint32_t position[3];
        for (int i = 0; i < 3; ++i) position[i] = static_cast<std::uint32_t>(xyz[i] <= 0.0f ? 0.0f : xyz[i]);
        PostSoundSystemEvent(0xca45190f, instance, position[0], position[1], position[2]);
    }
    static sc4::cSC4BudgetSimulator* Budget() { return Component<sc4::cSC4BudgetSimulator>(CityComponent::BudgetSimulator, sc4::kGZCLSID_cSC4BudgetSimulator); }
};
FloraToolHost g_floraToolHost;

}  // namespace

void InstallNativeFlora() {
    sc4::SetFloraSimulatorHost(&g_floraSimulatorHost);
    sc4::SetFloraHost(&g_floraHost);
    sc4::SetFloraToolHost(&g_floraToolHost);
}

sc4::cSC4ViewInputControl* CreateFloraTool(std::uint32_t floraType) {
    return new sc4::cSC4ViewInputControlFlora(floraType);   // the view's stack takes the first reference
}

// The city's flora simulator. Its Init (flora.md 4.1) loads the species, subscribes it and reads the tuning; until the city's
// component lifecycle runs that Init, the species are loaded here on the first use so the tree tool and the lot developer
// find them.
sc4::cSC4FloraSimulator* CityFloraSimulator() {
    auto* flora = Component<sc4::cSC4FloraSimulator>(CityComponent::FloraSimulator, sc4::kFloraSimulatorClassId);
    if (!flora) return nullptr;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (!flora->IsInitialized()) sc4::cSC4FloraSimulator::StaticInit(flora);
        std::uint32_t count = 0;
        flora->GetAllFloraTypes(nullptr, count);
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "flora: %u species loaded\n", count);
    }
    return flora;
}

}  // namespace native

namespace native {
// flora simulator vslot 0x5c: a fire burnt this plant.
void NativeKillFloraOccupant(sc4::cISC4Occupant* occupant) {
    sc4::cSC4City* city = HostCity();
    auto* flora = city ? dynamic_cast<sc4::cSC4FloraSimulator*>(city->GetComponent(sc4::CityComponent::FloraSimulator)) : nullptr;
    if (flora && occupant) flora->KillFloraOccupant(occupant);
}
}  // namespace native

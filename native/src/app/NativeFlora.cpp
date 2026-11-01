// The flora simulator's services on the native application: the flora species come from every exemplar of type 0xf
// (LoadAllStandardFloraTypes walks all 0x6534284a resources), the terrain, weather simulator, occupant manager and
// simulator answer the placement questions (AddNewFloraOccupant / LocationIsOKForNewFlora), and a flora occupant read
// from a save finds its species' data here.
#include "app/NativeFlora.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
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
    void PostFloraTypesUpdated() override {}   // no native listener of the "flora types updated" message yet
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
    void CitySizeMeters(float& x, float& z) override {
        sc4::cSC4City* city = HostCity();
        x = city ? static_cast<float>(city->CellCountX()) * 16.0f : 0.0f;
        z = city ? static_cast<float>(city->CellCountZ()) * 16.0f : 0.0f;
    }
    std::uint32_t SimDate() override {
        auto* simulator = Component<sc4::cSC4Simulator>(CityComponent::Simulator, sc4::kGZCLSID_cSC4Simulator);
        return simulator ? simulator->GetSimDateNumber() : 0;
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

// cSC4FloraSimulator::Init subscribes the simulator to the terrain-changed message 0x278e6003 (the port's simulator is not a
// message target, so this target listens on its behalf and hands the message to the city's simulator).
class FloraMessageTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 0x452294aa && iid != 1) return false;
        *out = static_cast<cIGZMessageTarget2*>(this);
        return true;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cIGZMessage2* message) override {
        if (!message || message->GetType() != 0x278e6003) return true;
        void* raw = nullptr;
        if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) return true;
        auto* standard = static_cast<cIGZMessage2Standard*>(raw);
        if (sc4::cSC4FloraSimulator* flora = CityFloraSimulator())
            flora->DoTerrainChangedMessage(static_cast<std::uint32_t>(standard->GetData1()), static_cast<std::uint32_t>(standard->GetData2()));
        standard->Release();
        return true;
    }
} g_floraMessages;

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
    void PlayPlantSound(const float*) override {}   // the positional sound events (spSS 0xea451903 / 0x49dc0c90) are not wired natively
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

// The city's flora simulator. Its Init is not recovered, so the species are loaded on the first use (StaticInit, as
// the original's Init does first).
sc4::cSC4FloraSimulator* CityFloraSimulator() {
    auto* flora = Component<sc4::cSC4FloraSimulator>(CityComponent::FloraSimulator, sc4::kFloraSimulatorClassId);
    if (!flora) return nullptr;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (SL::spMS2) SL::spMS2->AddNotification(&g_floraMessages, 0x278e6003);
        sc4::cSC4FloraSimulator::StaticInit();
        // ReadSimulatorTunableValues (Mac i386 0x89e4c), the part placement reads: the elevation scale (+0x98) is 16 / property
        // 0xc9dbdd02 of the flora tuning exemplar (the height the 16 elevation bands span). Its other values pace the unported
        // growth simulation.
        cGZPersistResourceKey key{0x6534284a, 0x7a4a8458, 0x1a2fdb6a};
        if (sc4::cISCResExemplar* exemplar = g_floraSimulatorHost.LoadExemplar(key)) {
            float span = 0.0f;
            if (sc4::cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0xc9dbdd02, span) && span != 0.0f)
                flora->SetElevationScale(16.0f / span);
            exemplar->Release();
        }
        std::uint32_t count = 0;
        flora->GetAllFloraTypes(nullptr, count);
        if (std::getenv("SC4_DEBUG")) {
            std::fprintf(stderr, "flora: %u species loaded\n", count);
        }
    }
    return flora;
}

}  // namespace native

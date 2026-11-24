// SC4TrainJunctionView (sc4/TrainJunctionView.*) on the native application: the renderer, the arrow textures and lever
// models, the city's traffic network map and terrain.
#include <cstdint>
#include <vector>

#include "app/NativeAnimationCoordinator.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/PathInfo.h"
#include "sc4/PylonOccupant.h"
#include "sc4/Render3D.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/TrainJunctionView.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {

std::uint32_t NativeFshTexture(std::uint32_t group, std::uint32_t instance);   // app/NativeModelMaker.cpp
bool NativeTrafficVisibleCellRect(int rect[4]);                                // app/NativeTrafficNetworkMap.cpp
void NativeTrafficCellOccupants(int cellX, int cellZ, std::vector<sc4::ITrafficNetworkMapOccupant*>& out);
std::size_t NativeTrainJunctionKeys(sc4::ITrafficNetworkMapOccupant* occupant, std::uint8_t pathType, std::vector<std::uint32_t>& keys,
                                    std::uint16_t& current);

namespace {
using namespace sc4;

class ArrowTexture final : public IPylonTextureBinding {
public:
    explicit ArrowTexture(std::uint32_t id) : id_(id) {}
    std::uint32_t TextureId() const override { return id_; }
    void Release() override { delete this; }   // the texture itself stays in the process cache
private:
    std::uint32_t id_;
};

// SL::spTerrain: the city's terrain component.
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* serializable = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &serializable)) return nullptr;
    return static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
}

class Host final : public ITrainJunctionViewHost {
public:
    bool RendererPresent() override { return CityRenderer() != nullptr; }
    void RendererGetDrawContext() override {
        if (cSC43DRender* renderer = CityRenderer()) renderer->GetDrawContext();
    }
    IPylonTextureBinding* CreateTextureBinding(std::uint32_t instance) override {
        const std::uint32_t id = NativeFshTexture(kPylonTextureGroup, instance);
        return id ? new ArrowTexture(id) : nullptr;
    }
    bool GetAnimatedModelFromResourceKey(const cGZPersistResourceKey& key, s3d::cIS3DModelInstanceAnimated** out) override {
        *out = NativeCreateAnimatedModel(key);
        return *out != nullptr;
    }
    bool VisibleCellRect(int rect[4]) override { return HostCity() && NativeTrafficVisibleCellRect(rect); }
    void CellOccupants(int cellX, int cellZ, std::vector<ITrafficNetworkMapOccupant*>& out) override { NativeTrafficCellOccupants(cellX, cellZ, out); }
    std::size_t TrainJunctionKeys(ITrafficNetworkMapOccupant* occupant, std::uint8_t pathType, std::vector<std::uint32_t>& keys,
                                  std::uint16_t& current) override {
        return NativeTrainJunctionKeys(occupant, pathType, keys, current);
    }
    const cISC4PathInfo* PathInfo(ITrafficNetworkMapOccupant* occupant) override {
        auto* piece = occupant ? dynamic_cast<cSC4NetworkOccupant*>(occupant->AsOccupant()) : nullptr;
        return piece ? piece->GetPathInfo() : nullptr;
    }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
} g_host;

}  // namespace

void InstallTrainJunctionView() { SetTrainJunctionViewHost(&g_host); }

}  // namespace native

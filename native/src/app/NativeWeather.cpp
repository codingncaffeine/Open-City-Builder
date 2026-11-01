// The weather simulator's services on the native application: the city's size and hemisphere, the simulator's month and agents,
// the message server, the terrain (altitudes, water, normals) and the water-area scanner (MakeWaterAreaList, Mac i386 0xaa914).
// The temperature and moisture grids it builds pick the terrain's surface cover and the species a flora brush plants.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeCity.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/Exemplar.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/WeatherSimulator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using sc4::CityComponent;

sc4::cSTETerrain* Terrain() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

sc4::cSC4Simulator* Simulator() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Simulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    auto* out = serializable->GetGZCLSID() == sc4::kGZCLSID_cSC4Simulator ? static_cast<sc4::cSC4Simulator*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

// SL::spTerrain as the weather simulator calls it (the city's terrain, looked up on each call: the weather initialises first).
struct WeatherTerrain final : sc4::IWeatherTerrain {
    int CellCountX() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    int CellCountZ() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountZ()) : 0; }
    void GetAltitudes(int x0, int z0, int x1, int z1, float* out, int stepX, int stepZ) override {
        sc4::cSTETerrain* t = Terrain();
        if (!t) return;
        // The one caller asks for a single column, its vertices consecutive along z.
        t->GetAltitudes(sc4::TerrainRect{x0, z0, x1, z1}, out, stepZ, static_cast<std::ptrdiff_t>(stepX) * (z1 - z0 + 1));
    }
    float AltitudeAtVertex(int x, int z) override {
        sc4::cSTETerrain* t = Terrain();
        if (!t || x < 0 || z < 0 || static_cast<std::uint32_t>(x) > t->CellCountX() || static_cast<std::uint32_t>(z) > t->CellCountZ()) return 0.0f;
        return t->GetAltitude(static_cast<std::int32_t>(static_cast<std::uint32_t>(z) * (t->CellCountX() + 1) + static_cast<std::uint32_t>(x)));
    }
    bool CellHasWater(int x, int z) override {
        sc4::cSTETerrain* t = Terrain();
        return t && x >= 0 && z >= 0 && t->CellHasWater(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
    }
    void VertexNormal(int x, int z, float out[3]) override {
        sc4::cSTETerrain* t = Terrain();
        sc4::TerrainVector3 normal{0.0f, 1.0f, 0.0f};
        if (t && x >= 0 && z >= 0) t->GetVertexNormal(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), normal);
        out[0] = normal.x;
        out[1] = normal.y;
        out[2] = normal.z;
    }
    float SeaLevel() override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
};

WeatherTerrain g_weatherTerrain;

struct WeatherHost final : sc4::IWeatherHost {
    sc4::cISCPropertyHolder* SimulatorTunables(const cGZPersistResourceKey& key, std::uint32_t filter) override {
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, filter, &resource, 0, nullptr) || !resource) return nullptr;
        return static_cast<sc4::cISCResExemplar*>(resource)->AsISCPropertyHolder();   // the simulator keeps the reference
    }
    float CitySizeX() override { sc4::cSC4City* c = HostCity(); return c ? static_cast<float>(c->CellCountX()) * 16.0f : 0.0f; }
    float CitySizeZ() override { sc4::cSC4City* c = HostCity(); return c ? static_cast<float>(c->CellCountZ()) * 16.0f : 0.0f; }
    float CityCellSizeX() override { return 16.0f; }
    float CityCellSizeZ() override { return 16.0f; }
    int CityHemisphere() override { sc4::cSC4City* c = HostCity(); return c ? c->GetWorldHemisphere() : 1; }
    int SimulatorMonth() override { sc4::cSC4Simulator* s = Simulator(); return s ? static_cast<int>(s->GetSimDate().Month()) : 1; }
    void RegisterAgent(cIGZMessageTarget2* target, int interval, const char* name) override { AddCitySimulatorAgent(target, interval, name, 2); }
    void UnregisterAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->AddNotification(target, type); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, type); }
    void PostMessage(cIGZMessage2* message) override { if (SL::spMS2 && message) SL::spMS2->MessagePost(message, false); }
    // SEAM: the original asks an app object (cISC4App vslot 0x8c, then vslot 0x54) for the range; its defaults 60..100 stand.
    void MoistureValueRange(std::uint8_t&, std::uint8_t&) override {}
    sc4::IWeatherTerrain* Terrain() override { return &g_weatherTerrain; }

    // MakeWaterAreaList (Mac i386 0xaa914): the rectangles of below-sea vertices, scanned row by row (z) from a free vertex at
    // or below the sea: the run along x, then grown along z while every row keeps a run at least `shorter` long; a rectangle
    // with sides of at least min(minWidth, minDepth) and max(...) is kept and its cells marked taken. Vertices are sampled
    // with the terrain's interpolated altitude at (x * cellWidth, z * cellDepth); the scan grid is 256 x 256.
    void MakeWaterAreaList(sc4::IWeatherTerrain&, std::vector<cRZRect>& out, int minWidth, int minDepth, int cellsX, int cellsZ,
                           int cellWidth, int cellDepth, const cRZRect* area) override {
        out.clear();
        sc4::cSTETerrain* terrain = native::Terrain();
        if (!terrain) return;
        const auto W = static_cast<std::uint32_t>(cellsX), D = static_cast<std::uint32_t>(cellsZ);
        const auto shorter = static_cast<std::uint32_t>(std::min(minWidth, minDepth));
        const auto longer = static_cast<std::uint32_t>(std::max(minWidth, minDepth));
        std::uint32_t x0, z0, xEnd, zEnd;
        if (!area) {
            x0 = 0;
            z0 = 0;
            xEnd = W - shorter + 1;
            zEnd = D - shorter + 1;
        } else {
            x0 = static_cast<std::uint32_t>(area->left);
            z0 = static_cast<std::uint32_t>(area->top);
            xEnd = static_cast<std::uint32_t>(area->right);
            zEnd = static_cast<std::uint32_t>(area->bottom);
        }
        const float sea = terrain->GetSeaLevel();
        static std::vector<std::uint8_t> taken;
        taken.assign(256 * 256, 0);   // (the original clears [x0..W) x [x0..D): z starts at x0 there too)
        const auto at = [&](std::uint32_t x, std::uint32_t z) -> std::uint8_t& { return taken[(x & 0xff) * 256 + (z & 0xff)]; };
        const auto dry = [&](std::uint32_t x, std::uint32_t z) {
            return sea < terrain->GetAltitude(static_cast<float>(x * static_cast<std::uint32_t>(cellWidth)),
                                              static_cast<float>(z * static_cast<std::uint32_t>(cellDepth)));
        };
        for (std::uint32_t z = z0; z < zEnd; ++z) {
            for (std::uint32_t x = x0; x < xEnd; ++x) {
                if (at(x, z) != 0 || dry(x, z)) continue;
                std::uint32_t runEnd = x;                                   // the run along x on the first row
                while (runEnd < W && at(runEnd, z) == 0 && !dry(runEnd, z)) ++runEnd;
                if (runEnd - x < shorter) continue;
                std::uint32_t left = x, right = runEnd, row = z;
                for (; row < D; ++row) {                                    // grow along z while each row keeps a run
                    std::uint32_t start = left, end = right;
                    bool inRun = false;
                    for (std::uint32_t cx = left; cx < right; ++cx) {
                        if (dry(cx, row) || at(cx, row) != 0) {
                            if (inRun) {
                                end = cx;
                                break;
                            }
                            ++start;
                        } else {
                            inRun = true;
                        }
                    }
                    if (end - start < shorter) break;
                    left = start;
                    right = end;
                }
                const std::uint32_t width = right - left, depth = row - z;
                if (std::min(width, depth) >= shorter && std::max(width, depth) >= longer) {
                    for (std::uint32_t cx = left; cx < right; ++cx)
                        for (std::uint32_t cz = z; cz < row; ++cz) at(cx, cz) = 1;
                    out.push_back(cRZRect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(z), static_cast<std::int32_t>(right),
                                          static_cast<std::int32_t>(row)});
                }
            }
        }
    }
};

WeatherHost g_weatherHost;

}  // namespace

void InstallNativeWeather() { sc4::SetWeatherHost(&g_weatherHost); }

// cSC4WeatherSimulator::DoPostCityInit (the city-started message 0x26d31ec1, which the native city does not post): a month and the
// weeks of a full moisture / temperature pass, before the terrain picks its first surface cover from them.
void WeatherPostCityInit() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::WeatherSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    if (serializable->GetGZCLSID() == sc4::kGZCLSID_cSC4WeatherSimulator) {
        auto* weather = static_cast<sc4::cSC4WeatherSimulator*>(serializable);
        weather->DoPostCityInit();
        if (std::getenv("SC4_DEBUG")) {
            std::vector<std::vector<std::uint8_t>>* map = nullptr;
            weather->GetMoistureValueMapPtr(map);
            unsigned lo = 255, hi = 0;
            if (map) for (auto& column : *map) for (std::uint8_t v : column) { lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
            std::fprintf(stderr, "weather: post city init, moisture grid %zu x %zu, at the centre %u (range %u..%u), temperature %.1f, humidity %.3f\n",
                         map ? map->size() : 0, map && !map->empty() ? (*map)[0].size() : 0, weather->GetMoistureValue(1024.0f, 1024.0f), lo, hi,
                         weather->CurrentTemperature(), weather->CurrentHumidity());
        }
    }
    serializable->Release();
}

}  // namespace native

// The city view's corner minimap on the native application (cSC4WinMiniMap's IMiniMapHost): the city's terrain, water and zones,
// the surface networks, the camera's rotation / view rectangle and the click that recentres the camera, the exemplar colour ramps,
// the animation tick and the anti-aliased line the view rectangle is drawn with (DrawLineAntialiased, Mac i386 0x002b4148 /
// DrawLineAntialiasedNoClip 0x002b2f02, ported here: the minimap is its only caller).
//
// PARTIAL: the city situation manager's target marker (no situation manager view natively), and the camera's view rectangle:
// its corners come from cSC43DRender::PickTerrainAlways (Mac 0x0050679c, over PickTerrain 0x0050498a and the terrain view's
// pick), which the renderer does not answer yet, so no rectangle is drawn.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "sc4/Render3D.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeMiniMap.h"
#include "gz/DrawContext.h"
#include "gz/Win.h"
#include "gz/WinInterfaces.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/CameraControl.h"
#include "sc4/Exemplar.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/SC4UI.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinMiniMap.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

template <class T> T* Component(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<cSTETerrain*>(serializable);
}
cISC4OccupantManager* Occupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}

class Terrain_ final : public IMiniMapTerrain {
public:
    int CellCountX() override { cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    int CellCountZ() override { cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountZ()) : 0; }
    float SeaLevel() override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
    float Altitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    void Altitudes(const cRZRect& rect, float* out, long rowStride, long colStride) override {
        if (cSTETerrain* t = Terrain()) t->GetAltitudes(TerrainRect{rect.left, rect.top, rect.right, rect.bottom}, out, rowStride, colStride);
    }
    void WaterDepths(const cRZRect& rect, float* out, long rowStride, long colStride) override {
        if (cSTETerrain* t = Terrain()) t->GetWaterDepths(TerrainRect{rect.left, rect.top, rect.right, rect.bottom}, out, rowStride, colStride);
    }
    void VertexNormal(int x, int z, cS3DVector3& out) override {
        TerrainVector3 normal;
        if (cSTETerrain* t = Terrain()) t->GetVertexNormal(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), normal);
        out = cS3DVector3{normal.x, normal.y, normal.z};
    }
} g_terrain;

// SL::spZoneManager -> GetZoneGrid -> data[x][z]: the zone type of the cell.
class ZoneGrid final : public IMiniMapZoneGrid {
public:
    int ZoneAt(int x, int z) override {
        cSC4ZoneManager* zones = Component<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId);
        return zones ? zones->GetZoneType(x, z) : 0;
    }
} g_zones;

class CameraControl final : public IMiniMapCameraControl {
public:
    int Rotation() const override { cSC4CameraControl* c = Control(); return c ? c->Rotation() : 0; }
    bool SetViewTargetPosition(const cS3DVector3& position) override { cSC4CameraControl* c = Control(); return c && c->SetViewTargetPosition(position); }
private:
    static cSC4CameraControl* Control() { cSC43DRender* r = CityRenderer(); return r ? r->GetCameraControl() : nullptr; }
} g_camera;

class Renderer final : public IMiniMapRenderer {
public:
    IMiniMapCameraControl* CameraControl() override { return CityRenderer() ? &g_camera : nullptr; }
    s3d::cS3DCamera* Camera() override { cSC43DRender* r = CityRenderer(); return r ? r->GetCamera() : nullptr; }
    bool PickTerrainAlways(const float* origin, const float* direction, float* out, bool a) override {
        cSC43DRender* r = CityRenderer();
        return r && r->PickTerrainAlways(origin, direction, out, a);
    }
} g_renderer;

class View3D final : public IMiniMapView3D {
public:
    int Width() override { cIGZWin* w = Window(); return w ? w->GetW() : 0; }
    int Height() override { cIGZWin* w = Window(); return w ? w->GetH() : 0; }
private:
    static cIGZWin* Window() {
        void* raw = nullptr;
        if (!SL::spView3DWin || !SL::spView3DWin->QueryInterface(kGZIID_cIGZWin, &raw) || !raw) return nullptr;
        auto* window = static_cast<cIGZWin*>(raw);
        window->Release();   // the locator keeps the window alive
        return window;
    }
} g_view;

// cSC4SurfaceNetworkOccupantFilter: a network occupant of a surface kind (any flag of 0x1f4f).
class SurfaceNetworkFilter final : public sc4::cISC4OccupantFilter {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsOccupantIncluded(sc4::cISC4Occupant* occupant) override {
        auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        return network && network->HasAnyNetworkFlag(0x1f4f);
    }
    bool IsOccupantTypeIncluded(std::uint32_t) override { return false; }
    bool IsPropertyHolderIncluded(sc4::cISCPropertyHolder*) override { return false; }
};

class OccupantQuery final : public IMiniMapOccupantQuery {
public:
    void Release() override { delete this; }
    bool GetFirstOccupant(int x, int z, cIGZUnknown*& out) override {   // occupant manager vslot 0x108
        out = nullptr;
        cISC4OccupantManager* occupants = Occupants();
        cISC4Occupant* occupant = nullptr;
        if (!occupants || !occupants->GetFirstOccupantByStandardCityCell(occupant, x, z, &filter_) || !occupant) return false;
        out = occupant;
        return true;
    }
private:
    SurfaceNetworkFilter filter_;
};

class Ramp final : public IMiniMapRamp {
public:
    std::vector<std::uint32_t> stops;
    const std::uint32_t* Data() override { return stops.data(); }
    int Count() override { return static_cast<int>(stops.size()); }
};

// The pixel blend of DrawLineAntialiasedNoClip's 16-bit cases: _blendTable5Bit[weight + (source * 32 + target) * 32], a 5-bit
// channel mix with a 5-bit weight (weight 31 = the source). The table is data in the original; this is the mix it holds.
std::uint32_t Blend5(std::uint32_t weight, std::uint32_t source, std::uint32_t target) {
    return (source * weight + target * (31 - weight) + 15) / 31;
}

// The 32-bit pixel mix of the 0x8 / 0x9 formats (two channels at a time, the original's arithmetic).
std::uint32_t Mix32(std::uint32_t pixel, std::uint32_t colour, std::uint32_t weight) {
    const std::uint32_t rb = pixel & 0xff00ff, ag = pixel & 0xff00ff00;
    return ((rb + ((((colour & 0xff00ff) - rb) * weight + 0x800080) >> 8)) & 0xff00ff) +
           ((ag + 0x800080 + weight * (((colour & 0xff00ff00) >> 8) - (ag >> 8))) & 0xff00ff00);
}

// One anti-aliased pixel pair of the 16-bit formats: 4 / 6 = X1R5G5B5, 5 = R5G6B5 (green's top five bits), 7 = A4R4G4B4.
void Plot16(std::uint16_t* a, std::uint16_t* b, std::uint32_t format, std::uint32_t colour, std::uint32_t weight) {
    auto mix = [&](std::uint16_t pixel, std::uint32_t w) -> std::uint16_t {
        switch (format) {
            case 5:
                return static_cast<std::uint16_t>(Blend5(w, colour >> 11, pixel >> 11) << 11 |
                                                  Blend5(w, (colour >> 6) & 0x1f, (pixel >> 6) & 0x1f) << 6 |
                                                  Blend5(w, colour & 0x1f, pixel & 0x1f));
            case 7:
                return static_cast<std::uint16_t>(Blend5(w, (colour >> 8) & 0xf, (pixel >> 8) & 0xf) << 8 |
                                                  Blend5(w, (colour >> 4) & 0xf, (pixel >> 4) & 0xf) << 4 |
                                                  Blend5(w, colour & 0xf, pixel & 0xf) | 0xf000);
            default:
                return static_cast<std::uint16_t>(Blend5(w, colour >> 10, pixel >> 10) << 10 |
                                                  Blend5(w, (colour >> 5) & 0x1f, (pixel >> 5) & 0x1f) << 5 |
                                                  Blend5(w, colour & 0x1f, pixel & 0x1f));
        }
    };
    *a = mix(*a, 31 - weight);
    *b = mix(*b, weight);
}

// DrawLineAntialiasedNoClip (0x002b2f02) on a locked surface: horizontal / vertical / 45-degree lines are solid; any other
// slope is a Wu line with a 16-bit error accumulator, each step blending the colour into two neighbouring pixels by the
// fraction, and the end point solid.
void DrawLineNoClip(const gz::cGZBufferSurfaceDesc& surface, int x0, int y0, int x1, int y1, std::uint32_t colour) {
    const bool wide = surface.bytesPerPixel == 4;
    const std::int64_t stride = surface.pitch / (wide ? 4 : 2);
    auto* p16 = reinterpret_cast<std::uint16_t*>(surface.pixels);
    auto* p32 = reinterpret_cast<std::uint32_t*>(surface.pixels);
    auto put = [&](int x, int y) {
        if (wide) p32[x + y * stride] = colour;
        else p16[x + y * stride] = static_cast<std::uint16_t>(colour);
    };
    if (y0 == y1) {   // the end pixel is not drawn
        if (x1 < x0) std::swap(x0, x1);
        for (int x = x0; x < x1; ++x) put(x, y0);
        return;
    }
    if (x0 == x1) {   // the end pixel is drawn
        if (y1 < y0) std::swap(y0, y1);
        for (int y = y0; y <= y1; ++y) put(x0, y);
        return;
    }
    if (y1 < y0) {
        std::swap(y0, y1);
        std::swap(x0, x1);
    }
    put(x0, y0);
    const int dy = y1 - y0;
    int dx = x1 - x0;
    int step = 1;
    if (dx < 0) {
        dx = -dx;
        step = -1;
    }
    if (dx == dy) {
        for (int x = x0, y = y0; y != y0 + dx;) put(x += step, ++y);
        return;
    }
    auto plot = [&](int x, int y, std::int64_t neighbour, std::uint16_t error) {
        if (wide) {
            std::uint32_t* pixel = &p32[x + y * stride];
            const std::uint32_t weight = error >> 8;
            *pixel = Mix32(*pixel, colour, 0xff - weight);
            pixel[neighbour] = Mix32(pixel[neighbour], colour, weight);
        } else {
            std::uint16_t* pixel = &p16[x + y * stride];
            Plot16(pixel, pixel + neighbour, surface.format, colour, error >> 11);
        }
    };
    std::uint16_t error = 0;
    int x = x0, y = y0;
    if (dx < dy) {   // y major: the error carries x
        const std::uint16_t adjust = static_cast<std::uint16_t>((static_cast<std::uint32_t>(dx) << 16) / static_cast<std::uint32_t>(dy));
        for (int i = 0; i != dy - 1; ++i) {
            const std::uint16_t next = static_cast<std::uint16_t>(error + adjust);
            if (next <= error) x += step;
            ++y;
            plot(x, y, step, next);
            error = next;
        }
    } else {         // x major: the error carries y
        const std::uint16_t adjust = static_cast<std::uint16_t>((static_cast<std::uint32_t>(dy) << 16) / static_cast<std::uint32_t>(dx));
        for (int i = 0; i != dx - 1; ++i) {
            const std::uint16_t next = static_cast<std::uint16_t>(error + adjust);
            if (next <= error) ++y;
            x += step;
            plot(x, y, stride, next);
            error = next;
        }
    }
    put(x1, y1);
}

// ClipLineToRect (Cohen-Sutherland against the inclusive rectangle); false when nothing is left.
bool ClipLine(int& x0, int& y0, int& x1, int& y1, int left, int top, int right, int bottom) {
    auto code = [&](int x, int y) { return (x < left ? 1 : x > right ? 2 : 0) | (y < top ? 4 : y > bottom ? 8 : 0); };
    int c0 = code(x0, y0), c1 = code(x1, y1);
    for (;;) {
        if (!(c0 | c1)) return true;
        if (c0 & c1) return false;
        const int c = c0 ? c0 : c1;
        int x = 0, y = 0;
        if (c & 8) { x = x0 + (x1 - x0) * (bottom - y0) / (y1 - y0); y = bottom; }
        else if (c & 4) { x = x0 + (x1 - x0) * (top - y0) / (y1 - y0); y = top; }
        else if (c & 2) { y = y0 + (y1 - y0) * (right - x0) / (x1 - x0); x = right; }
        else { y = y0 + (y1 - y0) * (left - x0) / (x1 - x0); x = left; }
        if (c == c0) { x0 = x; y0 = y; c0 = code(x0, y0); }
        else { x1 = x; y1 = y; c1 = code(x1, y1); }
    }
}

class Host final : public IMiniMapHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cIGZBuffer* CreateBuffer() override {
        auto* buffer = new cGZBuffer();
        buffer->AddRef();
        return buffer;
    }
    int CityCellCountX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellCountZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    float CitySizeX() override { cSC4City* c = HostCity(); return c ? c->SizeX() : 0.0f; }
    float CitySizeZ() override { cSC4City* c = HostCity(); return c ? c->SizeZ() : 0.0f; }
    IMiniMapSituation* CitySituationManager() override { return nullptr; }
    IMiniMapTerrain* Terrain() override { return native::Terrain() ? &g_terrain : nullptr; }
    IMiniMapZoneGrid* ZoneGrid() override { return HostCity() ? &g_zones : nullptr; }
    IMiniMapOccupantQuery* CreateSurfaceNetworkOccupantQuery() override { return Occupants() ? new OccupantQuery() : nullptr; }
    void InterpolateRamp(std::uint32_t* out, int count, const std::uint32_t* stops, int stopCount) override {
        nSC4UI::InterpolateRampToPalette(out, count, stops, stopCount);
    }
    IMiniMapRenderer* Renderer() override { return CityRenderer() ? &g_renderer : nullptr; }
    IMiniMapView3D* View3DWindow() override { return SL::spView3DWin ? &g_view : nullptr; }
    void AddTickNotification(cIGZMessageTarget2* target) override { AnimationTickServerView()->AddNotification(target, 0); }
    void RemoveTickNotification(cIGZMessageTarget2* target) override { AnimationTickServerView()->RemoveNotification(target, 0); }
    IMiniMapRamp* FindRamp(std::uint32_t propertyId) override {
        Ramp& ramp = propertyId == kMiniMapWaterRampProperty ? water_ : terrain_;
        ramp.stops.clear();
        if (!SL::spRM) return nullptr;
        const cGZPersistResourceKey key{0x6534284a, kMiniMapTunableGroup, kMiniMapTunableInstance};
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        const bool found = cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), propertyId, ramp.stops);
        exemplar->Release();
        return found && !ramp.stops.empty() ? &ramp : nullptr;
    }
    bool NetworkOccupantCell(cIGZUnknown* occupant, int& cellX, int& cellZ) override {
        auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        if (!network) return false;
        std::uint32_t x = 0, z = 0;
        network->GetOccupiedCell(x, z);
        cellX = static_cast<int>(x);
        cellZ = static_cast<int>(z);
        return true;
    }
    // DrawLineAntialiased (0x002b4148) with antialiasing on and no clip rectangle: clipped to the buffer, drawn on the
    // locked surface (locked here when the caller has not).
    bool DrawLineAntialiased(IWinBuffer* target, int x0, int y0, int x1, int y1, std::uint32_t colour) override {
        auto* context = target ? static_cast<gz::cIGZDrawContext*>(target->GetDrawContext()) : nullptr;
        if (!context) return false;
        gz::cGZBufferSurfaceDesc surface;
        if (!context->Lock(surface, false)) return false;
        if (surface.pixels && (surface.bytesPerPixel == 2 || surface.bytesPerPixel == 4) && surface.width && surface.height &&
            ClipLine(x0, y0, x1, y1, 0, 0, static_cast<int>(surface.width) - 1, static_cast<int>(surface.height) - 1))
            DrawLineNoClip(surface, x0, y0, x1, y1, colour);
        context->Unlock();
        return true;
    }

private:
    Ramp terrain_, water_;
};

}  // namespace

void InstallMiniMapHost() {
    static Host host;
    sc4::SetMiniMapHost(&host);
}

}  // namespace native

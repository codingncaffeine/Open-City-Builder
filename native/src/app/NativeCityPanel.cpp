// The city toolbar's gauges once a city is up: the three RCI demand bars and the six opinion-poll trend bars.
//
// cSC4View3DWin::DoPostCityInit (i386 0x0052bac8) gives every RCI bar its colour, its demand channels and the view as its
// notification target (the static table C.900 at 0x00851260), and every trend bar its two images and, from the exemplar
// {0x6534284a, 0x690f693f, window id}, the history series it shows (property 0x6a6564f4) and the response curve that maps
// the series into the gauge (0x6a6564f6). The port's view leaves that part of DoPostCityInit to the host; it runs here
// after the city UI is built. The two widgets' hosts (the demand simulator, the history warehouse, the message server and
// the trend bar's blits) are installed once, before any city UI exists, since the widgets bind them in their Init.
#include "app/NativeCityPanel.h"

#include <cstdio>
#include <cstdlib>

#include "app/NativeCity.h"
#include "app/NativeUi.h"
#include "gz/DrawContext.h"
#include "gz/Win.h"
#include "sc4/DemandSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/View3DWin.h"
#include "sc4/WinRCI.h"
#include "sc4/WinTrendBar.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ResponseCurve.h"

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

// ---- the RCI bar's services (SL::spDemandSim, SL::spMS2) ----
class RCIHost final : public sc4::IWinRCIHost {
public:
    sc4::cISC4DemandSimulator* DemandSimulator() override {
        sc4::cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::DemandSimulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(sc4::kGZIID_cISC4DemandSimulator, &raw) || !raw) return nullptr;
        auto* simulator = static_cast<sc4::cISC4DemandSimulator*>(raw);
        simulator->Release();   // the city holds it; the bar does not keep a reference
        return simulator;
    }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
};
RCIHost g_rci;

// ---- the trend bar's services: the message server, the history warehouse and the window-buffer blits ----
class TrendBarHost final : public sc4::ITrendBarHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    bool HistoryPoint(std::uint32_t series, const cIGZDate& date, float& value) override {
        auto* history = Component<sc4::cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse, sc4::kGZCLSID_cSC4HistoryWarehouse);
        return history && history->GetPoint(series, date, value);
    }
    std::uint32_t SimulatorDayNumber() override {
        auto* simulator = Component<sc4::cSC4Simulator>(CityComponent::Simulator, sc4::kGZCLSID_cSC4Simulator);
        return simulator ? simulator->GetSimDateNumber() : 0;
    }
    bool Blt(IWinBuffer* target, cIGZBuffer* source, const cRZRect& sourceRect, const cRZRect& destRect) override {
        auto* context = target ? static_cast<gz::cIGZDrawContext*>(target->GetDrawContext()) : nullptr;
        if (!context || !source) return false;
        context->Blt(source, sourceRect, destRect);
        return true;
    }
    bool BltTiled(IWinBuffer* target, cIGZBuffer* source, const cRZRect& sourceRect, const cRZRect& destRect) override {
        auto* context = target ? static_cast<gz::cIGZDrawContext*>(target->GetDrawContext()) : nullptr;
        if (!context || !source) return false;
        context->BltTiled(source, sourceRect, destRect, 0, 0);
        return true;
    }
};
TrendBarHost g_trend;

// C.900 (i386 0x00851260): window id, channel count, up to six demand ids, colour bytes r g b.
struct RCIBar {
    std::uint32_t window;
    int count;
    std::uint32_t ids[6];
    std::uint8_t r, g, b;
};
constexpr RCIBar kRCIBars[] = {
    {0x09d27eb0, 3, {0x1010, 0x1020, 0x1030}, 0x00, 0xc8, 0x00},                   // residential: green
    {0x29d27ec0, 5, {0x3110, 0x3120, 0x3320, 0x3130, 0x3330}, 0x00, 0x00, 0xc8},   // commercial: blue
    {0x49d27ed0, 3, {0x4200, 0x4300, 0x4400}, 0xc8, 0xc8, 0x00},                   // industrial: yellow
};
// trendBarIDs (i386 0x00abcd4c): environment, health, safety, traffic, education, land value.
constexpr std::uint32_t kTrendBars[] = {0x6a5e6edc, 0x6a5e6edd, 0x6a5e6ede, 0x6a5e6edf, 0x6a5e6ee0, 0x6a5e6ee1};
constexpr std::uint32_t kTrendFillImage = 0x14015580, kTrendMarkerImage = 0x14015584, kUiImageGroup = 0x46a006b0;
constexpr cGZPersistResourceKey kTrendExemplar{0x6534284a, 0x690f693f, 0};
constexpr std::uint32_t kTrendSeriesProperty = 0x6a6564f4, kTrendCurveProperty = 0x6a6564f6;
constexpr std::uint32_t kTrendDefaultSeries = 0x0a5cbf37;

}  // namespace

void InstallCityPanelHosts() {
    sc4::SetWinRCIHost(&g_rci);
    sc4::SetTrendBarHost(&g_trend);
}

void ConfigureCityPanels(sc4::cSC4View3DWin& view) {
    cIGZWin* root = view.AsIGZWin();
    if (!root) return;
    const bool debug = std::getenv("SC4_DEBUG") != nullptr;
    int rci = 0, trends = 0;
    for (const RCIBar& bar : kRCIBars) {
        void* raw = nullptr;
        if (!root->GetChildAsRecursive(bar.window, sc4::kGZIID_cISC4WinRCI, &raw) || !raw) continue;
        auto* widget = static_cast<sc4::cISC4WinRCI*>(raw);
        widget->SetColor(0xff000000u | (std::uint32_t{bar.r} << 16) | (std::uint32_t{bar.g} << 8) | bar.b);
        widget->SetDemandType(bar.ids, bar.count);
        if (cIGZWin* win = widget->AsIGZWin()) win->SetNotificationTarget(root);
        widget->Release();
        ++rci;
    }
    cIGZBuffer* fill = LoadBitmapResource(kUiImageGroup, kTrendFillImage);
    cIGZBuffer* marker = LoadBitmapResource(kUiImageGroup, kTrendMarkerImage);
    for (std::uint32_t id : kTrendBars) {
        void* raw = nullptr;
        if (!root->GetChildAsRecursive(id, 0xca5c2f84, &raw) || !raw) continue;
        auto* widget = static_cast<sc4::cISC4WinTrendBar*>(raw);
        widget->SetArtwork(fill, marker);
        void* resource = nullptr;
        cGZPersistResourceKey key = kTrendExemplar;
        key.instance = id;
        if (SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &resource, 0, nullptr) && resource) {
            auto* exemplar = static_cast<sc4::cISCResExemplar*>(resource);
            sc4::cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
            std::uint32_t series = kTrendDefaultSeries;
            std::vector<float> points;
            cSC4ResponseCurve curve;
            if (sc4::cSCPropertyHelp::GetPropertyValue(holder, kTrendSeriesProperty, series) &&
                sc4::cSCPropertyHelp::GetPropertyValue(holder, kTrendCurveProperty, points) &&
                curve.SetResponseCurveData(points.data(), points.size())) {
                widget->SetDataSource(series, curve);
                ++trends;
                if (debug) std::fprintf(stderr, "city panel: trend bar %08x shows series %08x (%zu curve points)\n", id, series, points.size() / 2);
            }
            exemplar->Release();
        }
        widget->Release();
    }
    if (fill) fill->Release();
    if (marker) marker->Release();
    if (debug) std::fprintf(stderr, "city panel: %d RCI bars, %d opinion polls configured\n", rci, trends);
}

}  // namespace native

// The graphs panel's services on the native application (sc4::IGraphsViewHost). The generated stub answered every history
// point with "none", so the City Graphs window drew empty axes.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/NativeCity.h"
#include "app/NativeUi.h"
#include "gz/Buffer.h"
#include "gz/GZFont.h"
#include "gz/Graph.h"
#include "gz/Win.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/Exemplar.h"
#include "sc4/GraphsViewWinProc.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC4City.h"

namespace native {
cGZFont* HtmlFont(const std::string& faces, std::uint32_t size, bool bold, bool italic);   // linux/game_main_linux.cpp

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_GRAPHDEBUG") != nullptr;
    return on;
}

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

class GraphsViewHost final : public IGraphsViewHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    // nsSC4TutorialSystem::FilterUICommandMessage: no tutorial runs natively yet, so every command goes through
    bool FilterUICommandMessage(cGZMessage&) override { return true; }
    bool LoadExemplarString(cIGZString& out, cISCPropertyHolder* holder, std::uint32_t property) override {
        return native::LoadExemplarString(out, holder, property);
    }
    // SL::spRM vslot 0x18 with key {0x6534284a, 0xca4ad545, option} and iid 0xa52160f5; one reference
    cISCResExemplar* LoadGraphExemplar(std::uint32_t option) override {
        void* raw = nullptr;
        const cGZPersistResourceKey key{0x6534284a, 0xca4ad545, option};
        auto* exemplar = SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) && raw ? static_cast<cISCResExemplar*>(raw) : nullptr;
        if (Trace()) std::fprintf(stderr, "graphs: exemplar %08x -> %p\n", option, static_cast<void*>(exemplar));
        return exemplar;
    }
    // the city's history warehouse (spCity vslot 0x100) vslot 0x40 GetPoint
    bool HistoryPoint(std::uint32_t series, const cIGZDate& date, float& value) override {
        auto* history = Component<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse, kGZCLSID_cSC4HistoryWarehouse);
        const bool ok = history && history->GetPoint(series, date, value);
        static int traced = 0;
        if (Trace() && traced < 60) {
            ++traced;
            std::fprintf(stderr, "graphs: point %08x day %u -> %s %.2f (warehouse %p)\n", series, date.DayNumber(), ok ? "ok" : "none", value,
                         static_cast<void*>(history));
        }
        return ok;
    }
    std::uint32_t SimulatorDayNumber() override {   // simulator vslot 0x40
        auto* simulator = Component<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        return simulator ? simulator->GetSimDateNumber() : 0;
    }
    void SimulatorStartDate(cIGZDate& out) override {   // simulator vslot 0x28
        cSC4Simulator::GetSimStartDateStatic(out);
    }
};
// The graph controls' services (cGZGraph and its line / bar / pie kinds draw on the window's buffer with their own fonts).
class GraphHost final : public gz::IGraphHost {
public:
    cIGZFont* CreateFont(const char* face, std::uint32_t size) override {   // one reference; the font cache keeps its own
        cGZFont* font = HtmlFont(face ? face : "", size, false, false);
        if (font) font->AddRef();
        return font;
    }
    bool FontsAvailable() override { return true; }
    cIGZBuffer* CreateBuffer() override {
        auto* buffer = new cGZBuffer();
        buffer->AddRef();
        return buffer;
    }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    cIGZBuffer* WindowBuffer(IWinBuffer* buffer) override { return buffer ? buffer->AsGZBuffer() : nullptr; }
};
}  // namespace

void InstallGraphsViewHost() {
    static GraphsViewHost host;
    sc4::SetGraphsViewHost(&host);
    static GraphHost graphs;
    gz::SetGraphHost(&graphs);
}

}  // namespace native

// The snapshot taker's and album's host (sc4/WinSnapshot.h): the open city's facts, the frame's pixels, the user's Albums
// folder, the UI art and strings; and the picture view's host (sc4/WinBMPView.h): its image factory.
#include "app/NativeAudio.h"
#include "app/NativeSnapshot.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/NativeCheats.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "app/NativeUi.h"
#include "app/NativeWinMgr.h"
#include "gz/Buffer.h"
#include "gz/CheatCodeManager.h"
#include "gz/GimexFactory.h"
#include "gz/GimexReaderWriter.h"
#include "gz/RZServices.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinFont.h"
#include "gz/cGZDate.h"
#include "rz/LanguageManager.h"
#include "rz/cRZVersion.h"
#include "sc4/App.h"
#include "sc4/Region.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinLocationSaver.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/SC4Utilities.h"
#include "sc4/Simulator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewUtilities.h"
#include "sc4/WinSnapshot.h"
#include "sc4/Render3D.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

FrameReader g_readFrame = nullptr;

bool Trace() {
    static const bool on = std::getenv("SC4_SNAPSHOTDEBUG") != nullptr;
    return on;
}

template <class T> T* Component(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    if (!object) return nullptr;
    if (T* direct = dynamic_cast<T*>(object)) return direct;
    void* raw = nullptr;
    if (!object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = dynamic_cast<T*>(serializable);
    serializable->Release();
    return out;
}

cIGZLanguageUtility* Language() {
    cIGZLanguageManager* manager = RZ::LM();
    return manager ? manager->GetLanguageUtility(0) : nullptr;
}

// The language manager's date (slot 0x28) for a date: month, day, and the year counted from the simulation's start
// (reduced below 10000 as the original does).
void DateText(const cIGZDate& date, cIGZString& out, cIGZLanguageUtility::DateFormat format) {
    cGZDate start;
    cSC4Simulator::GetSimStartDateStatic(start);
    int year = static_cast<int>(date.Year()) - static_cast<int>(start.Year());
    while (year > 10000) year -= 10000;
    if (cIGZLanguageUtility* language = Language())
        language->MakeDateString(date.Month(), date.DayOfMonth(), static_cast<std::uint32_t>(year), out, format);
}

cGZGimexFactory* NewImageFactory() {
    auto* factory = new cGZGimexFactory();
    factory->AddRef();
    factory->makeReader = [] { return static_cast<cIGZGimexReader*>(new cGZGimexReader()); };
    factory->makeWriter = [] { return static_cast<cIGZGimexWriter*>(new cGZGimexWriter()); };
    factory->makeBuffer = [] {   // CreateBuffer hands out one counted reference, as the COM class does
        auto* buffer = new cGZBuffer();
        buffer->AddRef();
        return static_cast<cIGZBuffer*>(buffer);
    };
    return factory;
}

struct Host final : ISnapshotHost {
    cIGZWin* MainWindow() override { WinMgr* manager = ActiveWinMgr(); return manager ? manager->GetMainWindow() : nullptr; }

    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) override {
        *out = nullptr;
        cIGZWin* window = native::CreateWindowFromScript(key, parent, id);
        if (Trace()) std::fprintf(stderr, "snapshot: window %08x of script %08x -> %s\n", id, key.instance, window ? "made" : "FAILED");
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out) && *out;
        window->Release();
        return ok;
    }

    // The frame's pixels (the GL driver's read-back, top-down BGRA) inside the view's rectangle, as an A8R8G8B8 buffer.
    cIGZBuffer* CaptureView(cIGZWin* view, int x, int y, int w, int h) override {
        if (!g_readFrame || !view || w <= 0 || h <= 0) return nullptr;
        std::vector<std::uint8_t> frame;
        std::uint32_t frameW = 0, frameH = 0;
        if (!g_readFrame(frame, frameW, frameH) || frameW == 0 || frameH == 0) return nullptr;
        cIGZWin* main = MainWindow();
        const double scaleX = main && main->GetW() > 0 ? double(frameW) / main->GetW() : 1.0;
        const double scaleY = main && main->GetH() > 0 ? double(frameH) / main->GetH() : 1.0;
        int left = 0, top = 0;
        view->WindowToScreenCoordinates(left, top);
        auto* picture = new cGZBuffer();
        picture->AddRef();
        picture->Init(w, h, cGZBufferColorType{kBufferColorARGB8888, 32});
        auto* bits = static_cast<std::uint8_t*>(picture->GetColorSurfaceBits());
        const std::uint32_t stride = picture->GetColorSurfaceStride();
        for (int row = 0; row < h; ++row) {
            const int fy = static_cast<int>((top + y + row) * scaleY);
            for (int col = 0; col < w; ++col) {
                const int fx = static_cast<int>((left + x + col) * scaleX);
                std::uint8_t* out = bits + row * stride + col * 4;
                if (fx < 0 || fy < 0 || fx >= int(frameW) || fy >= int(frameH)) {
                    std::memset(out, 0, 4);
                    continue;
                }
                std::memcpy(out, &frame[(std::size_t(fy) * frameW + fx) * 4], 4);
            }
        }
        if (Trace()) std::fprintf(stderr, "snapshot: captured %dx%d at %d,%d of a %ux%u frame\n", w, h, left + x, top + y, frameW, frameH);
        return picture;
    }

    void CityInfo(SnapshotCityInfo& out) override {
        out = SnapshotCityInfo{};
        cSC4City* city = HostCity();
        if (!city) {
            // without a city: the region's name (cISC4App slot 0x88 -> region slot 0x20 -> name)
            cISC4Region* region = SL::spSC4 ? SL::spSC4->GetRegion() : nullptr;
            if (cIGZString* name = region ? region->GetName() : nullptr) out.city = cRZString(*name);
            return;
        }
        out.hasCity = true;
        city->GetCityName(out.city);
        city->GetMayorName(out.mayor);
        out.number = city->GetCitySerialNumber();
        if (cSC4Simulator* simulator = CitySimulatorObject()) {
            const cGZDate& date = simulator->GetSimDate();
            out.hasSimulator = true;
            out.simDay = static_cast<int>(date.DayNumber());
            DateText(date, out.dateText, cIGZLanguageUtility::kDateNumeric);
            DateText(date, out.fileDate, cIGZLanguageUtility::kDateAbbreviated);
        }
        if (auto* residential = Component<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator)) {
            out.hasPopulation = true;
            out.population = residential->GetPopulation();
        }
        if (auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator)) {
            out.hasFunds = true;
            out.funds = budget->GetTotalFunds();
        }
        // cGZCheatCodeManager slot 0x34 GetListOfAllCheatStrings(null, count 0): the number of registered cheats
        if (cGZCheatCodeManager* cheats = GameCheatCodeManager()) {
            std::uint32_t none = 0;
            out.hasCheats = true;
            out.cheats = static_cast<int>(cheats->GetListOfAllCheatStrings(nullptr, none));
        }
        cSC43DRender* renderer = CityRenderer();
        if (SC4ViewUtilities* view = renderer ? renderer->GetViewUtilities() : nullptr) {
            int zoom = 0, rotation = 0;
            view->GetZoomAndRotation(zoom, rotation);
            out.hasView = true;
            out.zoom = static_cast<std::uint32_t>(zoom);
            out.rotation = static_cast<std::uint32_t>(rotation);
            if (cSC4View3DWin* window = CityView3DWin()) {
                float at[3] = {0, 0, 0};
                window->PickTerrain(window->GetW() / 2, window->GetH() / 2, at, false);
                out.centerX = static_cast<std::uint32_t>(static_cast<int>(at[0] * 0.0625f));
                out.centerZ = static_cast<std::uint32_t>(static_cast<int>(at[2] * 0.0625f));
            }
        }
    }

    void UserName(cRZString& out) override {
        cRZVersion version;
        version.GetUserNameA(out, nullptr);
    }
    void AlbumsDirectory(cRZString& out) override {
        if (SL::spSC4) SL::spSC4->GetAlbumDirectory(out);
        std::string path(out.Data(), out.Strlen());
        if (!path.empty() && path.back() != '/') out.AppendChars("/", 1);
    }
    void LoadString(std::uint32_t group, std::uint32_t instance, cRZString& out) override { LoadLocalizedString(group, instance, out); }
    void MoneyString(std::int64_t amount, cRZString& out) override { SC4MakeMoneyString(amount, out); }
    void SimDateText(int simDay, cRZString& out) override {
        const cGZDate date(static_cast<std::uint32_t>(simDay));
        DateText(date, out, cIGZLanguageUtility::kDateNumeric);
    }
    cIGZBuffer* LoadImage(std::uint32_t group, std::uint32_t instance) override {
        return LoadBitmapResource(group, instance);   // one counted reference, the album's
    }
    std::uint32_t Color(std::uint32_t id) override {
        gz::cIGZWinCtrlMgr* controls = gz::WinCtrlMgr();
        return controls ? controls->DefaultColor(static_cast<int>(id)) : 0;
    }
    gz::IWinFont* Font() override {
        gz::IWinFontSource* fonts = gz::WinFontSource();
        if (!fonts) return nullptr;
        if (gz::IWinFont* font = fonts->Font(0xcad46524)) return font;
        return fonts->Font(gz::kWinDefaultFontStyle);   // stands in for "Arial MT" 14
    }
    void PlayEvent(std::uint32_t id) override { PostSoundSystemEvent(1, id); }
    void PauseGame(bool on) override {
        cSC4Simulator* simulator = CitySimulatorObject();
        if (!simulator) return;
        if (on) simulator->HiddenPause();
        else simulator->HiddenResume();
    }
    void RestoreWindowLocation(cIGZWin* window) override {   // SL::spWinLocationSaver: restore, kept on screen
        if (auto* saver = static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver)) saver->RestoreWindowLocation(window, true);
    }
    void SaveWindowLocation(cIGZWin* window) override {
        if (auto* saver = static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver)) saver->SaveWindowLocation(window);
    }
    void Notify(const cRZString& title, const cRZString& text) override {
        std::fprintf(stderr, "snapshot: %s: %s\n", title.ToChar(), text.ToChar());
    }
    cGZGimexFactory* ImageFactory() override { return NewImageFactory(); }
};

// The picture view's host: its image factory (with the port's codecs); it paints through its draw context.
struct BMPViewHost final : IWinBMPViewHost {
    cIGZBuffer* DrawBuffer(cGZWin&) override { return nullptr; }
    cGZGimexFactory* CreateGimexFactory() override { return NewImageFactory(); }
    // The fit-to-view downscale. The original's two-pass tent filter (0x1003c938c..0x1003c9c60) is not recovered; this
    // averages each destination pixel's source area (a box filter) into an A8R8G8B8 buffer that keeps the picture's
    // aspect inside width x height (inferred: the original's window-sized buffer is filled the same way).
    cIGZBuffer* DownscaleImageToFit(cIGZBuffer* image, int width, int height) override {
        if (!image || width <= 0 || height <= 0) return nullptr;
        const int sw = static_cast<int>(image->Width()), sh = static_cast<int>(image->Height());
        if (sw <= 0 || sh <= 0 || image->GetColorType().type != kBufferColorARGB8888) return nullptr;
        const double scale = std::min(double(width) / sw, double(height) / sh);
        const int dw = std::max(1, static_cast<int>(sw * scale)), dh = std::max(1, static_cast<int>(sh * scale));
        if (!image->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) return nullptr;
        const auto* source = static_cast<const std::uint8_t*>(image->GetColorSurfaceBits());
        const std::uint32_t sourceStride = image->GetColorSurfaceStride();
        auto* out = new cGZBuffer();
        out->AddRef();
        out->Init(dw, dh, cGZBufferColorType{kBufferColorARGB8888, 32});
        auto* bits = static_cast<std::uint8_t*>(out->GetColorSurfaceBits());
        const std::uint32_t stride = out->GetColorSurfaceStride();
        for (int y = 0; y < dh; ++y) {
            const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
            for (int x = 0; x < dw; ++x) {
                const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
                std::uint64_t sum[4] = {0, 0, 0, 0};
                for (int sy = y0; sy < y1; ++sy) {
                    const std::uint8_t* row = source + std::size_t(sy) * sourceStride;
                    for (int sx = x0; sx < x1; ++sx)
                        for (int c = 0; c < 4; ++c) sum[c] += row[sx * 4 + c];
                }
                const std::uint64_t n = std::uint64_t(y1 - y0) * std::uint64_t(x1 - x0);
                for (int c = 0; c < 4; ++c) bits[std::size_t(y) * stride + x * 4 + c] = static_cast<std::uint8_t>(sum[c] / n);
            }
        }
        image->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
        return out;
    }
};

}  // namespace

void SetSnapshotFrameReader(FrameReader reader) { g_readFrame = reader; }

void InstallSnapshotHost() {
    static Host host;
    static BMPViewHost view;
    sc4::SetSnapshotHost(&host);
    sc4::SetWinBMPViewHost(&view);
}

bool CreateSnapshotTaker(cIGZWin* parent) {
    if (!parent) return false;
    return sc4::cSC4WinSnapshotTaker::CreateWinSnapshotTaker(parent, 0);
}

void ShowSnapshotAlbum(cIGZWin* parent) {
    cIGZWin* main = parent ? parent->GetMainWindow() : nullptr;
    sc4::cSC4WinSnapshotAlbum* album = sc4::cSC4WinSnapshotTaker::ShowSnapshotAlbum(true, true, main, nullptr);
    if (!album) return;
    album->DoModalWin();
    album->Release();
    if (cIGZWinMgr* manager = parent->GetWindowManager()) manager->GZSetFocus(parent);
}

}  // namespace native

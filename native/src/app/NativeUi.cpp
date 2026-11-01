// Platform glue of the native Metal application (no recovered counterpart on macOS); derived from the early host.
#include "platform/GameDataPath.h"
#include "app/NativeUi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/NativeBoot.h"
#include "app/NativeWinMgr.h"
#include "gz/Buffer.h"
#include "gz/GZCOMDirector.h"
#include "gz/GZFramework.h"
#include "gz/LegacyControl.h"
#include "gz/ScriptTerp.h"
#include "gz/ScriptVocab.h"
#include "gz/ScriptableControlSet.h"
#include "gz/UIScriptService.h"
#include "gz/SmallObjects.h"
#include "gz/WinBMP.h"
#include "gz/WinBtn.h"
#include "gz/WinFlatRect.h"
#include "gz/WinGen.h"
#include "gz/WinStyle.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinFont.h"
#include "gz/WinGrid.h"
#include "gz/WinLineInput.h"
#include "gz/WinListBox.h"
#include "gz/WinScrollbar.h"
#include "gz/WinSlider.h"
#include "gz/WinText.h"
#include "rz/COMDirectors.h"
#include "sc4/SLString.h"
#include "sc4/WinMapView.h"
#include "sc4/WinSmallViews.h"
#include "rz/cRZString.h"

#include "dbpf/DBPFReader.h"

namespace native {
namespace {

WinMgr* g_mgr = nullptr;
cIGZUIScriptService* g_service = nullptr;

// ---- the image loader: PNG resources of the game data (read with the recovered DBPF reader; the engine's own path
// goes through cGZPersistResourceManager + the Gimex factory, which is not wired in the host yet) -------------------
std::string GameDataDir() { return platform::GameDataDir(); }

PngDecoder g_decodePng = nullptr;   // set by host_main.mm (ImageIO lives there)

}  // namespace

// The standard bitmap resources (type 0x856ddbac, PNG) of the game data, searched in the engine's package order; one counted reference.
// The packages in lookup order: the language's SimCityLocale.dat first (its strings and art override the base ones), then
// the expansion and the base packages, the later-loaded package first as the game's resource manager resolves them. Names match without regard to case (the PC data spells some ".DAT"); the language folder is
// $SC4_LANGUAGE, else English.
std::string FindDataFile(const std::string& relative) {
    std::error_code error;
    if (std::filesystem::exists(GameDataDir() + relative, error)) return GameDataDir() + relative;
    std::filesystem::path dir = GameDataDir();
    for (const auto& part : std::filesystem::path(relative)) {
        std::string want = part.string(), found;
        for (char& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            std::string name = entry.path().filename().string();
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name == want) { found = entry.path().filename().string(); break; }
        }
        if (found.empty()) return {};
        dir /= found;
    }
    return dir.string();
}

bool LoadResourceBytes(std::uint32_t type, std::uint32_t group, std::uint32_t instance, std::vector<std::uint8_t>& bytes) {
    static std::vector<std::unique_ptr<dbpf::Package>> packages;
    static std::vector<std::string> names;
    if (names.empty()) {
        const char* language = std::getenv("SC4_LANGUAGE");
        names.push_back(std::string(language && *language ? language : "English") + "/SimCityLocale.dat");
        // later packages override earlier ones in the game's resource manager: the expansion first, then the base set newest first
        for (const char* name : {"EP1.dat", "SimCity_5.dat", "SimCity_4.dat", "SimCity_3.dat", "SimCity_2.dat", "SimCity_1.dat", "Intro.dat"})
            names.push_back(name);
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i == packages.size()) {   // open the next one only when the earlier ones do not have it
            auto package = std::make_unique<dbpf::Package>();
            const std::string path = FindDataFile(names[i]);
            if (!path.empty()) package->Open(path);
            packages.push_back(std::move(package));
        }
        const dbpf::Entry* entry = packages[i]->Find(type, group, instance);
        if (entry && packages[i]->Read(*entry, bytes)) return true;
    }
    return false;
}

cGZBuffer* LoadBitmapResource(std::uint32_t group, std::uint32_t instance) {
    if (!g_decodePng) return nullptr;
    std::vector<std::uint8_t> bytes;
    if (!LoadResourceBytes(0x856ddbac, group, instance, bytes)) return nullptr;
    return g_decodePng(bytes);
}

PngDecoder DecodeFunction() { return g_decodePng; }

namespace {

class HostScriptServices final : public IVocabularyServices, public IScriptControlHost, public IScriptTerpHost {
public:
    // ---- IVocabularyServices (spLocalVocab)
    IVocabulary* Cached() override { return cached_; }
    void AddRefCached() override { if (cached_) cached_->AddRef(); }
    void ReleaseCached() override { if (cached_ && cached_->Release() == 0) cached_ = nullptr; }
    bool CreateCached() override {
        void* object = nullptr;
        if (!GZCOM() || !GZCOM()->CreateInstance(0xba2e7954, kGZIID_cIGZScriptVocab, &object) || !object) return false;
        cached_ = reinterpret_cast<IVocabulary*>(static_cast<IScriptVocab*>(static_cast<cIGZUnknown*>(object)));
        return true;
    }
    // ---- IScriptTerpHost
    IScriptVocab* CreateVocabulary() override { auto* v = new cGZScriptVocab(); v->AddRef(); return v; }
    // ---- IScriptControlHost
    cIGZWin* CreateControl(std::uint32_t clsid, std::uint32_t iid) override {
        void* object = nullptr;
        if (!GZCOM() || !GZCOM()->CreateInstance(clsid, iid, &object) || !object) {
            if (std::getenv("HOST_DEBUG")) std::printf("ui: no class for control clsid %08x (iid %08x)\n", clsid, iid);
            return nullptr;
        }
        void* window = nullptr;
        const bool ok = static_cast<cIGZUnknown*>(object)->QueryInterface(0x22ba0121, &window);
        static_cast<cIGZUnknown*>(object)->Release();
        return ok ? static_cast<cIGZWin*>(window) : nullptr;
    }
    cIGZWin* GetMainWindow() override { return g_mgr->GetMainWindow(); }
    bool UnscriptWindow(cIGZWin*, cIGZString&) override { return false; }   // unscript_IGZWin is not recovered
    bool LoadScriptString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override { return LoadLocalizedString(group, instance, out); }
    cIGZBuffer* LoadScriptBuffer(std::uint32_t group, std::uint32_t instance) override { return LoadBitmapResource(group, instance); }
    // the colour manager's font mapping: a style name of the [Font Styles] table -> its style id
    std::uint32_t FontStyleFromName(const cIGZString& name) override {
        gz::IWinFontSource* fonts = gz::WinFontSource();
        return fonts ? fonts->StyleId(name.ToChar()) : 0;
    }
    // the Graphic Options' four resolution labels: the logical resolutions this window offers (ResolutionChoice)
    void GetNativeResolution(int index, int& width, int& height) override { ResolutionChoice(index, width, height); }
private:
    IVocabulary* cached_ = nullptr;
} g_services;

template <class T>
cIGZUnknown* MakeWin() { return static_cast<cIGZUnknown*>(static_cast<cIGZWin*>(new T(g_mgr))); }

}  // namespace

void SetPngDecoder(PngDecoder decoder) { g_decodePng = decoder; }

namespace {
int g_windowW = 1024, g_windowH = 768, g_gameH = 768;
}  // namespace
void SetDisplayGeometry(int windowWidth, int windowHeight, int gameHeight) {
    g_windowW = windowWidth; g_windowH = windowHeight; g_gameH = gameHeight;
}
// The four logical resolutions on offer: the largest four of the usual heights that fit the window (so the UI never
// shrinks below its drawn size), each as wide as the window's aspect ratio makes it.
bool ResolutionChoice(int index, int& width, int& height) {
    static constexpr int kHeights[] = {600, 720, 768, 900, 1080, 1200, 1440, 1600, 2160};
    std::vector<int> fit;
    for (int h : kHeights) if (h <= g_windowH) fit.push_back(h);
    if (fit.empty()) fit.push_back(g_windowH);
    while (fit.size() > 4) fit.erase(fit.begin());
    if (index < 0 || index >= int(fit.size())) { width = height = 0; return false; }
    height = fit[std::size_t(index)];
    width = int(std::lround(double(height) * g_windowW / std::max(1, g_windowH)));
    return true;
}
int CurrentGameHeight() { return g_gameH; }

int LoadDisplayHeight() {
    std::ifstream in(DocumentsDirectory() + "/display.conf");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("game-height=", 0) == 0) return std::atoi(line.c_str() + 12);
    return 0;
}
void SaveDisplayHeight(int height) {
    std::error_code error;
    std::filesystem::create_directories(DocumentsDirectory(), error);
    std::ofstream(DocumentsDirectory() + "/display.conf") << "# the game's logical height (Graphic Options > Resolution); --scale overrides it\ngame-height=" << height << "\n";
}

// The game's string resources (LTEXT, type 0x2026960b): a u16 character count, the marker 0x1000, then that many UTF-16LE
// characters; a record without the marker is plain 8-bit text. Stored as UTF-8, the encoding the native fonts read.
bool LoadLocalizedString(std::uint32_t group, std::uint32_t instance, cIGZString& out) {
    std::vector<std::uint8_t> bytes;
    if (!LoadResourceBytes(0x2026960b, group, instance, bytes) || bytes.empty()) return false;
    std::string text;
    if (bytes.size() >= 4 && bytes[2] == 0x00 && bytes[3] == 0x10) {
        const std::size_t count = std::min<std::size_t>(std::size_t(bytes[0]) | std::size_t(bytes[1]) << 8, (bytes.size() - 4) / 2);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t c = std::uint32_t(bytes[4 + 2 * i]) | std::uint32_t(bytes[5 + 2 * i]) << 8;
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < count) {   // a surrogate pair
                const std::uint32_t low = std::uint32_t(bytes[6 + 2 * i]) | std::uint32_t(bytes[7 + 2 * i]) << 8;
                if (low >= 0xdc00 && low < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00); ++i; }
            }
            if (c < 0x80) text += char(c);
            else if (c < 0x800) { text += char(0xc0 | c >> 6); text += char(0x80 | (c & 0x3f)); }
            else if (c < 0x10000) { text += char(0xe0 | c >> 12); text += char(0x80 | (c >> 6 & 0x3f)); text += char(0x80 | (c & 0x3f)); }
            else { text += char(0xf0 | c >> 18); text += char(0x80 | (c >> 12 & 0x3f)); text += char(0x80 | (c >> 6 & 0x3f)); text += char(0x80 | (c & 0x3f)); }
        }
    } else {
        text.assign(bytes.begin(), bytes.end());
    }
    out.AssignFromChars(text.c_str(), static_cast<std::uint32_t>(text.size()));
    return true;
}

// RZ::WCM()'s control entries for the composite controls (the grid's and list box's scroll bars, the option groups'
// children) and its stock images: built from the recovered classes. Nothing answered them natively, so a grid never got
// its scroll bar.
class CtrlFactory final : public gz::IWinCtrlFactory {
public:
    bool CreateCtrl(std::uint32_t classId, std::uint32_t iid, void** out) override {
        return GZCOM() && GZCOM()->CreateInstance(classId, iid, out) && *out;
    }
    // cGZWinCtrlMgr::CreateScrollbar (Mac i386 0x0016a9f2): the id, the direction, the stock scroll bar skin, the range
    // and the value.
    gz::cIGZWinScrollbar* CreateScrollbar(std::uint32_t id, int direction, int low, int high, int value) override {
        auto* bar = new gz::cGZWinScrollbar(g_mgr);
        auto* view = static_cast<gz::cIGZWinScrollbar*>(bar);
        view->AddRef();
        gz::cIGZWinSlider* slider = view->AsIGZWinSlider();
        view->AsIGZWin()->SetID(id);
        slider->SetDirection(static_cast<gz::tSliderWindowDirection>(direction));
        // the slider header forward-declares its own gz::cIGZBuffer; the stock image is the same object (cf. IWinGridHost::AsScrollBarImage)
        slider->SetImage(reinterpret_cast<gz::cIGZBuffer*>(Image(0xe8963ec7)), nullptr);
        slider->SetMinimumValue(low);
        slider->SetMaximumValue(high);
        view->SetValue(value);
        return view;
    }
    std::uint32_t DefaultColor(int) override { return 0xff000000u; }
    std::uint32_t DoubleClickMilliseconds() override { return 500; }
    // The stock images cSC4App::SetupWinCtrlMgr (Mac i386 0x0000d190) registers: UI art {0x856ddbac, 0x46a006b0, instance}
    // under the window control manager's ids. Loaded on first use, kept for the session (borrowed pointers).
    cIGZBuffer* Image(std::uint32_t id) override {
        static constexpr std::pair<std::uint32_t, std::uint32_t> kStock[] = {   // {instance, stock id}
            {0x46a006a2, 0xa8963eb8}, {0x46a006a2, 0xe8963ef1}, {0x144161ea, 0x68963ebe}, {0x144161ea, 0xa8963ef3},
            {0x0000000a, 0xc8963ef6}, {0x82b99d9d, 0x68963ed6}, {0x62b99d31, 0x68963ed8}, {0x42e55fd4, 0x48963eee},
            {0xe78ffc90, 0xe8963edf}, {0xc2b66daa, 0xe8963ee5}, {0x46a006a4, 0xc8963ed3}, {0x46a006a6, 0xe8963ec7},
            {0x46a006a7, 0x88963ec1}, {0x46a006a8, 0xa8963ec4}, {0x46a006a5, 0xa8963eea}, {0xe2b14588, 0xe2b14588},
            {0x62b19ce9, 0xe2b14589}};
        auto cached = images_.find(id);
        if (cached != images_.end()) return cached->second;
        cIGZBuffer* image = nullptr;
        for (const auto& [instance, stock] : kStock)
            if (stock == id) { image = LoadBitmapResource(0x46a006b0, instance); break; }
        images_[id] = image;   // the table keeps the reference
        return image;
    }

private:
    std::map<std::uint32_t, cIGZBuffer*> images_;
};

bool InitUiScripts(WinMgr* mgr) {
    g_mgr = mgr;
    static CtrlFactory ctrlFactory;
    gz::SetWinCtrlFactory(&ctrlFactory);
    // cSLString: the strings come straight from the packages (the resource manager has no string factory natively)
    static struct : sc4::ISLStringHost {
        int LanguageOffset() override { return 0; }
        sc4::cSCStringDetokenizer* Detokenizer() override { return nullptr; }
        bool LoadText(const cGZPersistResourceKey& key, cIGZString& out) override { return LoadLocalizedString(key.group, key.instance, out); }
    } stringHost;
    sc4::SetSLStringHost(&stringHost);
    cGZFramework* framework = BootFramework();
    if (!framework || !GZCOM()) { std::printf("ui: no framework (run the boot first)\n"); return false; }

    // GZCOM class makers: the recovered classes the scripts create (constructors take the window manager)
    gz::RegisterDirectorMaker("cGZUIScriptService", +[]() -> cIGZUnknown* { return static_cast<cIGZUIScriptService*>(new cGZUIScriptService()); });
    gz::RegisterDirectorMaker("cGZScriptableControlSet", +[]() -> cIGZUnknown* { return static_cast<cIGZScriptableControlSet*>(new cGZScriptableControlSet()); });
    gz::RegisterDirectorMaker("cGZLegacyControl", +[]() -> cIGZUnknown* { return static_cast<cIGZScriptableControl*>(new cGZLegacyControl()); });
    gz::RegisterDirectorMaker("cGZScriptVocab", +[]() -> cIGZUnknown* { return static_cast<IScriptVocab*>(new cGZScriptVocab()); });
    gz::RegisterDirectorMaker("cGZScriptTerp", +[]() -> cIGZUnknown* { return static_cast<cIGZScriptTerp*>(new cGZScriptTerp()); });
    gz::RegisterDirectorMaker("cGZWinGen", +[]() -> cIGZUnknown* { return MakeWin<gz::cGZWinGen>(); });
    gz::RegisterDirectorMaker("cGZWinText", +[]() -> cIGZUnknown* { return MakeWin<gz::cGZWinText>(); });
    gz::RegisterDirectorMaker("cGZWinBtn", +[]() -> cIGZUnknown* { return MakeWin<gz::cGZWinBtn>(); });
    gz::RegisterDirectorMaker("cGZWinBMP", +[]() -> cIGZUnknown* { return MakeWin<gz::cGZWinBMP>(); });
    gz::RegisterDirectorMaker("cGZWinCustom", +[]() -> cIGZUnknown* { return MakeWin<gz::cGZWinCustom>(); });
    gz::RegisterDirectorMaker("cSC4WinGenTransparent", +[]() -> cIGZUnknown* { return MakeWin<sc4::cSC4WinGenTransparent>(); });
    // cSC4WinText (clsid 0xaa7cecfd) is not recovered (3 functions); a plain cGZWinText stands in for it
    gz::RegisterDirectorMaker("cSC4WinText", +[]() -> cIGZUnknown* { return MakeWin<gz::cGZWinText>(); });
    gz::RegisterDirectorMaker("cGZWinFlatRect", +[]() -> cIGZUnknown* { return MakeWin<gz::cGZWinFlatRect>(); });
    gz::RegisterDirectorMaker("cSC4WinMapView", +[]() -> cIGZUnknown* { return MakeWin<sc4::cSC4WinMapView>(); });   // clsid 0x28c5a41f
    // The slider (clsid 0x21325208) and the grid (0xdaa6b9bf): without makers the scripts built bare windows in their place
    // (the audio options' volume sliders drew as filled boxes and its playlist took no rows).
    gz::RegisterDirectorMaker("cGZWinSlider", +[]() -> cIGZUnknown* { return MakeWin<gz::cGZWinSlider>(); });
    // The list box (clsid 0x598) and the line input (0x21335c5a): the region dialogs' region list and name field.
    gz::RegisterDirectorMaker("cGZWinListBox", +[]() -> cIGZUnknown* {
        return static_cast<cIGZUnknown*>(static_cast<cIGZWin*>(new gz::cGZWinListBox(g_mgr, false)));
    });
    gz::RegisterDirectorMaker("cGZWinLineInput", +[]() -> cIGZUnknown* {
        return static_cast<cIGZUnknown*>(static_cast<cIGZWin*>(new gz::cGZWinLineInput(g_mgr, true)));
    });
    gz::RegisterDirectorMaker("cGZWinGrid", +[]() -> cIGZUnknown* {   // the director's default flags (Mac i386 CreateGZWinGrid 0x0073c2ee)
        return static_cast<cIGZUnknown*>(static_cast<cIGZWin*>(new gz::cGZWinGrid(g_mgr, 0x19bff)));
    });

    // seams
    SetVocabularyServices(&g_services);
    SetScriptControlHost(&g_services);
    SetScriptTerpHost(&g_services);

    // the window manager is a framework service (the script service reads the main window size from it)
    framework->AddSystemService(static_cast<cIGZSystemService*>(mgr));

    void* service = nullptr;
    if (!GZCOM()->CreateInstance(0xfa3562fb, kGZIID_cIGZUIScriptService, &service) || !service) {
        std::printf("ui: GZCOM could not create the UI script service\n");
        return false;
    }
    g_service = static_cast<cIGZUIScriptService*>(service);
    // ... and a framework service too (system service 0x5a356e15): SC4CreateWindowFromScript and the recovered dialogs find it there.
    framework->AddSystemService(static_cast<cIGZSystemService*>(static_cast<cGZUIScriptService*>(g_service)));
    return true;
}

cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) {
    if (!g_service) return nullptr;
    return g_service->CreateWindowFromScript(key, parent, id);
}

}  // namespace native

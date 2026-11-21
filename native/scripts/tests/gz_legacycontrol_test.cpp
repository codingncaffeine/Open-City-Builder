// cGZLegacyControl — test. Exercises the three vtables of the adapter, the vocabulary bootstrap (the real
// keyword tables read out of the binary), SetParameters over a fake GZCOM/script host, the common and flat-rect
// property interpreters, the window-proc exception path and the script fragment.
//
// Build (from the repo root):
//   clang++ -std=c++20 -Wall -Wextra -O1 -fsanitize=address,undefined -Isrc -Iscripts/tests/stubs
//       scripts/tests/gz_legacycontrol_test.cpp src/gz/LegacyControl.cpp src/gz/WinFlatRect.cpp src/gz/Win.cpp
//       src/gz/DrawContext.cpp src/rz/cRZString.cpp src/rz/cRZVariant.cpp -o /tmp/lc_test
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "gz/LegacyControl.h"
#include "gz/WinFolders.h"
#include "gz/Win.h"
#include "gz/WinFlatRect.h"
#include "gz/WinGen.h"
#include "gz/WinBMP.h"
#include "gz/WinScrollbar.h"
#include "gz/WinBtn.h"
#include "gz/WinSlider.h"
#include "gz/WinSpinner.h"
#include "gz/WinText.h"
#include "gz/WinTextEdit.h"
#include "rz/cRZVariant.h"

static int gFailures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            ++gFailures;                                                              \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        if (!((a) == (b))) {                                                                              \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);                            \
            ++gFailures;                                                                                  \
        }                                                                                                 \
    } while (0)

// ---------------------------------------------------------------------------------------------------------
// Fakes: the script vocabulary (cGZScriptVocab), its service, GZCOM/window-manager/UI-script host, and the
// interfaces the adapter queries.
// ---------------------------------------------------------------------------------------------------------
namespace {

struct FakeVocabulary final : IVocabulary {
    struct Entry {
        std::string name;
        std::uint32_t value;
    };
    std::vector<Entry> entries;
    std::uint32_t refs = 1;   // the creation reference
    int rejected = 0;         // AddTranslation on an existing key (the map is case-insensitive)

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    // cGZScriptVocab::Release destroys itself at count 1 (or 0) and answers 0.
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        refs = 0;
        return 0;
    }
    bool AddTranslation(const cIGZString& name, std::uint32_t value) override {
        const std::string key = name.ToChar();
        for (const Entry& entry : entries) {
            if (entry.name.size() != key.size()) continue;
            bool same = true;
            for (std::size_t i = 0; i < key.size() && same; ++i) {
                const unsigned char a = static_cast<unsigned char>(entry.name[i]);
                const unsigned char b = static_cast<unsigned char>(key[i]);
                const unsigned char lowerA = (a >= 'A' && a <= 'Z') ? a + 32 : a;
                const unsigned char lowerB = (b >= 'A' && b <= 'Z') ? b + 32 : b;
                same = lowerA == lowerB;
            }
            if (same) {
                ++rejected;
                return false;
            }
        }
        entries.push_back({key, value});
        return true;
    }
    bool AddTranslation(const cIGZString&, cIGZVariant*) override { return false; }

    bool Has(const char* name, std::uint32_t expected) const {
        for (const Entry& entry : entries) {
            if (entry.name == name) return entry.value == expected;
        }
        return false;
    }
    bool Contains(const char* name) const {
        for (const Entry& entry : entries) {
            if (entry.name == name) return true;
        }
        return false;
    }
    int Duplicates() const {
        int duplicates = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            for (std::size_t j = i + 1; j < entries.size(); ++j) {
                if (entries[i].name == entries[j].name) ++duplicates;
            }
        }
        return duplicates;
    }
};

struct FakeVocabularyServices final : IVocabularyServices {
    FakeVocabulary* vocab = nullptr;
    int creates = 0, addRefs = 0, releases = 0;

    IVocabulary* Cached() override { return vocab; }
    void AddRefCached() override {
        ++addRefs;
        if (vocab) vocab->AddRef();
    }
    void ReleaseCached() override {
        ++releases;
        if (!vocab) return;
        vocab->Release();
        if (vocab->refs == 0) {
            delete vocab;
            vocab = nullptr;
        }
    }
    bool CreateCached() override {
        ++creates;
        if (!vocab) vocab = new FakeVocabulary();
        return true;
    }
};

struct FakeProc final : gz::cIGZWinProc {
    std::uint32_t refs = 1;
    int released = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        ++released;
        return refs > 1 ? --refs : 0;
    }
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }
};

// Minimal cIGZWinGen (the "exception" window the adapter hands its window proc to).
struct FakeGen final : gz::cIGZWinGen {
    std::uint32_t refs = 1;
    gz::cIGZWinProc* proc = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs > 1 ? --refs : 0; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    std::uint32_t GetCLSID() const override { return gz::kGZCLSID_cGZWinGen; }
    bool GetWinGenFlag(int) const override { return false; }
    bool SetWinGenFlag(int, bool) override { return false; }
    bool SetWinProc(gz::cIGZWinProc* p) override {
        if (proc && proc != p) proc->Release();   // the previous procedure (the adapter's dummy) is dropped
        proc = p;
        return true;
    }
    gz::cIGZWinProc* GetWinProc() const override { return proc; }
    bool GetWinProcAs(std::uint32_t, void** out) const override { (void)out; return false; }
    bool GetUserArea(cRZRect& out) const override { out = cRZRect{}; return true; }
    bool SetUserArea(const cRZRect&) override { return false; }
    bool SetBackgroundBitmap(gz::BGBltType, cIGZBuffer*) override { return false; }
    bool GetBackgroundBitmap(gz::BGBltType&, cIGZBuffer*&) const override { return false; }
    bool SetBackgroundBitmap(cIGZBuffer*) override { return false; }
    bool GetBackgroundBitmap(cIGZBuffer** out) const override { *out = nullptr; return false; }
    bool SetBackgroundBlitType(gz::BGBltType) override { return false; }
    bool GetBackgroundBlitType(gz::BGBltType&) const override { return false; }
    bool SetUserData(std::uint32_t, std::uint32_t) override { return false; }
    bool GetUserData(std::uint32_t, std::uint32_t&) const override { return false; }
    bool SetUserData(std::uint32_t) override { return false; }
    std::uint32_t GetUserData() const override { return 0; }
    void SetUserData(std::uint32_t, cIGZUnknown*) override {}
    bool GetUserData(std::uint32_t, std::uint32_t, void**) override { return false; }
    void RemoveUserData(std::uint32_t) override {}
    bool SetGutters(std::uint8_t, std::uint8_t) override { return false; }
    bool GetGutters(int&, int&) override { return false; }
    void SetGutters(std::int8_t, std::int8_t, std::int8_t, std::int8_t) override {}
    bool GetGutters(int&, int&, int&, int&) const override { return false; }
    bool AddColorBar(int, int, int, const cRZRect&) override { return false; }
    void AddDividerH(int, int) override {}
    bool RemoveColorBar(int) override { return false; }
};

// A flat rectangle that also answers the generic-window interface, so DoExceptions has something to talk to.
struct FlatGenWindow final : gz::cGZWinFlatRect {
    explicit FlatGenWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeGen gen;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinGen) {
            gen.AddRef();
            *out = static_cast<gz::cIGZWinGen*>(&gen);
            return true;
        }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

// The outline view as far as the adapter uses it: slots 2/3/5/13 of the recovered cIGZWinOutline vtable order.
struct FakeOutline {
    std::uint32_t flags = 0;
    int colors[3] = {0, 0, 0};
    std::uint8_t color[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    int polyCount = 0;
    cRZPoint poly[32];
    int releases = 0;

    virtual bool QueryInterface(std::uint32_t, void**) { return false; }
    virtual std::uint32_t AddRef() { return 1; }
    virtual std::uint32_t Release() { ++releases; return 1; }
    virtual void SetFlags(std::uint32_t f) { flags = f; }
    virtual std::uint32_t GetFlags() { return flags; }
    virtual void SetColor(int which, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (which >= 0 && which < 3) {
            colors[which] = 1;
            color[which][0] = r; color[which][1] = g; color[which][2] = b;
        }
    }
    virtual void GetColor(int, std::uint8_t&, std::uint8_t&, std::uint8_t&) {}
    virtual std::uint32_t GetFillColor() { return 0; }
    virtual void SetFillColor(std::uint32_t) {}
    virtual void SetDarkOutlineColor(std::uint32_t) {}
    virtual std::uint32_t GetDarkOutlineColor() { return 0; }
    virtual std::uint32_t GetLightOutlineColor() { return 0; }
    virtual void SetLightOutlineColor(std::uint32_t) {}
    virtual bool SetShapePoly(int count, const cRZPoint* points, bool resize, bool resizeToBounds) {
        (void)resize; (void)resizeToBounds;
        polyCount = count < 32 ? count : 32;
        for (int i = 0; i < polyCount; ++i) poly[i] = points[i];
        return true;
    }
};

struct OutlineWindow final : gz::cGZWinFlatRect {
    explicit OutlineWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeOutline outline;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0xe303e5aeu) {
            *out = &outline;
            return true;
        }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

// The slider view (iid 0x21325207).
struct FakeSlider final : gz::cIGZWinSlider {
    int minimum = 0, maximum = 0, value = 0;
    gz::tSliderWindowDirection direction = gz::kSliderVertical;
    int releases = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { ++releases; return 1; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    int GetValue() const override { return value; }
    bool SetValue(int v) override { value = v; return true; }
    void SetMinimumValue(int v) override { minimum = v; }
    int GetMinimumValue() const override { return minimum; }
    void SetMaximumValue(int v) override { maximum = v; }
    int GetMaximumValue() const override { return maximum; }
    void SetDirection(gz::tSliderWindowDirection d) override { direction = d; }
    void GetDirection(gz::tSliderWindowDirection& d) const override { d = direction; }
    bool SetImage(cIGZBuffer*, const cRZRect*) override { return false; }
    bool GetImage(cIGZBuffer*&, cRZRect&) const override { return false; }
};

struct SliderWindow final : gz::cGZWinFlatRect {
    explicit SliderWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeSlider slider;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinSlider) {
            *out = &slider;
            return true;
        }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

// The scrollbar view (iid 0x61325a2d).
struct FakeScrollbar final : gz::cIGZWinScrollbar {
    int minimum = 0, maximum = 0, value = 0, orientation = 1, pageSize = 0, lineSize = 0;
    std::uint32_t lines = 0, pages = 0;
    int releases = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { ++releases; return 1; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    gz::cIGZWinSlider* AsIGZWinSlider() override { return nullptr; }
    bool SetValue(int v) override { value = v; return true; }
    int GetValue() const override { return value; }
    bool SetMinMax(int min, int max) override { minimum = min; maximum = max; return true; }
    bool GetMinMax(int& min, int& max) const override { min = minimum; max = maximum; return true; }
    bool SetOrientation(int o) override { orientation = o; return true; }
    int GetOrientation() const override { return orientation; }
    bool SetPageSize(int size) override { pageSize = size; return true; }
    std::uint32_t GetPageSize() const override { return static_cast<std::uint32_t>(pageSize); }
    bool SetLineSize(int size) override { lineSize = size; return true; }
    std::uint32_t GetLineSize() const override { return static_cast<std::uint32_t>(lineSize); }
    void SetLineAndPageCount(std::uint32_t l, std::uint32_t p) override { lines = l; pages = p; }
    void GetLineAndPageCount(std::uint32_t& l, std::uint32_t& p) const override { l = lines; p = pages; }
    bool ImageSetup(const cRZRect&, cIGZBuffer*, int) override { return false; }
    bool GetImage(cRZRect&, cIGZBuffer*&, int&) const override { return false; }
    bool GetMinMaxInfo(cRZRect&) const override { return false; }
};

struct ScrollbarWindow final : gz::cGZWinFlatRect {
    explicit ScrollbarWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeScrollbar bar;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinScrollbar) {
            *out = &bar;
            return true;
        }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

// The bitmap view (iid 0xc12cea13).
struct FakeBmp final : gz::cIGZWinBMP {
    bool alphaEnabled = false;
    float alpha = 0.0f;
    cRZRect imageArea{};
    bool hasImageArea = false;
    int flags = 0;
    int releases = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { ++releases; return 1; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    void SetBMP(cIGZBuffer*) override {}
    bool GetBMP(std::uint32_t, void** out) const override { (void)out; return false; }
    void SetAlphaBlending(bool enable, float a) override { alphaEnabled = enable; alpha = a; }
    void GetAlphaBlending(bool& enable, float& a) const override { enable = alphaEnabled; a = alpha; }
    void SetImageArea(const cRZRect& area) override { imageArea = area; hasImageArea = true; }
    void GetImageArea(cRZRect& area) const override { area = imageArea; }
    bool GetWinBMPFlag(int flag) const override { return (flags & flag) != 0; }
    void SetWinBMPFlag(int flag, bool on) override {
        if (on) flags |= flag; else flags &= ~flag;
    }
};

struct BmpWindow final : gz::cGZWinFlatRect {
    explicit BmpWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeBmp bmp;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinBMP) {
            *out = &bmp;
            return true;
        }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

struct FakeSc3Gen final : gz::cIGZWinGen {
    std::map<int, bool> flags;
    std::uint32_t userData = 0;
    bool userDataSet = false;
    int gutterX = -1, gutterY = -1;
    gz::cIGZWinProc* proc = nullptr;
    std::vector<std::pair<int, int>> dividers;
    struct Bar { int id, sizeMode, outline; cRZRect area; };
    std::vector<Bar> bars;
    int releases = 0, bitmapCalls = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { ++releases; return 1; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    std::uint32_t GetCLSID() const override { return 0; }
    bool GetWinGenFlag(int flag) const override { auto it = flags.find(flag); return it != flags.end() && it->second; }
    bool SetWinGenFlag(int flag, bool on) override { flags[flag] = on; return true; }
    bool SetWinProc(gz::cIGZWinProc* p) override { proc = p; if (p) p->AddRef(); return true; }
    gz::cIGZWinProc* GetWinProc() const override { return proc; }
    bool GetWinProcAs(std::uint32_t, void**) const override { return false; }
    bool GetUserArea(cRZRect&) const override { return false; }
    bool SetUserArea(const cRZRect&) override { return false; }
    bool SetBackgroundBitmap(gz::BGBltType, cIGZBuffer*) override { ++bitmapCalls; return true; }
    bool GetBackgroundBitmap(gz::BGBltType&, cIGZBuffer*&) const override { return false; }
    bool SetBackgroundBitmap(cIGZBuffer*) override { return false; }
    bool GetBackgroundBitmap(cIGZBuffer**) const override { return false; }
    bool SetBackgroundBlitType(gz::BGBltType) override { return false; }
    bool GetBackgroundBlitType(gz::BGBltType&) const override { return false; }
    bool SetUserData(std::uint32_t, std::uint32_t) override { return false; }
    bool GetUserData(std::uint32_t, std::uint32_t&) const override { return false; }
    bool SetUserData(std::uint32_t value) override { userData = value; userDataSet = true; return true; }
    std::uint32_t GetUserData() const override { return userData; }
    void SetUserData(std::uint32_t, cIGZUnknown*) override {}
    bool GetUserData(std::uint32_t, std::uint32_t, void**) override { return false; }
    void RemoveUserData(std::uint32_t) override {}
    bool SetGutters(std::uint8_t x, std::uint8_t y) override { gutterX = x; gutterY = y; return true; }
    bool GetGutters(int&, int&) override { return false; }
    void SetGutters(std::int8_t, std::int8_t, std::int8_t, std::int8_t) override {}
    bool GetGutters(int&, int&, int&, int&) const override { return false; }
    bool AddColorBar(int id, int sizeMode, int outline, const cRZRect& area) override { bars.push_back({id, sizeMode, outline, area}); return true; }
    void AddDividerH(int id, int y) override { dividers.emplace_back(id, y); }
    bool RemoveColorBar(int) override { return false; }
};

struct Sc3GenWindow final : gz::cGZWinFlatRect {
    explicit Sc3GenWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeSc3Gen gen;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinGen) { *out = &gen; return true; }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

struct FakeText final : gz::cIGZWinText {
    std::map<int, bool> flags;
    std::vector<std::pair<int, std::uint32_t>> colors;
    std::uint32_t style = 0xffffffffu, alignment = 0xffffffffu;
    int gutterX = -9, gutterY = -9, offX = -9, offY = -9;
    int fits = 0, releases = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { ++releases; return 1; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    void SetCaption(const cIGZString&) override {}
    bool FitWindowToText(bool, bool) override { ++fits; return true; }
    bool GetWinTextFlag(int) override { return false; }
    bool SetWinTextFlag(int flag, bool on) override { flags[flag] = on; return true; }
    void SetTextColor(std::uint32_t) override {}
    void GetTextColor(std::uint32_t&) override {}
    void ClearTextColor() override {}
    bool SetTextColor(tFontColorIdx index, std::uint32_t native) override { colors.emplace_back(int(index), native); return true; }
    bool GetTextColor(tFontColorIdx, std::uint32_t&) const override { return false; }
    bool SetTextColor(tFontColorIdx, gz::cRZColor) override { return false; }
    bool GetTextColor(tFontColorIdx, gz::cRZColor&) const override { return false; }
    bool ClearTextColor(tFontColorIdx) override { return false; }
    void SetBackgroundOpaque(bool) override {}
    bool IsBackgroundOpaque() override { return false; }
    bool SetFont(gz::IWinFont*) override { return false; }
    bool SetFontStyle(std::uint32_t id) override { style = id; return true; }
    bool GetFontStyle(std::uint32_t&) override { return false; }
    bool SetAlignment(std::uint32_t a) override { alignment = a; return true; }
    bool GetAlignment(std::uint32_t&) const override { return false; }
    bool SetGutters(std::int8_t x, std::int8_t y) override { gutterX = x; gutterY = y; return true; }
    bool GetGutters(std::int8_t&, std::int8_t&) override { return false; }
    bool SetTextOffsets(std::int8_t x, std::int8_t y) override { offX = x; offY = y; return true; }
    bool GetTextOffsets(std::int8_t&, std::int8_t&) override { return false; }
    void SetOutlineUse(bool, std::uint32_t) override {}
    void GetOutlineUse(bool&, std::uint32_t&) const override {}
};

struct TextWindow final : gz::cGZWinFlatRect {
    explicit TextWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeText text;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinText) { *out = &text; return true; }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

struct RgbGraphics final : IWinGraphics {
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return 0xff000000u | (r << 16) | (g << 8) | b; }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    IWinBuffer* PrimaryBuffer() override { return nullptr; }
    bool DrawSystemActive() override { return false; }
};

struct FakeBtn final : gz::cIGZWinBtn {
    std::map<int, bool> flags;
    std::vector<std::pair<int, std::uint32_t>> colors;
    int styleCalls = 0, style = -1; bool styleHasImage = false; cRZRect styleArea{};
    std::uint32_t fontStyle = 0; int fontSets = 0; gz::IWinFont* font = reinterpret_cast<gz::IWinFont*>(1);
    int captionStyle = -1, g1x = -1, g1y = -1, g4[4] = {-1, -1, -1, -1};
    std::string tip; int tipType = -1, tipX = -99, tipY = -99;
    std::uint32_t sounds[2] = {7, 7};
    int releases = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { ++releases; return 1; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    bool GetBtnFlag(int f) const override { auto it = flags.find(f); return it != flags.end() && it->second; }
    bool SetBtnFlag(int f, bool on) override { flags[f] = on; return true; }
    bool ToggleOn() override { return false; }
    bool ToggleOff() override { return false; }
    bool IsOn() const override { return false; }
    bool SetChecked(bool) override { return false; }
    bool IsChecked() const override { return false; }
    int GetStyle() override { return 0; }
    bool SetStyle(int st, cIGZBuffer* image, const cRZRect* area) override { ++styleCalls; style = st; styleHasImage = image != nullptr; if (area) styleArea = *area; return true; }
    bool SetButtonStyle(int) override { return false; }
    int GetImageW() const override { return 0; }
    int GetImageH() const override { return 0; }
    bool GetImageArea(cRZRect&) const override { return false; }
    bool SetBuffer(cIGZBuffer*) override { return false; }
    bool GetBuffer(cIGZBuffer*&) const override { return false; }
    bool SetFont(gz::IWinButtonFont* f) override { ++fontSets; font = f; return true; }
    bool SetFontStyle(std::uint32_t id) override { fontStyle = id; return true; }
    bool GetFontStyle(std::uint32_t&) override { return false; }
    bool SetFontColor(int index, std::uint32_t native) override { colors.emplace_back(index, native); return true; }
    bool GetFontColor(int, std::uint32_t&) const override { return false; }
    bool SetFontColor(int, gz::cRZColor) override { return false; }
    bool GetFontColor(int, gz::cRZColor&) const override { return false; }
    bool ClearFontColor(int) override { return false; }
    bool SetCaptionStyle(int st) override { captionStyle = st; return true; }
    bool GetCaptionStyle(int&) const override { return false; }
    bool SetGutters(std::uint8_t x, std::uint8_t y) override { g1x = x; g1y = y; return true; }
    bool GetGutters(int&, int&) const override { return false; }
    bool SetGutters(std::uint8_t l, std::uint8_t t, std::uint8_t r, std::uint8_t b) override { g4[0] = l; g4[1] = t; g4[2] = r; g4[3] = b; return true; }
    bool GetGutters(int&, int&, int&, int&) const override { return false; }
    bool FitSizeToContent() override { return false; }
    bool GetMinMaxInfo(cRZRect&) override { return false; }
    bool SetTipText(const cIGZString& t) override { tip = t.ToChar(); return true; }
    bool GetTipText(cIGZString&) const override { return false; }
    bool ClearTipText() override { return false; }
    bool SetTipPlacementType(int t) override { tipType = t; return true; }
    bool GetTipPlacementType(int&) const override { return false; }
    bool SetTipPlacementOffsets(int x, int y) override { tipX = x; tipY = y; return true; }
    bool GetTipPlacementOffsets(int&, int&) const override { return false; }
    bool SetTipColor(int, std::uint8_t, std::uint8_t, std::uint8_t) override { return false; }
    bool GetTipColor(int, std::uint8_t&, std::uint8_t&, std::uint8_t&) const override { return false; }
    void Flash(std::uint32_t, std::uint32_t) override {}
    bool SetSoundID(int which, std::uint32_t id) override { sounds[which] = id; return true; }
    std::uint32_t GetSoundID(int which) const override { return sounds[which]; }
};

struct BtnWindow final : gz::cGZWinFlatRect {
    explicit BtnWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeBtn btn;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinBtn) { *out = &btn; return true; }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

struct FakeTextEdit final : gz::cIGZWinTextEdit {
    std::uint32_t flags = 0x10b;
    int setFlagsCalls = 0, resizes = 0;
    std::uint32_t fontStyle = 0, maxText = 0, gutterX = 99, gutterY = 99, insertIndex = 99, caretPeriod = 0, undo = 0;
    std::uint32_t caret = 0, highlight = 0, line = 99, column = 99;
    std::vector<std::pair<int, std::uint32_t>> fontColors;
    bool outlineOn = false; std::uint32_t outlineColor = 0; bool insertMode = true;
    int vertImages = 0, horzImages = 0; cRZRect vertArea{}, horzArea{};
    int releases = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { ++releases; return 1; }
    void SetFlags(std::uint32_t f) override { flags = f; ++setFlagsCalls; }
    std::uint32_t GetFlags() override { return flags; }
    bool SetFontStyle(std::uint32_t id) override { fontStyle = id; return true; }
    bool SetFontColor(std::uint32_t) override { return false; }
    bool SetFontColor(ColorIndex index, std::uint32_t c) override { fontColors.emplace_back(int(index), c); return true; }
    void SetCaretColor(std::uint32_t c) override { caret = c; }
    void SetHighlightFillColor(std::uint32_t c) override { highlight = c; }
    void SetOutlineUse(bool on, std::uint32_t c) override { outlineOn = on; outlineColor = c; }
    void SetMaximumTextToAllow(std::uint32_t m) override { maxText = m; }
    void SetTextGutterX(std::uint32_t g) override { gutterX = g; }
    void SetTextGutterY(std::uint32_t g) override { gutterY = g; }
    bool SetInsertMode(bool m) override { insertMode = m; return true; }
    bool SetInsertionIndex(std::uint32_t i) override { insertIndex = i; return true; }
    bool SetInsertionLineAndColumn(std::uint32_t l, std::uint32_t c) override { line = l; column = c; return true; }
    bool SetCaretPeriod(std::uint32_t ms) override { caretPeriod = ms; return true; }
    void SetMaxUndoLevelCount(std::uint32_t n) override { undo = n; }
    bool SetScrollBarImageVertical(cIGZBuffer*, const cRZRect& a) override { ++vertImages; vertArea = a; return true; }
    bool SetScrollBarImageHorizontal(cIGZBuffer*, const cRZRect& a) override { ++horzImages; horzArea = a; return true; }
    bool ResizeWindowForExactLineHeights() override { ++resizes; return true; }
    bool Init() override { return false; }
    bool Shutdown() override { return false; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    void SetArea(int l, int t, int r, int b) override { }
    void GetTextArea(cRZRect& out) override { }
    cIGZFont* GetFont() const override { return nullptr; }
    void SetFont(cIGZFont* font) override { }
    bool GetFontStyle(std::uint32_t& styleId) override { return false; }
    bool GetFontColor(ColorIndex index, std::uint32_t& native) const override { return false; }
    bool SetColor(ColorIndex index, std::uint32_t color) override { return false; }
    bool GetColor(ColorIndex index, std::uint32_t& color) const override { return false; }
    bool ClearFontColor(ColorIndex index) override { return false; }
    bool GetCaretColor(std::uint32_t& native) const override { return false; }
    void SetFillColor(std::uint32_t native) override { }
    std::uint32_t GetHighlightFillColor() override { return 0; }
    void GetBackgroundImage(cIGZBuffer*& image, bool& stretch) override { }
    void SetBackgroundImage(cIGZBuffer* image, bool stretch) override { }
    bool ScrollLineUp() override { return false; }
    bool ScrollLineDown() override { return false; }
    bool ScrollToStart() override { return false; }
    bool ScrollToEnd() override { return false; }
    bool ScrollPageUp() override { return false; }
    bool ScrollPageDown() override { return false; }
    std::uint32_t GetMaximumTextToAllow() override { return 0; }
    std::uint32_t GetFirstVisibleLine() override { return 0; }
    std::uint32_t GetLineCount() override { return 0; }
    std::uint32_t GetLineHeight() override { return 0; }
    void GetOutlineUse(bool& on, std::uint32_t& native) const override { }
    std::uint32_t GetTextGutterX() override { return 0; }
    std::uint32_t GetTextGutterY() override { return 0; }
    const char* GetText() override { return nullptr; }
    std::uint32_t GetTextLength() override { return 0; }
    void GetText(cIGZString& out) override { }
    void GetText(char* buffer, std::uint32_t& length) override { }
    bool SetText(const char* text, std::uint32_t length, bool notify) override { return false; }
    void SetText(const cIGZString& text, bool notify) override { }
    std::uint32_t InsertText(const char* text, std::uint32_t length, std::uint32_t index) override { return 0; }
    void InsertText(const char* text, std::uint32_t length, std::uint32_t line, std::uint32_t column) override { }
    bool RemoveChar(std::uint32_t index, bool plain, cIGZString* removed) override { return false; }
    std::uint32_t RemoveText(std::uint32_t index, std::uint32_t length, cIGZString* removed) override { return 0; }
    void RemoveText(std::uint32_t line, std::uint32_t column, std::uint32_t length, cIGZString* removed) override { }
    void RemoveText(std::uint32_t line0, std::uint32_t column0, std::uint32_t line1, std::uint32_t column1, cIGZString* removed) override { }
    std::uint32_t FindText(const char* text, std::uint32_t length, std::uint32_t start, bool forward) override { return 0; }
    void GetIndexFromPosition(std::uint32_t& index, int x, int y) override { }
    void GetColumnFromPosition(std::uint32_t line, std::uint32_t& column, int x) override { }
    void GetLineAndColumnFromPosition(std::uint32_t& line, std::uint32_t& column, int x, int y) override { }
    void GetPositionFromLineAndColumn(std::uint32_t line, std::uint32_t column, int& x, int& y) override { }
    void GetLineAndColumnFromIndex(std::uint32_t& line, std::uint32_t& column, std::uint32_t index) override { }
    bool GetIndexFromLineAndColumn(std::uint32_t line, std::uint32_t column, std::uint32_t& index) override { return false; }
    std::uint32_t GetCharacterCountOfLine(std::uint32_t line) override { return 0; }
    bool GetInsertMode() const override { return false; }
    bool GetInsertionIndex(std::uint32_t& index) const override { return false; }
    bool GetInsertionLineAndColumn(std::uint32_t& line, std::uint32_t& column) const override { return false; }
    void IncrementInsertionPosition(int characters) override { }
    void IncrementInsertionPositionByWord(int words) override { }
    bool GetCaretPeriod(std::uint32_t& ms) const override { return false; }
    bool GetSelection(std::uint32_t& start, std::uint32_t& end) override { return false; }
    bool SetSelection(std::uint32_t start, std::uint32_t end) override { return false; }
    void SetFlag(std::uint32_t flag) override { }
    void ClearFlag(std::uint32_t flag) override { }
    bool IsFlagSet(std::uint32_t flag) override { return false; }
    std::uint32_t GetMaxUndoLevelCount() const override { return 0; }
    bool Undo() override { return false; }
    bool Redo() override { return false; }
    bool GetScrollBarImageVertical(cIGZBuffer*& image, cRZRect& area) const override { return false; }
    bool GetScrollBarImageHorizontal(cIGZBuffer*& image, cRZRect& area) const override { return false; }
    bool SetSoundID(int which, int soundId) override { return false; }
};

struct TextEditWindow final : gz::cGZWinFlatRect {
    explicit TextEditWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeTextEdit edit;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinTextEdit) { *out = &edit; return true; }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

struct FakeSpinner final : gz::cIGZWinSpinner {
    int type = -1; std::map<std::uint32_t, bool> flags;
    int value = -99, low = -99, high = -99, step = -99, fontSets = 0, imageSets = 0, align = -1, gx = -1, gy = -1;
    gz::IWinFont* font = reinterpret_cast<gz::IWinFont*>(1);
    std::vector<std::pair<int, std::uint32_t>> colors;
    std::uint32_t outline[4] = {7, 7, 7, 7}; int outlineCalls = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool SetSpinnerType(tSpinnerType t) override { type = int(t); return true; }
    bool SetSpinnerFlag(std::uint32_t f, bool on) override { flags[f] = on; return true; }
    bool SetValue(int v) override { value = v; return true; }
    bool SetMinMax(int l, int h) override { low = l; high = h; return true; }
    bool SetStepSize(int s) override { step = s; return true; }
    bool SetImage(cIGZBuffer*, const cRZRect*) override { ++imageSets; return true; }
    bool SetFont(gz::IWinFont* f) override { ++fontSets; font = f; return true; }
    bool SetFontColor(tFontColorIdx i, std::uint32_t c) override { colors.emplace_back(int(i), c); return true; }
    bool SetFontColor(tFontColorIdx, gz::cRZColor) override { return false; }
    bool SetTextAlignment(std::uint32_t a) override { align = int(a); return true; }
    bool SetGutters(std::uint8_t x, std::uint8_t y) override { gx = x; gy = y; return true; }
    bool SetOutlineColors(std::uint32_t l, std::uint32_t t, std::uint32_t r, std::uint32_t b) override { outline[0] = l; outline[1] = t; outline[2] = r; outline[3] = b; ++outlineCalls; return true; }
    cIGZWin* AsIGZWin() override { return nullptr; }
    bool GetSpinnerType(tSpinnerType& type) const override { return false; }
    bool GetSpinnerFlag(std::uint32_t flag) const override { return false; }
    int GetValue() const override { return 0; }
    bool GetMinMax(int& low, int& high) const override { return false; }
    bool GetStepSize(int& step) const override { return false; }
    bool SetButtonImage(cIGZBuffer* image) override { return false; }
    bool GetButtonImage(cIGZBuffer*& image) override { return false; }
    bool SetButtonImageArea(const cRZRect& area) override { return false; }
    bool GetButtonImageArea(cRZRect& area) override { return false; }
    bool SetFontStyle(std::uint32_t styleId) override { return false; }
    bool GetFontStyle(std::uint32_t& styleId) override { return false; }
    bool GetFontColor(tFontColorIdx index, std::uint32_t& native) const override { return false; }
    bool GetFontColor(tFontColorIdx index, gz::cRZColor& color) const override { return false; }
    bool ClearFontColor(tFontColorIdx index) override { return false; }
    bool GetTextAlignment(std::uint32_t& alignment) const override { return false; }
    bool GetGutters(std::uint8_t& x, std::uint8_t& y) const override { return false; }
    bool GetOutlineColors(std::uint32_t& left, std::uint32_t& top, std::uint32_t& right, std::uint32_t& bottom) const override { return false; }
    bool SetOutlineColor(tOutlineIndex index, const gz::cRZColor& color) override { return false; }
    bool GetOutlineColor(tOutlineIndex index, gz::cRZColor& color) const override { return false; }

};

struct SpinnerWindow final : gz::cGZWinFlatRect {
    explicit SpinnerWindow(cIGZWinMgr* mgr) : cGZWinFlatRect(mgr) {}
    FakeSpinner spin;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinSpinner) { *out = &spin; return true; }
        return gz::cGZWinFlatRect::QueryInterface(iid, out);
    }
};

struct FakeLegacyBinding final : cIGZLegacyControl {
    cIGZWin* control = nullptr;
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs > 1 ? --refs : 0; }
    cIGZWin* GetControl() override { return control; }
    void SetOwnControl(bool) override {}
    void ForceControl(cIGZWin*) override {}
    void UseWinProc(gz::cIGZWinProc*) override {}
    bool GetScriptFragment(cIGZString**) override { return false; }
};

struct FakeScriptableParent final : cIGZScriptableControl {
    FakeLegacyBinding binding;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0x9a4527cb) {
            binding.AddRef();
            *out = static_cast<cIGZLegacyControl*>(&binding);
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool SetParameters(tScriptPropertyArray&) override { return false; }
    IVocabulary* GetControlVocabulary() override { return nullptr; }
    std::uint32_t GetID() override { return 0; }
    void SetID(std::uint32_t) override {}
    void SetOwnControl(bool) override {}
    bool ImplementInterface(cIGZScriptableControlSet*, std::uint32_t, std::uint32_t, cIGZVariant*) override { return false; }
    bool SetParent(cIGZScriptableControl*) override { return false; }
    bool Render(cIGZGDriver*) override { return true; }
    bool SetArea(cRZRect&) override { return true; }
    bool GetArea(cRZRect&) override { return true; }
    bool ControlEvent(cRZPoint&, std::uint32_t) override { return true; }
    cIGZWin* GetControl() override { return nullptr; }
    void ForceControl(cIGZWin*) override {}
    void UseWinProc(gz::cIGZWinProc*) override {}
    bool GetScriptFragment(cIGZString**) override { return false; }
};

struct FakeHost final : IScriptControlHost {
    FakeMgr* mgr = nullptr;
    cIGZWin* mainWindow = nullptr;
    std::map<std::uint32_t, cIGZWin* (*)(FakeHost*)> factories;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> createCalls;

    cIGZWin* CreateControl(std::uint32_t clsid, std::uint32_t iid) override {
        createCalls.emplace_back(clsid, iid);
        auto it = factories.find(clsid);
        if (it == factories.end()) return nullptr;
        cIGZWin* control = it->second(this);
        if (control) control->AddRef();   // GZCOM hands out a counted reference
        return control;
    }
    cIGZWin* GetMainWindow() override { return mainWindow; }
    bool UnscriptWindow(cIGZWin*, cIGZString& out) override {
        out.AssignFromCString("fragment 42");
        return true;
    }
    bool LoadScriptString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "res-%08x-%08x", group, instance);
        out.AssignFromCString(buffer);
        return true;
    }
    cIGZBuffer* LoadScriptBuffer(std::uint32_t, std::uint32_t) override { return nullptr; }
    void GetNativeResolution(int index, int& width, int& height) override {
        static const int kResolutions[4][2] = {{800, 600}, {1024, 768}, {1280, 1024}, {1920, 1080}};
        if (index < 0 || index >= 4) index = 0;
        width = kResolutions[index][0];
        height = kResolutions[index][1];
    }
};

cIGZWin* MakeFlatGen(FakeHost* host) {
    auto* window = new FlatGenWindow(host->mgr);
    host->mgr->valid.insert(window);
    return window;
}

cIGZWin* MakeFlat(FakeHost* host) { return MakeFlatGen(host); }

// Build a property list; the variants must outlive the list.
struct Params {
    tScriptPropertyArray list;
    std::vector<cRZVariant*> owned;

    void Uint(std::uint32_t id, std::uint32_t value) {
        owned.push_back(new cRZVariant(value));
        list.push_back({id, owned.back()});
    }
    void Text(std::uint32_t id, const char* value) {
        owned.push_back(new cRZVariant(cRZString(value)));
        list.push_back({id, owned.back()});
    }
    ~Params() {
        for (cRZVariant* variant : owned) {
            variant->Release();
        }
    }
};

}  // namespace

int main() {
    // -----------------------------------------------------------------------------------------------------
    // 1. The vocabulary bootstrap: the constructor creates spLocalVocab once and registers the 21 tables.
    // -----------------------------------------------------------------------------------------------------
    FakeMgr mgr;
    FakeHost host;
    host.mgr = &mgr;
    auto* mainWindow = new cGZWin(&mgr);
    mainWindow->AddRef();
    mgr.valid.insert(mainWindow);
    host.mainWindow = mainWindow;

    FakeVocabularyServices vocabServices;
    SetVocabularyServices(&vocabServices);
    SetScriptControlHost(&host);

    auto* primary = new cGZLegacyControl;   // creates the vocabulary and registers everything
    CHECK(vocabServices.creates == 1);
    CHECK(vocabServices.addRefs == 0);
    CHECK(vocabServices.vocab != nullptr);
    CHECK(primary->GetControlVocabulary() == vocabServices.vocab);

    FakeVocabulary* vocab = vocabServices.vocab;
    // 368 registrations from the 21 tables, one of which ("center", registered by GZWinCommon as 0xf008) is
    // rejected by the case-insensitive vocabulary when GZWinText registers its own "center" (4) later.
    CHECK_EQ(vocab->rejected, 1);
    CHECK_EQ(vocab->entries.size(), std::size_t{367});
    CHECK_EQ(vocab->Duplicates(), 0);
    CHECK(vocab->Has("center", 0xf008u));
    CHECK(vocab->Has("clsid", 0xffffffffu));
    CHECK(vocab->Has("iid", 0xfffffffeu));
    CHECK(vocab->Has("caption", 0x105u));
    CHECK(vocab->Has("transparent", 0x107u));
    CHECK(vocab->Has("captionres", 0x106u));
    CHECK(vocab->Has("winflag_visible", 0xf01au));
    CHECK(vocab->Has("winflag_alphablend", 0xf026u));
    CHECK(vocab->Has("GZWinBtn", 0x22ecfc47u));
    CHECK(vocab->Has("IGZWinBtn", 0x8810u));
    CHECK(vocab->Has("GZWinFlatRect", 0xc2afa76eu));
    CHECK(vocab->Has("IGZWinFlatRect", 0xc2afa76fu));
    CHECK(vocab->Has("tiled", 0u));
    CHECK(vocab->Has("edge", 2u));
    CHECK(vocab->Has("GZWinGen", 0x4386d516u));
    CHECK(vocab->Has("IGZWinGen", 0x5386d516u));
    CHECK(vocab->Has("speed", 0x1200u));
    CHECK(vocab->Has("font", 0xf000u));
    CHECK(vocab->Has("combodownarrowrect", 0x801u));
    CHECK(vocab->Has("colorleft", 0x200u));

    // The two dispatch tables of SetParameters.
    CHECK_EQ(cGZLegacyControl::Tables().clsidToIid.size(), std::size_t{20});
    CHECK_EQ(cGZLegacyControl::Tables().interpret.size(), std::size_t{20});
    CHECK_EQ(cGZLegacyControl::Tables().clsidToIid[0xc2afa76eu], 0xc2afa76fu);
    CHECK_EQ(cGZLegacyControl::Tables().clsidToIid[0x4386d516u], 0x5386d516u);
    CHECK(cGZLegacyControl::Tables().interpret[0xc2afa76fu] == &cGZLegacyControl::InterpretPropertiesGZWinFlatRect);
    CHECK(cGZLegacyControl::Tables().interpret[0x8810u] == &cGZLegacyControl::InterpretPropertiesGZWinBtn);

    // A second object only takes another reference; the tables are not registered twice.
    auto* secondary = new cGZLegacyControl;
    CHECK_EQ(vocabServices.creates, 1);
    CHECK_EQ(vocabServices.addRefs, 1);
    CHECK_EQ(vocabServices.vocab, vocab);
    CHECK_EQ(vocab->entries.size(), std::size_t{367});
    secondary->Release();
    CHECK_EQ(vocabServices.releases, 1);
    CHECK_EQ(vocabServices.vocab, vocab);   // still referenced by `primary`

    // -----------------------------------------------------------------------------------------------------
    // 2. The three vtables: QueryInterface, the shared reference count and the object offsets.
    // -----------------------------------------------------------------------------------------------------
    auto* adapter = new cGZLegacyControl;
    CHECK_EQ(vocabServices.addRefs, 2);
    cIGZScriptableControl* scriptable = nullptr;
    cIGZControl* controlView = nullptr;
    cIGZLegacyControl* legacyView = nullptr;
    void* unknown = nullptr;

    CHECK(adapter->QueryInterface(1, &unknown));
    CHECK_EQ(reinterpret_cast<char*>(unknown), reinterpret_cast<char*>(adapter));
    CHECK(adapter->QueryInterface(0x1a356083u, reinterpret_cast<void**>(&scriptable)));
    CHECK_EQ(reinterpret_cast<char*>(scriptable), reinterpret_cast<char*>(adapter));
    CHECK(adapter->QueryInterface(0x5a356159u, reinterpret_cast<void**>(&controlView)));
    CHECK_EQ(reinterpret_cast<char*>(controlView), reinterpret_cast<char*>(adapter) + 8);   // second vptr
    CHECK(adapter->QueryInterface(0x9a4527cbu, reinterpret_cast<void**>(&legacyView)));
    CHECK_EQ(reinterpret_cast<char*>(legacyView), reinterpret_cast<char*>(adapter) + 0x10);  // third vptr
    CHECK(!adapter->QueryInterface(0xdeadbeefu, &unknown));

    // One shared counter: AddRef through one view is visible through the other.
    CHECK_EQ(legacyView->AddRef(), 5u);
    CHECK_EQ(controlView->AddRef(), 6u);
    CHECK_EQ(scriptable->Release(), 5u);
    CHECK_EQ(legacyView->Release(), 4u);
    CHECK_EQ(controlView->Release(), 3u);
    // Drop the four QueryInterface references, leaving the one that owns the object for this section.
    CHECK_EQ(scriptable->Release(), 2u);
    CHECK_EQ(legacyView->Release(), 1u);
    // A control-less adapter: GetID is 0 and SetID is a no-op.
    CHECK_EQ(adapter->GetID(), 0u);
    adapter->SetID(0x1234u);
    CHECK_EQ(adapter->GetID(), 0u);
    cRZRect emptyArea{};
    CHECK(!adapter->SetArea(emptyArea));
    CHECK(!adapter->GetArea(emptyArea));
    CHECK(adapter->Render(nullptr));
    cRZPoint point{};
    CHECK(adapter->ControlEvent(point, 1));

    // -----------------------------------------------------------------------------------------------------
    // 3. ForceControl / SetID / SetArea / GetArea / GetScriptFragment over a real flat rectangle.
    // -----------------------------------------------------------------------------------------------------
    auto* flat = MakeFlat(&host);   // AddRef'd by the factory helper? no: MakeFlat returns a count-0 object
    flat->AddRef();                 // the caller's reference
    adapter->ForceControl(flat);
    flat->SetID(0x77u);
    CHECK_EQ(adapter->GetID(), 0x77u);
    adapter->SetID(0x78u);
    CHECK_EQ(flat->GetID(), 0x78u);

    cRZRect area{1, 2, 30, 40};
    CHECK(adapter->SetArea(area));
    CHECK_EQ(flat->GetL(), 1);
    CHECK_EQ(flat->GetT(), 2);
    CHECK_EQ(flat->GetR(), 30);
    CHECK_EQ(flat->GetB(), 40);
    cRZRect readback;
    CHECK(adapter->GetArea(readback));
    CHECK_EQ(readback.right, 30);

    cIGZString* fragment = nullptr;
    CHECK(adapter->GetScriptFragment(&fragment));
    CHECK(fragment != nullptr);
    CHECK(std::string(fragment->ToChar()) == "fragment 42");
    fragment->Release();

    // UseWinProc takes a reference and drops the old one; the destructor releases the last one.
    auto* proc = new FakeProc;
    CHECK_EQ(proc->refs, 1u);
    adapter->UseWinProc(proc);
    CHECK_EQ(proc->refs, 2u);
    adapter->UseWinProc(nullptr);
    CHECK_EQ(proc->refs, 1u);
    CHECK_EQ(proc->released, 1);
    CHECK_EQ(proc->refs, 1u);

    adapter->ForceControl(nullptr);   // releases the flat; we still hold a reference
    CHECK_EQ(adapter->GetID(), 0u);
    adapter->Release();
    CHECK_EQ(vocabServices.releases, 2);
    delete proc;
    flat->Release();   // the caller's reference, the adapter dropped its own

    // -----------------------------------------------------------------------------------------------------
    // 4. SetParameters: control creation, GZCOM fallbacks, the common properties and the flat-rect ones.
    // -----------------------------------------------------------------------------------------------------
    host.factories[0xc2afa76eu] = &MakeFlatGen;
    host.factories[0x478d1e6fu] = &MakeFlatGen;
    host.createCalls.clear();

    auto* built = new cGZLegacyControl;
    Params params;
    params.Uint(0xffffffffu, 0xc2afa76eu);            // clsid: GZWinFlatRect
    params.Uint(0x100u, 0x1234u);                     // id
    params.Text(0x102u, "(5,6)");                     // pos
    params.Text(0x103u, "(100,40)");                  // size
    params.Text(0x104u, "(10,20,30)");                // fillcolor
    params.Text(0x105u, "hello");                     // caption
    params.Uint(0xf005u, 0x206u);                     // style: all edges
    params.Uint(0xf005u, 0xf00bu);                    // style: sunken (both entries are visited)
    params.Text(0x200u, "(1,2,3)");                   // colorleft
    params.Text(0x201u, "(4,5,6)");                   // colortop
    params.Text(0x202u, "(7,8,9)");                   // colorright
    params.Text(0x203u, "(10,11,12)");                // colorbottom
    params.Uint(0xf01b, 1u);                          // winflag_enabled
    params.Uint(0x107u, 1u);                          // transparent

    CHECK(!built->SetParameters(params.list));             // quirk: the function always reports failure
    CHECK_EQ(host.createCalls.size(), std::size_t{1});
    CHECK_EQ(host.createCalls[0].first, 0xc2afa76eu);
    CHECK_EQ(host.createCalls[0].second, 0x22ba0121u);   // the fixed cIGZWin iid
    cIGZWin* controlWindow = built->GetControl();
    CHECK(controlWindow != nullptr);
    auto* flatControl = static_cast<gz::cGZWinFlatRect*>(controlWindow);

    CHECK_EQ(built->GetID(), 0x1234u);
    CHECK_EQ(flatControl->GetL(), 5);
    CHECK_EQ(flatControl->GetT(), 6);
    CHECK_EQ(flatControl->GetW(), 100);
    CHECK_EQ(flatControl->GetH(), 40);
    std::uint8_t r = 0, g = 0, b = 0;
    flatControl->GetFillColor(r, g, b);
    CHECK_EQ(r, 10); CHECK_EQ(g, 20); CHECK_EQ(b, 30);
    CHECK(std::string(flatControl->GetCaption().ToChar()) == "hello");
    CHECK(flatControl->GetFlag(0x20000u));             // transparent
    CHECK(flatControl->GetFlag(0x2u));                 // winflag_enabled (re-applied after Init)
    CHECK_EQ(flatControl->GetOutlineFlags(), std::uint8_t{0x2f});   // 0x0f | 0x20
    flatControl->GetOutlineColor(0, r, g, b);
    CHECK_EQ(r, 1); CHECK_EQ(g, 2); CHECK_EQ(b, 3);
    flatControl->GetOutlineColor(3, r, g, b);
    CHECK_EQ(r, 10); CHECK_EQ(g, 11); CHECK_EQ(b, 12);

    // The class and view ids were stored as parameters (0xe85eebde / 0x8d6e462).
    cIGZVariant* param = nullptr;
    CHECK(flatControl->GetParam(0xe85eebdeu, &param));
    CHECK_EQ(param->GetValUint32(), 0xc2afa76eu);
    param->Release();
    CHECK(flatControl->GetParam(0x8d6e462u, &param));
    CHECK_EQ(param->GetValUint32(), 0xc2afa76fu);
    param->Release();

    // The control was parented into the main window.
    cIGZWin* parent = flatControl->GetParentWin();
    CHECK_EQ(parent, mainWindow);
    CHECK_EQ(mainWindow->GetChildCount(), 1u);

    built->Release();   // ownControl: removes the child, shuts it down and releases it
    CHECK_EQ(mainWindow->GetChildCount(), 0u);

    // Fallback 1: unknown class, no iid -> the default cGZWinCustom class (0x478d1e6f).
    host.createCalls.clear();
    auto* fallback = new cGZLegacyControl;
    Params unknownClass;
    unknownClass.Uint(0xffffffffu, 0xdeadbeefu);
    CHECK(!fallback->SetParameters(unknownClass.list));
    CHECK_EQ(host.createCalls.size(), std::size_t{2});
    CHECK_EQ(host.createCalls[0].first, 0xdeadbeefu);
    CHECK_EQ(host.createCalls[1].first, 0x478d1e6fu);
    CHECK(fallback->GetControl() != nullptr);
    fallback->Release();

    // Fallback 2: unknown class with a known view iid -> the class registered under that iid.
    host.createCalls.clear();
    auto* byIid = new cGZLegacyControl;
    Params byIidParams;
    byIidParams.Uint(0xffffffffu, 0xdeadbeefu);
    byIidParams.Uint(0xfffffffeu, 0xc2afa76fu);
    CHECK(!byIid->SetParameters(byIidParams.list));
    CHECK_EQ(host.createCalls.size(), std::size_t{2});
    CHECK_EQ(host.createCalls[0].first, 0xdeadbeefu);
    CHECK_EQ(host.createCalls[1].first, 0xc2afa76eu);
    byIid->Release();

    // A missing/zero clsid does nothing.
    host.createCalls.clear();
    auto* noClass = new cGZLegacyControl;
    Params noClassParams;
    noClassParams.Uint(0xffffffffu, 0u);
    CHECK(!noClass->SetParameters(noClassParams.list));
    CHECK_EQ(host.createCalls.size(), std::size_t{0});
    CHECK(noClass->GetControl() == nullptr);
    noClass->Release();

    // -----------------------------------------------------------------------------------------------------
    // 5. SetParent: the control is added to the scriptable parent's window instead of the main window.
    // -----------------------------------------------------------------------------------------------------
    auto* parentWindow = new cGZWin(&mgr);
    parentWindow->AddRef();
    mgr.valid.insert(parentWindow);
    FakeScriptableParent scriptableParent;
    scriptableParent.binding.control = parentWindow;
    scriptableParent.binding.refs = 1;

    auto* child = new cGZLegacyControl;
    CHECK(child->SetParent(&scriptableParent));
    CHECK_EQ(scriptableParent.binding.refs, 1u);   // the binding reference taken by SetParent is dropped
    Params parentParams;
    parentParams.Uint(0xffffffffu, 0xc2afa76eu);
    CHECK(!child->SetParameters(parentParams.list));
    cIGZWin* childWindow = child->GetControl();
    CHECK(childWindow != nullptr);
    CHECK_EQ(childWindow->GetParentWin(), parentWindow);
    CHECK_EQ(parentWindow->GetChildCount(), 1u);
    CHECK_EQ(mainWindow->GetChildCount(), 0u);
    child->Release();
    CHECK_EQ(parentWindow->GetChildCount(), 0u);
    parentWindow->Release();

    // SetParent(nullptr) drops the stored parent window again.
    auto* parentWindow2 = new cGZWin(&mgr);
    parentWindow2->AddRef();
    mgr.valid.insert(parentWindow2);
    scriptableParent.binding.control = parentWindow2;
    auto* detach = new cGZLegacyControl;
    CHECK(detach->SetParent(&scriptableParent));   // takes the second reference to parentWindow2
    CHECK(detach->SetParent(nullptr));             // and drops it again
    Params detachParams;
    detachParams.Uint(0xffffffffu, 0xc2afa76eu);
    CHECK(!detach->SetParameters(detachParams.list));
    CHECK_EQ(detach->GetControl()->GetParentWin(), mainWindow);
    detach->Release();
    parentWindow2->Release();

    // -----------------------------------------------------------------------------------------------------
    // 6. DoExceptions through SetParameters: the generic window is handed the window procedure and the
    //    adapter drops its own reference to it.
    // -----------------------------------------------------------------------------------------------------
    host.factories[0x4386d516u] = &MakeFlatGen;
    auto* genProc = new FakeProc;
    auto* withProc = new cGZLegacyControl;
    withProc->UseWinProc(genProc);
    CHECK_EQ(genProc->refs, 2u);
    Params genParams;
    genParams.Uint(0xffffffffu, 0x4386d516u);   // SC3WinGen -> IGZWinGen 0x5386d516
    CHECK(!withProc->SetParameters(genParams.list));
    auto* genWindow = static_cast<FlatGenWindow*>(withProc->GetControl());
    CHECK(genWindow != nullptr);
    CHECK(genWindow->gen.proc == genProc);
    CHECK_EQ(genProc->refs, 1u);   // the adapter released its reference
    CHECK_EQ(genProc->released, 1);
    genWindow->Init();
    withProc->Release();           // the generic window dies with the control
    delete genProc;

    // -----------------------------------------------------------------------------------------------------
    // 7. The common interpreter on its own (including captionres and the native resolution resources).
    // -----------------------------------------------------------------------------------------------------
    auto* direct = new gz::cGZWinFlatRect(&mgr);
    direct->AddRef();
    Params common;
    common.Uint(0xf01a, 0u);                 // winflag_visible = false
    common.Uint(0xf01d, 1u);                 // winflag_sizeable
    common.Text(0x101u, "(2,3,4,5)");        // area as a string wins over pos/size
    common.Text(0x102u, "(9,9)");
    common.Text(0x106u, "{12345678,9abcdef0}");   // captionres
    common.Uint(0x107u, 1u);
    cGZLegacyControl::InterpretPropertiesGZWinCommon(direct, common.list, true);
    CHECK_EQ(direct->GetL(), 2);
    CHECK_EQ(direct->GetR(), 4);
    CHECK(!direct->GetFlag(0x1u));
    CHECK(direct->GetFlag(0x200u));
    CHECK(std::string(direct->GetCaption().ToChar()) == "res-12345678-9abcdef0");
    CHECK(direct->GetFlag(0x20000u));
    cIGZVariant* keyParam = nullptr;
    CHECK(direct->GetParam(0xb31771au, &keyParam));
    CHECK_EQ(keyParam->GetType(), std::uint16_t{0x8000});   // 12-byte blob
    keyParam->Release();
    cIGZVariant* resParam = nullptr;
    CHECK(direct->GetParam(0x3aca6906u, &resParam));
    CHECK_EQ(resParam->GetCount(), 8u);
    resParam->Release();

    // Resolution group 0xa554ae8 / instance 0xca7e17d8 -> "1920x1080"? index 0 in the fake.
    Params resolution;
    resolution.Text(0x106u, "{a554ae8,ca7e17d8}");
    cGZLegacyControl::InterpretPropertiesGZWinCommon(direct, resolution.list, true);
    CHECK(std::string(direct->GetCaption().ToChar()) == "800x600");

    // The post-init pass only re-applies the five late flags.
    Params late;
    late.Uint(0xf01b, 0u);
    late.Uint(0xf01f, 0u);
    late.Uint(0xf020, 0u);
    late.Uint(0xf021, 0u);
    late.Uint(0xf022, 0u);
    cGZLegacyControl::InterpretPropertiesGZWinCommon(direct, late.list, false);
    CHECK(!direct->GetFlag(0x2u));
    CHECK(!direct->GetFlag(0x10000u));
    CHECK(!direct->GetFlag(0x20000u));
    CHECK(!direct->GetFlag(0x40000u));
    CHECK(!direct->GetFlag(0x100000u));
    direct->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
    direct->Release();

    // The flat-rect interpreter needs a flat window; pos/size are ignored because "area" is present.
    CHECK_EQ(cGZLegacyControl::Tables().interpret[0xc2afa76fu] != nullptr, true);

    // The outline interpreter: style -> flags, colours and the polygon nodes.
    auto* outlineWindow = new OutlineWindow(&mgr);
    outlineWindow->AddRef();
    mgr.valid.insert(outlineWindow);
    Params outlineParams;
    outlineParams.Uint(0xf005u, 0xf00bu);           // sunken
    outlineParams.Uint(0xf005u, 0xf00du);           // filled
    outlineParams.Text(0x1100u, "(1,2,3)");
    outlineParams.Text(0x1101u, "(4,5,6)");
    outlineParams.Text(0x1102u, "(10,20)");
    outlineParams.Text(0x1102u, "(30,40)");
    cGZLegacyControl::InterpretPropertiesGZWinOutline(outlineWindow, outlineParams.list, true);
    CHECK_EQ(outlineWindow->outline.flags, 0x22u);  // 0x02 | 0x20
    CHECK_EQ(outlineWindow->outline.color[1][0], 1);
    CHECK_EQ(outlineWindow->outline.color[2][2], 6);
    CHECK_EQ(outlineWindow->outline.polyCount, 2);
    CHECK_EQ(outlineWindow->outline.poly[1].x, 30);
    CHECK_EQ(outlineWindow->outline.poly[1].y, 40);
    CHECK_EQ(outlineWindow->outline.releases, 1);
    cGZLegacyControl::InterpretPropertiesGZWinOutline(outlineWindow, outlineParams.list, false);   // no-op
    CHECK_EQ(outlineWindow->outline.releases, 2);
    outlineWindow->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
    outlineWindow->Release();

    // The folder strip interpreter: the init pass records the font name and the outline image, the later pass replaces
    // the folders with the `listelement` strings (other properties and non-string values are skipped).
    {
        auto* folders = new gz::cGZWinFolders(&mgr);
        folders->AddRef();
        folders->AddFolder(cRZString("old"), 5);
        Params initParams;
        initParams.Text(0xf000u, "main9");
        initParams.Text(0xf016u, "{11223344,55667788}");
        initParams.Text(0x502u, "ignored at init");
        cGZLegacyControl::InterpretPropertiesGZWinFolders(folders, initParams.list, true);
        std::uint32_t count = 0;
        folders->GetCount(count);
        CHECK_EQ(count, 1u);                          // the init pass leaves the folders alone
        cIGZVariant* folderParam = nullptr;
        CHECK(folders->GetParam(0xfaa3be85u, &folderParam));
        if (folderParam) folderParam->Release();
        folderParam = nullptr;
        CHECK(folders->GetParam(0xdaa3b860u, &folderParam));
        if (folderParam) { CHECK_EQ(folderParam->GetCount(), 8u); folderParam->Release(); }
        Params listParams;
        listParams.Text(0x502u, "Globals");
        listParams.Uint(0x502u, 3u);                  // not a string: skipped
        listParams.Text(0x503u, "other");
        listParams.Text(0x502u, "Locals");
        cGZLegacyControl::InterpretPropertiesGZWinFolders(folders, listParams.list, false);
        folders->GetCount(count);
        CHECK_EQ(count, 2u);
        cRZString folderName;
        CHECK(folders->GetFolderByIndex(0, folderName) && std::string(folderName.ToChar()) == "Globals");
        CHECK(folders->GetFolderByIndex(1, folderName) && std::string(folderName.ToChar()) == "Locals");
        cGZLegacyControl::InterpretPropertiesGZWinFolders(nullptr, listParams.list, false);   // no window: nothing
        folders->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
        folders->Release();
    }

    // The slider interpreter: min/max, direction, image resource and the post-init initial value.
    auto* sliderWindow = new SliderWindow(&mgr);
    sliderWindow->AddRef();
    mgr.valid.insert(sliderWindow);
    Params sliderParams;
    sliderParams.Text(0xc00u, "(20,5)");          // reversed: max then min
    sliderParams.Uint(0xc01u, 0xc03u);            // vertical
    sliderParams.Text(0xf016u, "{11223344,55667788}");
    sliderParams.Uint(0x40au, 7u);
    cGZLegacyControl::InterpretPropertiesGZWinSlider(sliderWindow, sliderParams.list, true);
    CHECK_EQ(sliderWindow->slider.maximum, 20);
    CHECK_EQ(sliderWindow->slider.minimum, 5);
    CHECK_EQ(sliderWindow->slider.direction, gz::kSliderVertical);
    CHECK_EQ(sliderWindow->slider.value, 0);      // only the post-init pass sets it
    CHECK(sliderWindow->GetParam(0xdaa3b860u, &keyParam));
    CHECK_EQ(keyParam->GetCount(), 8u);
    keyParam->Release();
    CHECK(sliderWindow->GetParam(0x6b4bcbd4u, &keyParam));
    CHECK_EQ(keyParam->GetCount(), 12u);
    keyParam->Release();
    cGZLegacyControl::InterpretPropertiesGZWinSlider(sliderWindow, sliderParams.list, false);
    CHECK_EQ(sliderWindow->slider.value, 7);
    CHECK_EQ(sliderWindow->slider.releases, 2);
    sliderWindow->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
    sliderWindow->Release();

    // The scrollbar interpreter: min/max, orientation, page/line sizes, line/page count and the initial value.
    auto* barWindow = new ScrollbarWindow(&mgr);
    barWindow->AddRef();
    mgr.valid.insert(barWindow);
    Params barParams;
    barParams.Text(0xc00u, "(9,3)");              // reversed: max 9, min 3
    barParams.Uint(0xc01u, 0xc03u);               // vertical
    barParams.Uint(0xd00u, 4u);                   // pagesize
    barParams.Uint(0xd01u, 2u);                   // linesize
    barParams.Text(0xd02u, "(5,6)");              // line/page count
    barParams.Uint(0x40au, 3u);
    cGZLegacyControl::InterpretPropertiesGZWinScrollbar(barWindow, barParams.list, true);
    CHECK_EQ(barWindow->bar.minimum, 3);
    CHECK_EQ(barWindow->bar.maximum, 9);
    CHECK_EQ(barWindow->bar.orientation, 2);
    CHECK_EQ(barWindow->bar.pageSize, 4);
    CHECK_EQ(barWindow->bar.lineSize, 2);
    CHECK_EQ(barWindow->bar.lines, 5u);
    CHECK_EQ(barWindow->bar.pages, 6u);
    CHECK_EQ(barWindow->bar.value, 0);
    cGZLegacyControl::InterpretPropertiesGZWinScrollbar(barWindow, barParams.list, false);
    CHECK_EQ(barWindow->bar.value, 3);
    CHECK_EQ(barWindow->bar.releases, 2);
    barWindow->Release();

    // The bitmap interpreter: alpha, the flag word, the image resource and the post-init image rectangle.
    auto* bmpWindow = new BmpWindow(&mgr);
    bmpWindow->AddRef();
    mgr.valid.insert(bmpWindow);
    Params bmpParams;
    bmpParams.Uint(0xb00u, 300u);                 // clamped to 255
    bmpParams.Uint(0xf003u, 1u);                  // flag 1
    bmpParams.Uint(0xb01u, 1u);                   // flag 2
    bmpParams.Uint(0xb02u, 0u);                   // flag 8 stays off
    bmpParams.Text(0xf016u, "{11223344,55667788}");
    cGZLegacyControl::InterpretPropertiesGZWinBMP(bmpWindow, bmpParams.list, true);
    CHECK(bmpWindow->bmp.alphaEnabled);
    CHECK_EQ(bmpWindow->bmp.alpha, 1.0f);
    CHECK_EQ(bmpWindow->bmp.flags, 3);            // 1 | 2
    CHECK(bmpWindow->GetParam(0xdaa3b860u, &keyParam));
    CHECK_EQ(keyParam->GetCount(), 8u);
    keyParam->Release();
    CHECK(bmpWindow->GetParam(0xab428c5du, &keyParam));
    CHECK_EQ(keyParam->GetCount(), 12u);
    keyParam->Release();
    Params bmpRect;
    bmpRect.Text(0xf018u, "(1,2,30,40)");
    cGZLegacyControl::InterpretPropertiesGZWinBMP(bmpWindow, bmpRect.list, false);
    CHECK(bmpWindow->bmp.hasImageArea);
    CHECK_EQ(bmpWindow->bmp.imageArea.right, 30);
    CHECK_EQ(bmpWindow->bmp.flags, 3 | 0x10);
    CHECK_EQ(bmpWindow->bmp.releases, 2);
    bmpWindow->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
    bmpWindow->Release();

    // The generic-window interpreter (SC3WinGen): flag keywords (title bar / fill / outline / paint are inverted),
    // user data, gutters, the image resource params, the dummy window procedure; then the divider / bluebar strings.
    {
        auto* genWindow = new Sc3GenWindow(&mgr);
        genWindow->AddRef();
        mgr.valid.insert(genWindow);
        Params genParams;
        genParams.Uint(0x10004u, 1u);     // moveable -> flag 1 on
        genParams.Uint(0x10005u, 0u);     // sizeable -> flag 2 off
        genParams.Uint(0x1000au, 1u);     // closedisabled -> 0x100 on
        genParams.Uint(0x1000du, 0u);     // titlebar=no -> flag 0x1000 ON (inverted)
        genParams.Uint(0xf00du, 1u);      // fill=yes -> flag 0x2000 off
        genParams.Uint(0xf019u, 0u);      // outline=no -> flag 0x4000 on
        genParams.Uint(0x1000eu, 1u);     // paint=yes -> flag 0x8000 off
        genParams.Uint(0x1000fu, 1u);     // sidebar -> 0x10000 on
        genParams.Uint(0x10000u, 77u);    // userdata
        genParams.Text(0xf004u, "(64,32)");   // gutters
        genParams.Text(0xf016u, "{46a006b0,ea7f0eae}");
        cGZLegacyControl::InterpretPropertiesSC3WinGen(genWindow, genParams.list, true);
        const auto& g = genWindow->gen;
        CHECK(g.GetWinGenFlag(1) && !g.GetWinGenFlag(2) && g.GetWinGenFlag(0x100));
        CHECK(g.GetWinGenFlag(0x1000) && !g.GetWinGenFlag(0x2000) && g.GetWinGenFlag(0x4000) && !g.GetWinGenFlag(0x8000) && g.GetWinGenFlag(0x10000));
        CHECK(g.flags.count(4) == 0);        // keywords that were not written are untouched
        CHECK(g.userDataSet && g.userData == 77u);
        CHECK(g.gutterX == 64 && g.gutterY == 32);
        CHECK(g.proc != nullptr);                                    // the dummy window procedure
        void* procOut = nullptr;
        CHECK(g.proc->QueryInterface(0x22e85d8e, &procOut));
        g.proc->Release();
        CHECK(g.bitmapCalls == 0);                                   // the fake host loads no image
        cIGZVariant* genKey = nullptr;
        CHECK(genWindow->GetParam(0xdaa3b860u, &genKey));
        genKey->Release();
        CHECK(genWindow->GetParam(0xab428c5du, &genKey));
        genKey->Release();
        CHECK_EQ(g.releases, 1);                                     // the interface reference the query took
        Params secondPass;
        secondPass.Text(0x10002u, "(7,120)");                 // divider (id, y)
        secondPass.Text(0x10003u, "(9,1,2,300,40)");          // bluebar (id, l, t, r, b)
        secondPass.Text(0x10002u, "(bad)");                   // malformed: ignored
        cGZLegacyControl::InterpretPropertiesSC3WinGen(genWindow, secondPass.list, false);
        CHECK(genWindow->gen.dividers.size() == 1 && genWindow->gen.dividers[0].first == 7 && genWindow->gen.dividers[0].second == 120);
        CHECK(genWindow->gen.bars.size() == 1 && genWindow->gen.bars[0].id == 9 && genWindow->gen.bars[0].area.right == 300 && genWindow->gen.bars[0].area.bottom == 40);
        CHECK(genWindow->gen.bars[0].sizeMode == 0 && genWindow->gen.bars[0].outline == 0);
        CHECK_EQ(genWindow->gen.releases, 2);
        genWindow->gen.proc->Release();   // the reference the fake took in SetWinProc
        genWindow->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
        genWindow->Release();
    }

    // The text interpreter: three flag properties, the font style, the two colours (through the graphics system), the
    // gutters and offsets, the autofit parameter; then the second pass' alignment (centre -> 4, others pass through).
    {
        RgbGraphics rgb;
        SetWinGraphics(&rgb);
        auto* textWindow = new TextWindow(&mgr);
        textWindow->AddRef();
        mgr.valid.insert(textWindow);
        Params textParams;
        textParams.Uint(0xf003u, 1u);
        textParams.Uint(0x300u, 0u);
        textParams.Uint(0x301u, 0u);                 // flag 4 off, and the window's transparent-private-buffer flag on
        textParams.Uint(0xf000u, 12u);               // font style id
        textParams.Text(0xf002u, "(255,204,0)");
        textParams.Text(0xf001u, "(1,2,3)");
        textParams.Text(0xf004u, "(2,3)");
        textParams.Text(0x305u, "(-1,4)");
        cGZLegacyControl::InterpretPropertiesGZWinText(textWindow, textParams.list, true);
        const auto& t = textWindow->text;
        CHECK(t.flags.at(1) && !t.flags.at(2) && !t.flags.at(4));
        CHECK(textWindow->GetFlag(kWinPrivateBufferTransparent));
        CHECK(t.style == 12u);
        CHECK(t.colors.size() == 2 && t.colors[0].first == 0 && t.colors[0].second == 0xffffcc00u && t.colors[1].first == 1 && t.colors[1].second == 0xff010203u);
        CHECK(t.gutterX == 2 && t.gutterY == 3 && t.offX == -1 && t.offY == 4);
        CHECK(t.fits == 0);
        cIGZVariant* autofitParam = nullptr;
        bool autofitValue = true;
        CHECK(textWindow->GetParam(0xdacf8f9cu, &autofitParam));
        autofitParam->GetValBool(autofitValue);
        CHECK(!autofitValue);
        autofitParam->Release();
        // second pass with autofit (0x304) and a centre alignment
        Params second;
        second.Uint(0xf015u, 0xf008u);
        second.Uint(0x304u, 1u);
        cGZLegacyControl::InterpretPropertiesGZWinText(textWindow, second.list, false);
        CHECK(t.alignment == 4u && t.fits == 1);
        CHECK(textWindow->GetParam(0xdacf8f9cu, &autofitParam));
        autofitParam->GetValBool(autofitValue);
        CHECK(autofitValue);
        autofitParam->Release();
        Params right;
        right.Uint(0xf015u, 0xf007u);                // not centre: the keyword value itself is passed (kept quirk)
        cGZLegacyControl::InterpretPropertiesGZWinText(textWindow, right.list, false);
        CHECK(t.alignment == 0xf007u);
        // no gutter property: (0,0)
        Params none;
        cGZLegacyControl::InterpretPropertiesGZWinText(textWindow, none.list, true);
        CHECK(t.gutterX == 0 && t.gutterY == 0);
        textWindow->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
        textWindow->Release();
        SetWinGraphics(nullptr);
    }

    // The button interpreter: plain and inverted flags, the style keyword table, the default "main9" font, colours,
    // gutters (2 or 4 numbers), caption alignment, tip text / offsets / placement, sound ids and their params.
    {
        RgbGraphics rgb;
        SetWinGraphics(&rgb);
        auto* btnWindow = new BtnWindow(&mgr);
        btnWindow->AddRef();
        mgr.valid.insert(btnWindow);
        Params p;
        p.Uint(0xa00u, 1u); p.Uint(0xa01u, 0u); p.Uint(0xf00du, 0u);     // flag 1 on, 2 off, 0x10 ON (inverted)
        p.Uint(0xa07u, 1u); p.Uint(0xa08u, 0u);                         // 0x200 off, 0x400 on (inverted)
        p.Uint(0xf005u, 0xa0bu);                                        // style keyword -> 2
        p.Uint(0xf000u, 77u);                                           // font style id
        p.Text(0xf00fu, "(1,2,3)"); p.Text(0xf012u, "(9,8,7)");
        p.Text(0xf004u, "(4,5)");
        p.Text(0xf015u, "x");                                           // wrong type for the keyword: ignored
        p.Uint(0xf015u, 0xf007u);                                       // right -> caption style 2
        p.Text(0xa10u, "Hello tip");
        p.Text(0xa11u, "(3,-4)");
        p.Text(0xa19u, "{11,22}");
        cGZLegacyControl::InterpretPropertiesGZWinBtn(btnWindow, p.list, true);
        const auto& b = btnWindow->btn;
        CHECK(b.flags.at(1) && !b.flags.at(2) && b.flags.at(0x10) && !b.flags.at(0x200) && b.flags.at(0x400));
        CHECK(b.style == 2 && b.styleCalls == 1 && !b.styleHasImage);
        CHECK(b.fontStyle == 77u && b.fontSets == 0);
        CHECK(b.colors.size() == 2 && b.colors[0].first == 0 && b.colors[0].second == 0xff010203u && b.colors[1].first == 1 && b.colors[1].second == 0xff090807u);
        CHECK(b.g1x == 4 && b.g1y == 5 && b.g4[0] == -1);
        CHECK(b.captionStyle == 2);
        CHECK(b.tip == "Hello tip" && b.tipX == 3 && b.tipY == -4 && b.tipType == 0x1000000);
        CHECK(b.sounds[0] == 0x22u && b.sounds[1] == 0u);
        cIGZVariant* soundParam = nullptr;
        CHECK(btnWindow->GetParam(0xa5be39du, &soundParam)); soundParam->Release();
        CHECK(!btnWindow->GetParam(0x4a5be2f8u, &soundParam));
        // four gutters, style keyword 0xa00 -> 3, no font -> the "main9" lookup through the host, explicit tip type
        Params q;
        q.Text(0xf004u, "(1,2,3,4)");
        q.Uint(0xf005u, 0xa00u);
        q.Uint(0xa18u, 5u);
        q.Text(0xa1au, "{aa,bb}");
        cGZLegacyControl::InterpretPropertiesGZWinBtn(btnWindow, q.list, true);
        CHECK(b.g4[0] == 1 && b.g4[1] == 2 && b.g4[2] == 3 && b.g4[3] == 4);
        CHECK(b.style == 3 && b.tipType == 5 && b.sounds[1] == 0xbbu && b.sounds[0] == 0u);
        CHECK(b.fontSets == 1 && b.font == nullptr);                   // no button host: SetFont(null), as the original passes the lookup result on
        // second pass: only flag 4
        Params second; second.Uint(0xa02u, 1u);
        cGZLegacyControl::InterpretPropertiesGZWinBtn(btnWindow, second.list, false);
        CHECK(b.flags.at(4));
        btnWindow->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
        btnWindow->Release();
        SetWinGraphics(nullptr);
    }

    // The edit control interpreter: the flag table (1 sets, anything else clears), font, colours, caret / highlight /
    // outline colours, limits, gutters, insertion state and the autofit second pass.
    {
        RgbGraphics rgb;
        SetWinGraphics(&rgb);
        auto* editWindow = new TextEditWindow(&mgr);
        editWindow->AddRef();
        mgr.valid.insert(editWindow);
        Params p;
        p.Uint(0x400u, 0u);          // clears bit 0x1 (default word 0x10b has it)
        p.Uint(0x300u, 1u);          // sets 0x2 (already set)
        p.Uint(0x401u, 1u);          // sets 0x4
        p.Uint(0xf019u, 1u);         // sets 0x10
        p.Uint(0x417u, 1u);          // sets 0x8000
        p.Uint(0x416u, 2u);          // value 2 is not 1: clears 0x800 (not set anyway)
        p.Uint(0xf000u, 21u);
        p.Text(0xf00fu, "(1,2,3)"); p.Text(0xf014u, "(7,8,9)");
        p.Text(0x407u, "(10,20,30)"); p.Text(0x408u, "(40,50,60)"); p.Text(0x40bu, "(1,1,1)");
        p.Uint(0x409u, 255u); p.Text(0xf004u, "(3,4)"); p.Uint(0x40cu, 0u); p.Uint(0x40du, 5u);
        p.Text(0x40eu, "(2,6)"); p.Uint(0x40fu, 500u); p.Uint(0x410u, 20u);
        cGZLegacyControl::InterpretPropertiesGZWinTextEdit(editWindow, p.list, true);
        const auto& e = editWindow->edit;
        CHECK(e.setFlagsCalls == 1 && e.flags == ((0x10bu & ~0x1u) | 0x4u | 0x10u | 0x8000u));
        CHECK(e.fontStyle == 21u);
        CHECK(e.fontColors.size() == 2 && e.fontColors[0].first == int(gz::cIGZWinTextEdit::kColorText) && e.fontColors[0].second == 0xff010203u);
        CHECK(e.fontColors[1].first == int(gz::cIGZWinTextEdit::kColorSelectedBack) && e.fontColors[1].second == 0xff070809u);
        CHECK(e.caret == 0xff0a141eu && e.highlight == 0xff28323cu);
        CHECK(e.outlineOn && e.outlineColor == 0xff010101u);          // the outline switch is the flag keyword 0xf019 (1)
        CHECK(e.maxText == 255u && e.gutterX == 3u && e.gutterY == 4u && e.insertMode && e.insertIndex == 5u);
        CHECK(e.line == 2u && e.column == 6u && e.caretPeriod == 500u && e.undo == 20u);
        CHECK(e.vertImages == 0 && e.horzImages == 0);                // the fake host loads no images
        cIGZVariant* keyParam2 = nullptr;
        CHECK(editWindow->GetParam(0xdacf8f9cu, &keyParam2) == false);   // the autofit param belongs to the second pass
        Params second;
        second.Uint(0x304u, 1u);
        cGZLegacyControl::InterpretPropertiesGZWinTextEdit(editWindow, second.list, false);
        CHECK(e.resizes == 1);
        CHECK(editWindow->GetParam(0xdacf8f9cu, &keyParam2)); keyParam2->Release();
        Params none;
        cGZLegacyControl::InterpretPropertiesGZWinTextEdit(editWindow, none.list, false);
        CHECK(e.resizes == 1);
        editWindow->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
        editWindow->Release();
        SetWinGraphics(nullptr);
    }

    // The spinner interpreter: placement keyword, flags, fonts (default "main9" goes through the font source), colours,
    // alignment, gutters, the outline colour fallback rule, and the second pass' min/max, step and value.
    {
        RgbGraphics rgb;
        SetWinGraphics(&rgb);
        auto* spinWindow = new SpinnerWindow(&mgr);
        spinWindow->AddRef();
        mgr.valid.insert(spinWindow);
        Params p;
        p.Uint(0x1000u, 0xf007u);                    // buttons left -> 2
        p.Uint(0xf019u, 1u); p.Uint(0x1002u, 0u);    // flag 1 on, 0x2000 off
        p.Text(0xf00fu, "(1,2,3)"); p.Text(0xf011u, "(4,5,6)");
        p.Uint(0xf015u, 0xf008u);                    // centre -> 1
        p.Text(0xf004u, "(2,3)");
        p.Text(0x1006u, "(9,9,9)"); p.Text(0x1008u, "(1,1,1)"); p.Text(0x100au, "(2,2,2)");
        cGZLegacyControl::InterpretPropertiesGZWinSpinner(spinWindow, p.list, true);
        const auto& sp = spinWindow->spin;
        CHECK(sp.type == 2 && sp.flags.at(1) && !sp.flags.at(0x2000));
        CHECK(sp.fontSets == 1 && sp.font == nullptr);               // default font: no font source installed
        CHECK(sp.colors.size() == 2 && sp.colors[0].first == 0 && sp.colors[1].first == 1);   // hover is index 1
        CHECK(sp.align == 1 && sp.gx == 2 && sp.gy == 3);
        CHECK(sp.outlineCalls == 1 && sp.outline[0] == 0xff090909u && sp.outline[1] == 0xff010101u && sp.outline[2] == 0xff090909u && sp.outline[3] == 0xff020202u);
        // no outline property at all: SetOutlineColors is not called
        Params none;
        cGZLegacyControl::InterpretPropertiesGZWinSpinner(spinWindow, none.list, true);
        CHECK(sp.outlineCalls == 1);
        // only the first one: the four sides are all that colour
        Params base; base.Text(0x1006u, "(5,6,7)");
        cGZLegacyControl::InterpretPropertiesGZWinSpinner(spinWindow, base.list, true);
        CHECK(sp.outlineCalls == 2 && sp.outline[0] == 0xff050607u && sp.outline[3] == 0xff050607u);
        Params second;
        second.Text(0x1004u, "(2,40)"); second.Uint(0x1005u, 5u); second.Uint(0x40au, 12u);
        cGZLegacyControl::InterpretPropertiesGZWinSpinner(spinWindow, second.list, false);
        CHECK(sp.low == 2 && sp.high == 40 && sp.step == 5 && sp.value == 12);
        spinWindow->Release();
        SetWinGraphics(nullptr);
    }

    // -----------------------------------------------------------------------------------------------------
    // 8. The property helpers.
    // -----------------------------------------------------------------------------------------------------
    Params helperParams;
    helperParams.Uint(0x10u, 42u);
    helperParams.Text(0x11u, "(3,4)");
    helperParams.Text(0x12u, "(7,8,9)");
    std::uint32_t number = 0;
    CHECK(GetPropertyUint32(helperParams.list, 0x10u, number));
    CHECK_EQ(number, 42u);
    CHECK(!GetPropertyUint32(helperParams.list, 0x11u, number));   // wrong type
    CHECK(!GetPropertyUint32(helperParams.list, 0x13u, number));   // missing
    cRZString text;
    CHECK(GetPropertyString(helperParams.list, 0x11u, text));
    CHECK(std::string(text.ToChar()) == "(3,4)");
    CHECK(!GetPropertyString(helperParams.list, 0x10u, text));     // wrong type
    CHECK(std::string(text.ToChar()).empty());   // cleared first
    int x = 0, y = 0;
    CHECK(GetPropertyCoords(helperParams.list, 0x11u, x, y));
    CHECK_EQ(x, 3); CHECK_EQ(y, 4);
    std::uint8_t cr = 0, cg = 0, cb = 0;
    CHECK(GetPropertyColor(helperParams.list, 0x12u, cr, cg, cb));
    CHECK_EQ(cr, 7); CHECK_EQ(cg, 8); CHECK_EQ(cb, 9);
    CHECK(!GetPropertyColor(helperParams.list, 0x11u, cr, cg, cb));   // only two components
    cRZRect rect;
    CHECK(!GetPropertyRect(helperParams.list, 0x11u, rect));          // only two components
    Params rectParams;
    rectParams.Text(0x20u, "(1,2,30,40)");
    CHECK(GetPropertyRect(rectParams.list, 0x20u, rect));
    CHECK_EQ(rect.left, 1); CHECK_EQ(rect.top, 2); CHECK_EQ(rect.right, 30); CHECK_EQ(rect.bottom, 40);
    int sint = 0;
    CHECK(GetPropertySint32(rectParams.list, 0x20u, sint) == false);  // wrong type
    Params sintParams;
    sintParams.Uint(0x21u, 0xfffffff9u);          // -7 stored as uint32, read through AsSint32
    CHECK(GetPropertySint32(sintParams.list, 0x21u, sint));
    CHECK_EQ(sint, -7);

    // SetGZWinFont / SetResourceKeyParam post variants on the window.
    auto* helperWindow = new cGZWin(&mgr);
    helperWindow->AddRef();
    SetGZWinFont(helperWindow, 0x68963c4cu, 0);
    cIGZVariant* fontParam = nullptr;
    CHECK(helperWindow->GetParam(0xfaa3be85u, &fontParam));
    CHECK_EQ(fontParam->GetValUint32(), 0x68963c4cu);
    fontParam->Release();
    SetGZWinFont(helperWindow, cRZString("Arial"), 1);
    CHECK(helperWindow->GetParam(0xfaa3be86u, &fontParam));
    cRZString fontName;
    CHECK(fontParam->GetValString(fontName));
    CHECK(std::string(fontName.ToChar()) == "Arial");
    fontParam->Release();
    SetResourceKeyParam(helperWindow, 0x11223344u, cGZPersistResourceKey{1, 2, 3});
    CHECK(helperWindow->GetParam(0x11223344u, &fontParam));
    CHECK_EQ(fontParam->GetCount(), 12u);
    fontParam->Release();
    SetGZWinFont(nullptr, 1u, 0);          // null-safe
    SetGZWinFont(nullptr, cRZString("x"), 0);
    SetResourceKeyParam(nullptr, 1u, cGZPersistResourceKey{});
    helperWindow->Init();   // Shutdown (which drops the window's parameters) only runs on an initialized window
    helperWindow->Release();

    // -----------------------------------------------------------------------------------------------------
    // 9. Take the primary object down and check the vocabulary reference bookkeeping.
    // -----------------------------------------------------------------------------------------------------
    primary->Release();
    CHECK_EQ(vocabServices.addRefs, 9);     // one per extra cGZLegacyControl
    CHECK_EQ(vocabServices.releases, 10);   // one per cGZLegacyControl, including the creating one
    CHECK(vocabServices.vocab == nullptr);  // last reference: the cache was cleared

    SetVocabularyServices(nullptr);
    SetScriptControlHost(nullptr);
    mainWindow->Release();

    if (gFailures == 0) {
        std::printf("gz_legacycontrol: all checks passed\n");
        return 0;
    }
    std::printf("gz_legacycontrol: %d check(s) failed\n", gFailures);
    return 1;
}

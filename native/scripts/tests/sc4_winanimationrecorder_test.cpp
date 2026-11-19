// cSC4WinAnimationRecorder tests: the constructor defaults and the application-ini round trip, Init's window tree (labels,
// edits, buttons, output-type group, recording outline, the GX-directory walk and the location-saver restore), GZPaint on
// a private buffer, the three mouse handlers (including the drag-anchor quirk), the recording pipeline (frame bitmaps,
// the colour-buffer copy, the animation-tick pacing and the millisecond-timer pacing), the Gimex write-out with the three
// file name formats, Stop's Yes/No box, the frame-buffer-full path with loop mode off and Shutdown's releases.
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "SDL.h"                 // the test stub: timers never fire (and can be made to fail to create)
#include "fake_winmgr.h"
#include "gz/Buffer.h"
#include "gz/Font.h"
#include "gz/WinBtn.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinMsgBox.h"
#include "gz/WinOptGrp.h"
#include "gz/WinText.h"
#include "gz/WinTextEdit.h"
#include "sc4/WinAnimationRecorder.h"
#include "sc4/WinLocationSaver.h"
#include "sc4/WinResizableOutline.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const long long va = (a), vb = (b); if (va != vb) { \
    std::printf("FAIL line %d: %s == %s (%lld vs %lld)\n", __LINE__, #a, #b, va, vb); ++failures; } } while (0)
#define CHECK_STR(a, b) do { const std::string va = (a), vb = (b); if (va != vb) { \
    std::printf("FAIL line %d: %s == %s (\"%s\" vs \"%s\")\n", __LINE__, #a, #b, va.c_str(), vb.c_str()); ++failures; } } while (0)
#define CHECK_RECT(r, l, t, rr, b) do { if ((r).left != (l) || (r).top != (t) || (r).right != (rr) || (r).bottom != (b)) { \
    std::printf("FAIL line %d: %s = {%d,%d,%d,%d}\n", __LINE__, #r, (r).left, (r).top, (r).right, (r).bottom); ++failures; } } while (0)

using namespace sc4;

namespace {

std::string g_root;

bool MakeDirectory(const std::string& path) { return mkdir(path.c_str(), 0777) == 0; }
bool MakeFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    std::fclose(file);
    return true;
}
bool PathExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}
std::string ReadFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return std::string();
    std::string text;
    char buffer[4096];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof buffer, file)) != 0) text.append(buffer, got);
    std::fclose(file);
    return text;
}

// ---- the font the host hands out (one reference per CreateFont call) ------------------------------------------------
struct FakeFont final : gz::IWinFont {
    int refs = 0;
    int created = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t DefaultTextColor() override { return 0xff102030; }
    std::uint32_t DefaultShadowColor() override { return 0xff405060; }
    std::int16_t LineHeight(int, int) override { return 10; }
    int CountLines(const char*, std::uint32_t, int) override { return 1; }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 0; }
    void MeasureText(const char*, std::uint32_t length, cRZRect& out) override { out = {0, 0, int(length) * 5, 10}; }
    int TextWidth(const char*, std::uint32_t length) override { return int(length) * 5; }
    void DrawText(IWinBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
    void DrawTextBox(IWinBuffer*, const cRZRect&, const char*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                     const cRZRect*, int) override {}
};

// ---- the font source the composed controls look fonts up through (WinFontSource) --------------------------------
struct FakeFontSource final : gz::IWinFontSource {
    FakeFont* font = nullptr;
    gz::IWinFont* Font(std::uint32_t) override { return font; }
    bool Available() override { return font != nullptr; }
};

// ---- the window's own drawing buffer (cGZWin's private buffer, reached through IWinGraphics) -------------------------
struct FakeDrawBuffer final : IWinBuffer {
    int refs = 1;
    cRZRect bounds{0, 0, 0, 0};
    std::vector<std::uint32_t> locks, unlocks;
    std::uint32_t fillColor = 0;
    cRZRect fillRect{};
    int fills = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    const cRZRect* GetBounds() const override { return &bounds; }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    void NativeToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = (n >> 16) & 0xff; g = (n >> 8) & 0xff; b = n & 0xff;
    }
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return true; }
    bool Init(int w, int h) override { bounds = {0, 0, w, h}; return true; }
    void Shutdown() override {}
    bool Lock(std::uint32_t flags) override { locks.push_back(flags); return true; }
    void Unlock(std::uint32_t flags) override { unlocks.push_back(flags); }
    bool Fill(std::uint32_t color, const cRZRect& rect) override {
        fillColor = color;
        fillRect = rect;
        ++fills;
        return true;
    }
};

struct FakeGraphics final : IWinGraphics {
    FakeDrawBuffer* buffer = nullptr;
    int created = 0;
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    void NativeToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = (n >> 16) & 0xff; g = (n >> 8) & 0xff; b = n & 0xff;
    }
    IWinBuffer* PrimaryBuffer() override { return nullptr; }
    bool DrawSystemActive() override { return false; }
    IWinBuffer* GraphicsCreateBuffer() override { ++created; return buffer; }
};

// ---- a frame bitmap (cIGZBuffer) the window recorder creates through the host's CreateBuffer -------------------------
struct FakeFrameBuffer final : cIGZBuffer {
    int refs = 1;
    int inits = 0;
    std::uint32_t width = 0, height = 0;
    cGZBufferColorType colorType{};
    cRZRect area{};
    bool initResult = true;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init(std::uint32_t w, std::uint32_t h, cGZBufferColorType type) override {
        ++inits; width = w; height = h; colorType = type; area = {0, 0, int(w), int(h)};
        return initResult;
    }
    bool Shutdown() override { return true; }
    bool Uninitialize() override { return true; }
    bool Lock(eLockFlags) override { return false; }
    bool Unlock(eLockFlags) override { return false; }
    bool IsLocked() const override { return false; }
    std::uint32_t Width() const override { return width; }
    std::uint32_t Height() const override { return height; }
    const cRZRect* GetBufferArea() const override { return &area; }
    void GetBufferArea(cRZRect& out) const override { out = area; }
    std::uint32_t GetMemoryUsage() const override { return 0; }
    cGZBufferColorType GetColorType() const override { return colorType; }
    void GetColorType(cGZBufferColorType& out) const override { out = colorType; }
    std::uint32_t GetBitsPerPixel() const override { return 32; }
    std::uint32_t GetBytesPerPixel() const override { return 4; }
    bool Fill(std::uint32_t, const cRZRect&, const cRZRect*) override { return true; }
    bool Fill(std::uint32_t, int, int, int, int, const cRZRect*) override { return true; }
    bool Fill(std::uint32_t, const cRZRect*) override { return true; }
    std::uint32_t GetPixel(std::uint32_t, std::uint32_t) const override { return 0; }
    void SetPixel(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void SetTransparency(std::uint32_t) override {}
    bool GetTransparency() const override { return false; }
    bool IsPixelTransparent(std::uint32_t, std::uint32_t) override { return false; }
    bool GetTransparentColor(std::uint32_t&) const override { return false; }
    void RemoveTransparency() override {}
    bool DrawLine(int x0, int y0, int x1, int y1, int color, std::uint32_t thickness) override {
        lines.push_back({x0, y0, x1, y1, color, static_cast<int>(thickness)});
        return true;
    }
    bool Blt(cIGZBuffer*, const cRZRect&, const cRZRect&, const cRZRect*) override { return false; }
    std::uint32_t ConvertRGBValueToNative(std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    bool ConvertNativeValueToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override { return false; }
    std::uint32_t ConvertRGBAValueToNative(std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    bool ConvertNativeValueToRGBA(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&, std::uint8_t&) override { return false; }
    void* GetColorSurfaceBits() override { return nullptr; }
    std::uint32_t GetColorSurfaceStride() override { return 0; }
    std::uint32_t GetChangeCounter() override { return 0; }
    cIGZBufferHardwareCache* GetHardwareCache() override { return nullptr; }
    bool SetHardwareCache(cIGZBufferHardwareCache*) override { return false; }
    bool IsReady() override { return true; }
    struct Line { int x0, y0, x1, y1, color, thickness; };
    std::vector<Line> lines;
};

// ---- the colour-buffer extension of the GZ driver (iid 0xe69bfe2a) --------------------------------------------------
struct FakeColorBuffer final : IWinAnimationColorBuffer {
    int refs = 1;
    int copies = 0;
    int x = 0, y = 0, w = 0, h = 0;
    cIGZBuffer* destination = nullptr;
    bool result = true;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZBuffer* CopyColorBuffer(int px, int py, int pw, int ph, cIGZBuffer* target) override {
        ++copies; x = px; y = py; w = pw; h = ph; destination = target;
        return result ? target : nullptr;
    }
};

// ---- the animation tick manager (service 0x86ad10ec) ---------------------------------------------------------------
struct FakeTick final : IWinAnimationTick {
    std::vector<int> paused;
    std::vector<std::uint32_t> deltas;
    int steps = 0;
    void PauseAnimationSystem(bool on) override { paused.push_back(on ? 1 : 0); }
    void SetStepTickTimeDelta(std::uint32_t delta) override { deltas.push_back(delta); }
    void StepAnimationSystem() override { ++steps; }
};

// ---- the host seam -------------------------------------------------------------------------------------------------
struct FakeHost final : IWinAnimationRecorderHost {
    std::string iniPath;        // empty: no application
    std::string appDirectory;
    bool appPresent = true;
    FakeFont font;
    FakeGraphics* graphics = nullptr;
    gz::cGZWinCtrlMgr* wcm = nullptr;
    FakeColorBuffer* colorBuffer = nullptr;
    FakeTick* tick = nullptr;
    cSLWinLocationSaver* saver = nullptr;
    cIGZWin* view = nullptr;
    cIGZBuffer* drawTarget = nullptr;
    std::vector<FakeFrameBuffer*> created;
    bool createBuffers = true;
    bool failSecondInit = false;
    // recorded calls
    std::vector<std::string> fontsCreated;
    struct Saved { std::string path; std::uint32_t flags; };
    std::vector<Saved> saved;
    bool saveResult = true;

    bool AppIniFilePath(cRZString& out) override {
        if (!appPresent) return false;
        out.AssignFromCString(iniPath.c_str());
        return true;
    }
    bool AppDirectory(cRZString& out) override {
        out.AssignFromCString(appDirectory.c_str());
        return true;
    }
    gz::cIGZWinCtrlMgr* WinCtrlMgr() override { return wcm; }
    gz::IWinFont* CreateFont(const char* face, int type, int size, int style, bool reuse) override {
        fontsCreated.push_back(std::string(face) + "/" + std::to_string(type) + "/" + std::to_string(size) + "/" +
                               std::to_string(style) + "/" + (reuse ? "1" : "0"));
        font.AddRef();
        ++font.created;
        return &font;
    }
    cIGZBuffer* CreateBuffer() override {
        if (!createBuffers) return nullptr;
        auto* buffer = new FakeFrameBuffer;
        if (failSecondInit && created.size() == 1) buffer->initResult = false;
        created.push_back(buffer);
        return buffer;
    }
    bool SaveImage(const cIGZString& path, cIGZBuffer* buffer, std::uint32_t flags) override {
        saved.push_back({path.Data(), flags});
        (void)buffer;
        return saveResult;
    }
    IWinAnimationColorBuffer* ColorBuffer() override {
        if (!colorBuffer) return nullptr;
        colorBuffer->AddRef();
        return colorBuffer;
    }
    IWinAnimationTick* AnimationTick() override { return tick; }
    cSLWinLocationSaver* WinLocationSaver() override { return saver; }
    cIGZWin* View3DWindow() override { return view; }
    cIGZBuffer* WindowBuffer(IWinBuffer*) override { return drawTarget; }
};

// A plain window used as the 3D view (the outline's parent) and as the recorder's parent window.
struct FakeWin final : cGZWin {
    using cGZWin::cGZWin;
    std::vector<cGZMessage> got;
    bool DoMessage(cGZMessage& message) override {
        got.push_back(message);
        return true;
    }
};

struct Fixture {
    FakeMgr mgr;
    gz::cGZWinCtrlMgr wcm{&mgr};
    FakeGraphics graphics;
    FakeDrawBuffer drawBuffer;
    FakeWin view{&mgr};
    cSLWinLocationSaver saver;
    FakeColorBuffer colorBuffer;
    FakeTick tick;
    FakeHost host;
    FakeFrameBuffer drawTarget;
    std::vector<FakeFrameBuffer*> created;   // owned by FakeHost::created

    FakeFontSource fontSource;

    Fixture() {
        graphics.buffer = &drawBuffer;
        host.graphics = &graphics;
        host.wcm = &wcm;
        host.colorBuffer = &colorBuffer;
        host.tick = &tick;
        host.saver = &saver;
        host.view = &view;
        host.drawTarget = &drawTarget;
        colorBuffer.refs = 0;                          // the fixture holds no reference of its own
        fontSource.font = &host.font;
        gz::SetWinFontSource(&fontSource);
        gz::SetWinCtrlFactory(&wcm);
        host.iniPath = g_root + "/prefs.ini";
        host.appDirectory = g_root + "/App/Sub/";
        SetWinGraphics(&graphics);
        SetWinAnimationRecorderHost(&host);
    }
    ~Fixture() {
        for (FakeFrameBuffer* frame : host.created) delete frame;   // Release never deletes a fake buffer
        SetWinAnimationRecorderHost(nullptr);
        SetWinGraphics(nullptr);
        gz::SetWinFontSource(nullptr);
        gz::SetWinCtrlFactory(nullptr);
    }
    cSC4WinAnimationRecorder* Make() {
        auto* recorder = new cSC4WinAnimationRecorder(&mgr);
        recorder->AddRef();
        return recorder;
    }
    std::vector<FakeFrameBuffer*>& frames() { return host.created; }
};

std::string Caption(cIGZWin* window) { return std::string(window->GetCaption().Data()); }

}  // namespace

int main() {
    g_root = "/tmp/sc4_war_test_" + std::to_string(getpid());
    MakeDirectory(g_root);
    MakeDirectory(g_root + "/App");
    MakeDirectory(g_root + "/App/Sub");
    MakeDirectory(g_root + "/Bin");
    MakeDirectory(g_root + "/Bin/GX");
    MakeFile(g_root + "/Bin/GX/gx.exe");
    MakeDirectory(g_root + "/out4");
    MakeDirectory(g_root + "/App/Bin");
    MakeDirectory(g_root + "/App/Bin/GX");
    MakeFile(g_root + "/App/Bin/GX/gx.exe");
    // the edits' caret timers would hold a self-reference (and fire after deletion) if they ever started
    g_timerCreationFails = true;
    g_ticks = 1000;                       // cRZTimer counts from a non-zero tick (the SDL stub starts at 0)

    // ---- A: the constructor's defaults, the GX-directory walk and Start without a colour-buffer extension ----------
    char startDirectory[1024] = {};
    CHECK(getcwd(startDirectory, sizeof startDirectory) != nullptr);
    {
        Fixture f;
        f.host.colorBuffer = nullptr;                 // no GZ driver extension: Start must refuse to record
        CHECK(chdir(g_root.c_str()) == 0);            // the GX walk builds a relative path (see ANIMREC-4)
        cSC4WinAnimationRecorder* recorder = f.Make();
        CHECK_EQ(recorder->GetID(), 0u);                              // the ctor does not set the id (Init does)
        CHECK_STR(recorder->Name().Data(), "Animation");
        CHECK_STR(recorder->Rate().Data(), "100");
        CHECK_STR(recorder->Time().Data(), "5");
        CHECK_STR(recorder->PreferencesPath().Data(), f.host.iniPath);
        CHECK_EQ(recorder->Take(), 0);
        CHECK_EQ(recorder->FrameCount(), 0);
        CHECK_EQ(recorder->OutputType(), kAnimationRecordJPG);
        CHECK(recorder->LoopRecording());
        CHECK(recorder->DeleteBitmapsWhenFinished());
        CHECK_EQ(recorder->Timer().GetUnits(), cRZTimer::kMilliseconds);
        CHECK_EQ(recorder->GetChildCount(), 0u);

        CHECK(recorder->Init());
        CHECK_EQ(recorder->GetID(), 0x6acb1af1u);
        CHECK(recorder->GetFlag(kWinPrivateBuffer));
        CHECK_RECT(recorder->GetArea(), 4, 4, 0x120, 0x67);
        CHECK_EQ(recorder->FillColor(), 0xff000000u);                  // MakeFillColor(0, 0, 0)
        CHECK_EQ(recorder->GetChildCount(), 12u);                      // 4 labels + status + 3 edits + 3 buttons + output group
        CHECK_EQ(f.host.fontsCreated.size(), 1u);
        CHECK_STR(f.host.fontsCreated[0], "Arial MT/0/10/0/1");
        CHECK_EQ(f.host.font.created, 1);
        CHECK(f.host.font.refs >= 5);                                  // five labels hold one reference each
        CHECK_EQ(f.view.GetChildCount(), 1u);                          // the recording outline lives in the 3D view
        CHECK(recorder->Outline() != nullptr);

        // the GX directory walk: SplitDirectory hands back the second-to-last *component* as parts[1], so the candidate
        // is the relative path "App/Bin/GX/" (ANIMREC-4); it is tested from the application directory's parent.
        CHECK_STR(recorder->GXDirectory().Data(), "App/Bin/GX/");
        CHECK(recorder->GXExecutableExists());

        // the status line and the edits
        CHECK_STR(Caption(recorder->StatusText()), "Status: Stopped.\nFrame count: 0");
        CHECK_STR(Caption(recorder->NameEdit()), "Animation");
        CHECK_STR(Caption(recorder->RateEdit()), "100");
        CHECK_STR(Caption(recorder->TimeEdit()), "5");
        CHECK(recorder->NameEdit()->IsFlagSet(gz::kTESingleLine));
        CHECK(!recorder->NameEdit()->IsFlagSet(gz::kTEWrap));
        CHECK_RECT(recorder->NameEdit()->GetArea(), 0x39, 6, 0xb7, 0x16);
        CHECK_RECT(recorder->RateEdit()->GetArea(), 0x39, 0x1b, 0x67, 0x2c);
        CHECK_RECT(recorder->TimeEdit()->GetArea(), 0x92, 0x1b, 0xb7, 0x2c);

        // the buttons
        CHECK(recorder->StartButton() != nullptr);
        CHECK_EQ(recorder->StartButton()->AsIGZWin()->GetID(), kAnimationRecordButtonStart);
        CHECK_STR(Caption(recorder->StartButton()->AsIGZWin()), "Start");
        CHECK(!recorder->StartButton()->GetBtnFlag(gz::kBtnFitToContent));
        CHECK_RECT(recorder->StartButton()->AsIGZWin()->GetArea(), 0x60, 0x46, 0x98, 0x5c);
        CHECK_EQ(recorder->CloseButton()->AsIGZWin()->GetID(), kAnimationRecordButtonClose);
        CHECK_STR(Caption(recorder->CloseButton()->AsIGZWin()), "Close");
        CHECK_RECT(recorder->CloseButton()->AsIGZWin()->GetArea(), 0x9f, 0x46, 0xd7, 0x5c);
        CHECK_EQ(recorder->SpanButton()->AsIGZWin()->GetID(), kAnimationRecordButtonSpan);
        CHECK_STR(Caption(recorder->SpanButton()->AsIGZWin()), "Span");
        CHECK(!recorder->SpanButton()->AsIGZWin()->GetFlag(kWinEnabled));   // disabled while idle
        std::uint32_t spanFontColor = 0;
        CHECK(recorder->SpanButton()->GetFontColor(4, spanFontColor));
        CHECK_EQ(spanFontColor, 0xff808080u);
        CHECK_RECT(recorder->SpanButton()->AsIGZWin()->GetArea(), 0xde, 0x46, 0x116, 0x5c);

        // the output-type group
        CHECK(recorder->OutputTypeGroup() != nullptr);
        CHECK_EQ(recorder->OutputTypeGroup()->GetValue(), 3u);
        CHECK(!recorder->OutputTypeGroup()->GetOptGrpFlag(1));
        CHECK(recorder->OutputTypeGroup()->GetOptGrpFlag(2));
        CHECK(!recorder->OutputTypeGroup()->GetOptGrpFlag(4));
        CHECK(!recorder->OutputTypeGroup()->GetOptGrpFlag(8));
        CHECK_EQ(recorder->OutputTypeGroup()->AsIGZWin()->GetL(), 0x35);
        CHECK_EQ(recorder->OutputTypeGroup()->AsIGZWin()->GetT(), 0x2c);
        CHECK(recorder->OutputTypeGroup()->AsIGZWin()->GetW() > 0);

        // the outline: id, area, colours and flags
        CHECK_EQ(recorder->Outline()->GetID(), kAnimationRecordOutlineId);
        CHECK_RECT(recorder->Outline()->GetArea(), 100, 100, 0x106, 0xde);
        CHECK(recorder->Outline()->GetFlag(kWinPrivateBuffer));
        CHECK(!recorder->Outline()->GetFlag(kWinIgnoreCursor));
        CHECK(recorder->Outline()->Resizing());
        CHECK(recorder->Outline()->Moving());

        // a second Init is a no-op
        CHECK(recorder->Init());
        CHECK_EQ(recorder->GetChildCount(), 12u);
        CHECK_EQ(f.host.fontsCreated.size(), 1u);

        // Start without the driver extension: the message box and false
        CHECK(!recorder->Start());
        CHECK_EQ(f.mgr.msgBoxes.size(), 1u);
        CHECK_STR(f.mgr.msgBoxes[0].text, "Screencapture cIGZGDriver extension not present.");
        CHECK_STR(f.mgr.msgBoxes[0].title, "Animation Recorder");
        CHECK_EQ(f.mgr.msgBoxes[0].style, 0u);

        // GZPaint fills and frames the draw area on the private buffer
        CHECK(recorder->GZPaint());
        CHECK_EQ(f.drawBuffer.fills, 1);
        CHECK_EQ(f.drawBuffer.fillColor, 0xff000000u);
        CHECK_RECT(f.drawBuffer.fillRect, 0, 0, 0x11c, 0x63);
        CHECK_EQ(f.drawBuffer.locks.size(), 2u);
        CHECK_EQ(f.drawBuffer.locks[0], 0x8001u);
        CHECK_EQ(f.drawBuffer.locks[1], 0x8008u);
        CHECK_EQ(f.drawBuffer.unlocks.size(), 2u);
        CHECK_EQ(f.drawTarget.lines.size(), 4u);                       // DrawRectangleOutline's four edges
        CHECK_EQ(f.drawTarget.lines[0].color, static_cast<int>(0xff000000u));

        // the mouse handlers, including the anchor quirk
        recorder->SetArea(10, 20, 110, 80);
        CHECK(recorder->GZOnMouseDownL(50, 40, 0));
        CHECK(f.mgr.capture == static_cast<cIGZWin*>(recorder));
        CHECK_EQ(recorder->DragAnchorX(), 50);
        CHECK_EQ(recorder->DragAnchorY(), 40);
        CHECK(recorder->GZOnMouseMove(53, 44, 0));
        CHECK_RECT(recorder->GetArea(), 13, 24, 113, 84);
        CHECK_EQ(recorder->DragAnchorX(), 50);                         // ANIMREC-1: the anchor is written back
        CHECK(recorder->GZOnMouseMove(53, 44, 0));
        CHECK_RECT(recorder->GetArea(), 16, 28, 116, 88);              // and the same offset is applied again
        CHECK(recorder->GZOnMouseUpL(53, 44, 0));
        CHECK(f.mgr.capture == nullptr);
        recorder->GZOnMouseMove(60, 60, 0);
        CHECK_RECT(recorder->GetArea(), 16, 28, 116, 88);              // no capture: no move

        // GetCurrentCaptureArea: the outline inset by one pixel, never smaller than 2 x 2
        cRZRect capture;
        recorder->GetCurrentCaptureArea(capture);
        CHECK_RECT(capture, 101, 101, 0x105, 0xdd);
        recorder->Outline()->SetArea(10, 10, 11, 11);
        recorder->GetCurrentCaptureArea(capture);
        CHECK_RECT(capture, 11, 11, 12, 12);

        // FixAnimationName keeps alphanumerics and spaces only
        cRZString name("My Anim/ation:*? 2");
        recorder->FixAnimationName(name);
        CHECK_STR(name.Data(), "My Animation 2");
        std::string longName(200, 'x');
        longName[3] = '/';
        cRZString longString(longName.c_str());
        recorder->FixAnimationName(longString);
        CHECK_EQ(longString.Strlen(), 127u);                           // 128 bytes minus the removed '/'

        recorder->Shutdown();
        CHECK_EQ(recorder->GetChildCount(), 0u);
        CHECK_EQ(f.view.GetChildCount(), 0u);
        CHECK(recorder->Outline() == nullptr);
        CHECK_EQ(f.host.font.refs, 0);                                 // the labels released their font
        recorder->Release();
        CHECK(chdir(startDirectory) == 0);
    }

    // ---- B: the preference file, the output directory quirks and the whole recording pipeline -----------------------
    {
        std::string ini = g_root + "/record.ini";
        {
            std::FILE* file = std::fopen(ini.c_str(), "wb");
            CHECK(file != nullptr);
            if (file) {
                std::fprintf(file,
                             "[Animation Recorder]\nName=Walk Cycle\nRate=100\nTime=1\nRun=4\n"
                             "Output Directory=%s/out\nGX Directory=%s/Bin/GX/\nOutput Type=1\n",
                             g_root.c_str(), g_root.c_str());
                std::fclose(file);
            }
        }
        Fixture f;
        f.host.iniPath = ini;
        // the saver remembers the outline's *size* under the outline id + 1
        f.saver.SaveWindowLocation(kAnimationRecordOutlineId + 1, 60, 40);
        cSC4WinAnimationRecorder* recorder = f.Make();
        CHECK_STR(recorder->Name().Data(), "Walk Cycle");
        CHECK_STR(recorder->Rate().Data(), "100");
        CHECK_STR(recorder->Time().Data(), "1");
        CHECK_EQ(recorder->Take(), 4);
        CHECK_STR(recorder->OutputDirectory().Data(), g_root + "/out");   // ReadPreferences does not add the separator
        CHECK_STR(recorder->GXDirectory().Data(), g_root + "/Bin/GX/");
        CHECK_EQ(recorder->OutputType(), kAnimationRecordBMP);            // the preference is read ...
        CHECK(recorder->DeleteBitmapsWhenFinished());

        // ANIMREC-3: the directory quirks
        recorder->SetAnimationOutputDirectory(cRZString((g_root + "/out3/").c_str()), false);
        CHECK_STR(recorder->OutputDirectory().Data(), g_root + "/out3/");
        recorder->SetAnimationOutputDirectory(cRZString(("/tmp/sc4_war_test_new_" + std::to_string(getpid())).c_str()), false);
        CHECK_STR(recorder->OutputDirectory().Data(), "/tmp/sc4_war_test_new_" + std::to_string(getpid()) + "/");
        // createIfMissing with an existing current directory: the new one is ignored (ANIMREC-3)
        recorder->SetAnimationOutputDirectory(cRZString((g_root + "/out4/").c_str()), false);
        CHECK_STR(recorder->OutputDirectory().Data(), g_root + "/out4/");
        recorder->SetAnimationOutputDirectory(cRZString((g_root + "/out5").c_str()), true);
        CHECK_STR(recorder->OutputDirectory().Data(), g_root + "/out4/");
        CHECK(!PathExists(g_root + "/out5"));                             // it did not even try to create it
        // an empty directory becomes "/"
        recorder->SetAnimationOutputDirectory(cRZString(""), false);
        CHECK_STR(recorder->OutputDirectory().Data(), "/");
        recorder->SetAnimationOutputDirectory(cRZString((g_root + "/out3/").c_str()), false);

        CHECK(recorder->Init());
        CHECK_EQ(recorder->GetChildCount(), 12u);
        CHECK_EQ(recorder->Outline()->GetW(), 60);                        // restored from the saver under id + 1
        CHECK_EQ(recorder->Outline()->GetH(), 40);
        CHECK_EQ(recorder->OutputTypeGroup()->GetValue(), 3u);            // ANIMREC-8: the ini value is not applied
        CHECK_EQ(recorder->OutputType(), kAnimationRecordBMP);

        // Start: rate 100 ms, time 1 s -> 10 frames of the capture rectangle (100,100,160,140) inset by one
        CHECK(recorder->Start());
        CHECK(recorder->Timer().IsTimerRunning());
        CHECK_EQ(recorder->Take(), 5);
        CHECK_EQ(recorder->FrameCount(), 0);
        CHECK_EQ(recorder->FrameIntervalMs(), 100);
        CHECK_EQ(recorder->RecordingSeconds() * 100.0f, 100);             // 1.0 s
        CHECK_EQ(recorder->FrameTarget(), 10);
        CHECK_EQ(recorder->FramesLeft(), 10);
        CHECK_EQ(recorder->TickDivider(), 4);
        CHECK_EQ(recorder->FrameOffset(), 0);
        CHECK_RECT(recorder->CaptureArea(), 101, 101, 159, 139);
        CHECK_EQ(f.frames().size(), 10u);
        for (FakeFrameBuffer* frame : f.frames()) {
            CHECK_EQ(frame->inits, 1);
            CHECK_EQ(frame->width, 58u);
            CHECK_EQ(frame->height, 38u);
            CHECK_EQ(frame->colorType.type, kBufferColorARGB8888);
            CHECK_EQ(frame->colorType.bitsPerPixel, 32u);
        }
        CHECK_STR(Caption(recorder->StartButton()->AsIGZWin()), "Stop");
        CHECK(recorder->SpanButton()->AsIGZWin()->GetFlag(kWinEnabled));
        CHECK_STR(Caption(recorder->StatusText()), "Status: Recording.\nFrame count: 0");
        CHECK(recorder->Outline()->GetFlag(kWinIgnoreCursor));
        CHECK(!recorder->Outline()->Resizing());
        CHECK(recorder->Outline()->Moving());
        CHECK_EQ(f.tick.paused.size(), 1u);
        CHECK_EQ(f.tick.paused[0], 1);
        CHECK_EQ(f.tick.deltas.size(), 1u);
        CHECK_EQ(f.tick.deltas[0], 100000u);                              // rate * 1000
        CHECK(f.mgr.posted.size() >= 2u);
        CHECK_EQ(f.mgr.posted.back().type, kAnimationRecordTick);

        // a second Start while running does nothing
        CHECK(!recorder->Start());
        CHECK_EQ(f.frames().size(), 10u);

        // RecordCurrentFrame copies the capture rectangle out of the driver's colour buffer into the next bitmap
        CHECK(recorder->RecordCurrentFrame());
        CHECK_EQ(recorder->FrameCount(), 1);
        CHECK_EQ(f.colorBuffer.copies, 1);
        CHECK_EQ(f.colorBuffer.x, 101);
        CHECK_EQ(f.colorBuffer.y, 101);
        CHECK_EQ(f.colorBuffer.w, 58);
        CHECK_EQ(f.colorBuffer.h, 38);
        CHECK(f.colorBuffer.destination == f.frames()[0]);
        CHECK_STR(Caption(recorder->StatusText()), "Status: Recording.\nFrame count: 1");

        // the animation tick manager: four ticks per frame, then RecordCurrentFrame + StepAnimationSystem
        cGZMessage tickMessage;
        tickMessage.type = kAnimationRecordTick;
        for (int i = 0; i < 3; ++i) CHECK(recorder->DoMessage(tickMessage));
        CHECK_EQ(recorder->FrameCount(), 1);
        CHECK_EQ(f.tick.steps, 0);
        CHECK_EQ(recorder->TickDivider(), 1);
        CHECK(recorder->DoMessage(tickMessage));
        CHECK_EQ(recorder->FrameCount(), 2);
        CHECK_EQ(f.tick.steps, 1);
        CHECK_EQ(recorder->FramesLeft(), 9);
        CHECK_EQ(recorder->TickDivider(), 4);                             // rearmed
        CHECK(f.mgr.posted.size() >= 6u);                                 // one more post per tick

        // an unrelated message goes to cGZWin (here: no handler, still reported as handled by the base)
        cGZMessage other;
        other.type = 0x1234;
        recorder->DoMessage(other);

        // Stop: the Yes/No box, then the write-out through the Gimex factory (the group says jpg, the ini said bmp)
        f.mgr.msgBoxAnswer = static_cast<int>(gz::kMsgBoxYes);
        recorder->OutputTypeGroup()->SetValue(1);                         // bmp
        CHECK(recorder->Stop());
        CHECK(!recorder->Timer().IsTimerRunning());
        CHECK_EQ(f.mgr.msgBoxes.size(), 2u);
        CHECK_STR(f.mgr.msgBoxes[0].text, "Would you like to write out the animation frames?");
        CHECK_EQ(f.mgr.msgBoxes[0].style, 3u);
        CHECK_EQ(f.host.saved.size(), 2u);
        CHECK_STR(f.host.saved[0].path, g_root + "/out3/Walk Cycle 005_000.bmp");
        CHECK_STR(f.host.saved[1].path, g_root + "/out3/Walk Cycle 005_001.bmp");
        CHECK_EQ(f.host.saved[0].flags, 100u);
        CHECK_EQ(recorder->WrittenFiles().size(), 2u);
        CHECK_STR(recorder->WrittenFiles()[0].Data(), g_root + "/out3/Walk Cycle 005_000.bmp");
        CHECK_EQ(recorder->OutputType(), kAnimationRecordBMP);
        CHECK_EQ(recorder->FrameOffset(), 2);                             // advanced by the frame count
        CHECK_STR(f.mgr.msgBoxes[1].text, "AnimationFrames written OK.\n\n Output is in " + g_root + "/out3/");
        CHECK_EQ(f.frames()[0]->refs, 0);                                 // DestroyInternalBitmaps released all frames
        CHECK_EQ(recorder->Bitmaps().size(), 0u);
        CHECK_STR(Caption(recorder->StartButton()->AsIGZWin()), "Start");
        CHECK(!recorder->SpanButton()->AsIGZWin()->GetFlag(kWinEnabled));
        CHECK_STR(Caption(recorder->StatusText()), "Status: Stopped.\nFrame count: 0");
        CHECK(recorder->Outline()->Resizing());
        CHECK(!recorder->Outline()->GetFlag(kWinIgnoreCursor));
        CHECK_EQ(f.tick.paused.size(), 2u);
        CHECK_EQ(f.tick.paused[1], 0);
        CHECK(recorder->Stop() == false);                                 // not running any more

        // the three file-name formats; the frame offset advances with every write-out (ANIMREC-7)
        for (int type = 1; type <= 3; ++type) {
            f.host.saved.clear();
            f.mgr.msgBoxes.clear();
            f.mgr.msgBoxAnswer = static_cast<int>(gz::kMsgBoxYes);
            CHECK(recorder->Start());
            recorder->OutputTypeGroup()->SetValue(static_cast<std::uint32_t>(type));
            CHECK(recorder->RecordCurrentFrame());
            CHECK(recorder->Stop());
            CHECK_EQ(f.host.saved.size(), 1u);
            const char* extension = type == 1 ? "bmp" : type == 2 ? "png" : "jpg";
            const int take = 6 + (type - 1);
            const int frame = 0;                                          // Start resets the frame offset
            char expected[512];
            std::snprintf(expected, sizeof expected, "%s/out3/Walk Cycle %03d_%03d.%s", g_root.c_str(), take, frame, extension);
            CHECK_STR(f.host.saved[0].path, expected);
        }

        // ANIMREC-6: with loop mode off the frame list is capped and a full list stops the recording
        recorder->SetLoopRecording(false);
        f.mgr.msgBoxes.clear();
        f.mgr.msgBoxAnswer = static_cast<int>(gz::kMsgBoxNo);
        const std::size_t madeBefore = f.frames().size();
        CHECK(recorder->Start());
        CHECK_EQ(recorder->FrameTarget(), 10);
        CHECK_EQ(f.frames().size(), madeBefore + 10);
        for (int i = 0; i < 10; ++i) CHECK(recorder->RecordCurrentFrame());
        CHECK_EQ(recorder->FrameCount(), 10);
        CHECK(!recorder->RecordCurrentFrame());                           // full and not looping: Stop
        CHECK(!recorder->Timer().IsTimerRunning());
        CHECK_EQ(f.mgr.msgBoxes.size(), 1u);                              // the Yes/No box from the Stop inside
        CHECK_STR(f.mgr.msgBoxes[0].text, "Would you like to write out the animation frames?");
        CHECK_EQ(recorder->Bitmaps().size(), 0u);

        // the millisecond-timer pacing (loop mode off means no tick manager)
        recorder->SetLoopRecording(false);
        f.tick.paused.clear();
        g_ticks += 10;
        CHECK(recorder->Start());
        CHECK_EQ(f.tick.paused.size(), 0u);                               // the tick manager is not driven
        CHECK_EQ(recorder->RecordCurrentFrame(), true);
        CHECK_EQ(recorder->FrameCount(), 1);
        recorder->Timer().SetElapsedTimeU64(99);                          // below the interval: nothing
        CHECK(recorder->DoMessage(tickMessage));
        CHECK_EQ(recorder->FrameCount(), 1);
        recorder->Timer().SetElapsedTimeU64(100);                         // at the interval: one frame
        CHECK(recorder->DoMessage(tickMessage));
        CHECK_EQ(recorder->FrameCount(), 2);
        recorder->Stop();

        // WritePreferences: the section and the keys the original writes
        f.host.saved.clear();
        f.mgr.msgBoxAnswer = static_cast<int>(gz::kMsgBoxNo);
        CHECK(recorder->WritePreferences());
        const std::string text = ReadFile(ini);
        CHECK(text.find("[Animation Recorder]") != std::string::npos);
        CHECK(text.find("Name=Walk Cycle") != std::string::npos);
        CHECK(text.find("Rate=100") != std::string::npos);
        CHECK(text.find("Time=1") != std::string::npos);
        CHECK(text.find("Run=10") != std::string::npos);                   // Run 4 + six Starts in this block
        CHECK(text.find("Output Directory=" + g_root + "/out3/") != std::string::npos);
        CHECK(text.find("GX Directory=" + g_root + "/Bin/GX/") != std::string::npos);
        CHECK(text.find("Output Type=3") != std::string::npos);

        // the empty output directory was turned into "/" and back; the recording wrote no frames before the last Start
        CHECK(recorder->Outline() != nullptr);

        // Shutdown: the saver entries, the released extension and the preference write
        recorder->Shutdown();
        int x = 0, y = 0;
        CHECK(f.saver.GetWindowRestoreLocation(kAnimationRecordOutlineId, x, y, false));
        CHECK_EQ(x, 100);
        CHECK_EQ(y, 100);
        CHECK(f.saver.GetWindowRestoreLocation(kAnimationRecordOutlineId + 1, x, y, false));
        CHECK_EQ(x, 60);
        CHECK_EQ(y, 40);
        CHECK(f.saver.GetWindowRestoreLocation(0x6acb1af1u, x, y, false));
        CHECK_EQ(x, 4);
        CHECK_EQ(y, 4);
        CHECK_EQ(f.colorBuffer.refs, 0);
        CHECK_EQ(recorder->GetChildCount(), 0u);
        CHECK_EQ(f.view.GetChildCount(), 0u);
        const std::string after = ReadFile(ini);
        CHECK(after.find("Name=Walk Cycle") != std::string::npos);
        recorder->Release();
        CHECK(f.mgr.cleanups >= 1);                                      // cGZWin's destructor cleaned up
    }

    SetWinAnimationRecorderHost(nullptr);
    SetWinGraphics(nullptr);
    if (failures == 0) std::printf("sc4_winanimationrecorder_test: ok\n");
    else std::printf("sc4_winanimationrecorder_test: %d failure(s)\n", failures);
    return failures != 0;
}

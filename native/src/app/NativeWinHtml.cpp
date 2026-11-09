// NativeWinHtml — the engine side of the HTML window (gz/WinHTML) and of the windows built on it: the news ticker's advice
// lists (sc4/WinAdviceList) and the ticker's window procedure (sc4/NewsTickerWinProc).
//
// Not game code (there is no original counterpart): the seams the recovered classes expect, implemented over what the native
// game already has.
//  * IWinHtmlCanvas    = the window's draw buffer (native::Buffer): fills, lines and blits through the same cIGZBuffer the
//                        other windows draw with (the screen's draw context, or a private buffer's pixels).
//  * IHtmlFontSystem   = cIGZFontSys::CreateFont: the game's own font files (GameData/Fonts) as recovered cGZFonts, one shared
//                        font per (file, size); bold / italic pick the face file, underline / strike are the font's own styles.
//  * IHtmlImageDecoder = the PNG / BMP decoder the UI uses; "sc4://HTML/<group>/<instance>" names a UI picture resource
//                        (type 0x856ddbac), anything else a file.
//  * IHtmlWebSystem    = the part of the WWW system the pages reach: sc4:// resources and local files. A job reports back on
//                        the next message pump (a posted message), never inside StartJob, as the original's download threads do.
//  * IWinAdviceListHost, INewsTickerWindow: the advice lists' HTML child (a real cGZWinHTML through GZCOM) and the ticker.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeAudio.h"
#include "app/NativeGraphics.h"
#include "app/NativeUi.h"
#include "gz/Buffer.h"
#include "gz/GZCOM.h"
#include "gz/GZCOMDirector.h"
#include "gz/GZFont.h"
#include "gz/WinHTML.h"
#include "gz/cIGZMessage2.h"
#include "rz/COMDirectors.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/NewsTickerWinProc.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/WinAdviceList.h"

namespace native {

cGZFont* HtmlFont(const std::string& faces, std::uint32_t size, bool bold, bool italic);   // linux/game_main_linux.cpp
cGZFont* HtmlStyleFont(const std::string& styleName, std::uint32_t size);

namespace {

bool HtmlDebug() {
    static const bool on = std::getenv("SC4_HTMLDEBUG") != nullptr;
    return on;
}

constexpr std::uint32_t kUiPictureType = 0x856ddbac;
// The message the web system posts to itself to deliver finished jobs on the main thread (native only).
constexpr std::uint32_t kWebDeliverMessage = 0x0b7e4d01;

cRZRect Intersect(const cRZRect& a, const cRZRect& b) {
    return cRZRect{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
bool Empty(const cRZRect& r) { return r.left >= r.right || r.top >= r.bottom; }

// ---- pictures ----------------------------------------------------------------------------------------------------------------

class HtmlBitmap final : public html::IHtmlBitmap {
public:
    explicit HtmlBitmap(cGZBuffer* pixels) : pixels_(pixels) {}   // takes over one reference
    ~HtmlBitmap() override { if (pixels_) pixels_->Release(); }
    void AddRef() override { ++refs_; }
    void Release() override { if (--refs_ == 0) delete this; }
    int Width() override { return int(pixels_->Width()); }
    int Height() override { return int(pixels_->Height()); }
    cRZRect Bounds() override { return cRZRect{0, 0, Width(), Height()}; }
    cGZBuffer* Pixels() { return pixels_; }

private:
    int refs_ = 1;
    cGZBuffer* pixels_;
};

cGZBuffer* NewPixels(int w, int h) {
    auto* buffer = new cGZBuffer();
    buffer->AddRef();
    buffer->Init(std::uint32_t(w), std::uint32_t(h), cGZBufferColorType{kBufferColorARGB8888, 32});
    return buffer;
}

// "sc4://HTML/<group>/<instance>" (hex, any case) -> the UI picture resource.
bool ParseResourceUrl(const std::string& url, std::uint32_t& group, std::uint32_t& instance) {
    static const char kPrefix[] = "sc4://html/";
    if (url.size() <= sizeof kPrefix - 1) return false;
    for (std::size_t i = 0; i + 1 < sizeof kPrefix; ++i)
        if (char(std::tolower(static_cast<unsigned char>(url[i]))) != kPrefix[i]) return false;
    const char* p = url.c_str() + sizeof kPrefix - 1;
    char* end = nullptr;
    group = std::uint32_t(std::strtoul(p, &end, 16));
    if (end == p || *end != '/') return false;
    p = end + 1;
    instance = std::uint32_t(std::strtoul(p, &end, 16));
    return end != p;
}

class ImageDecoder final : public html::IHtmlImageDecoder {
public:
    // The game's own pictures (sc4:// resources) never change while it runs: the advice lists rebuild their page on every
    // advice change, so each picture is decoded once and shared (the cache holds one reference). Files are read every time.
    bool LoadImageFile(const std::string& path, html::IHtmlBitmap*& bitmap) override {
        bitmap = nullptr;
        cGZBuffer* pixels = nullptr;
        std::uint32_t group = 0, instance = 0;
        if (ParseResourceUrl(path, group, instance)) {
            const std::uint64_t key = std::uint64_t(group) << 32 | instance;
            auto cached = resources_.find(key);
            if (cached == resources_.end()) {
                cGZBuffer* decoded = LoadBitmapResource(group, instance);
                if (HtmlDebug()) std::printf("html: image %s -> %s\n", path.c_str(), decoded ? "loaded" : "missing");
                cached = resources_.emplace(key, decoded ? new HtmlBitmap(decoded) : nullptr).first;
            }
            if (!cached->second) return false;
            cached->second->AddRef();
            bitmap = cached->second;
            return true;
        } else if (PngDecoder decode = DecodeFunction()) {
            std::ifstream file(path, std::ios::binary);
            if (file) {
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                pixels = decode(bytes);
            }
        }
        if (HtmlDebug()) std::printf("html: image %s -> %s\n", path.c_str(), pixels ? "loaded" : "missing");
        if (!pixels) return false;
        bitmap = new HtmlBitmap(pixels);
        return true;
    }

private:
    std::map<std::uint64_t, HtmlBitmap*> resources_;   // never released: the pictures live as long as the game
};

// ---- the canvas --------------------------------------------------------------------------------------------------------------

class Canvas final : public IWinHtmlCanvas {
public:
    explicit Canvas(Buffer* buffer) : buffer_(buffer) {}

    // The cIGZBuffer the window's other drawing uses: the screen's draw context, or a private buffer's pixels (Buffer::Fill).
    cIGZBuffer* Target() { return buffer_->OnScreen() ? buffer_->AsBuffer() : static_cast<cIGZBuffer*>(buffer_->Pixels()); }
    // Text always goes through the context (the windows' fonts draw the same way: WinFont::DrawText).
    cIGZBuffer* TextTarget() { return buffer_->AsBuffer(); }

    bool Begin(std::uint32_t) override { return true; }
    void End(std::uint32_t) override {}

    void FillRect(html::Color color, const cRZRect& rect, const cRZRect* clip) override {
        Target()->Fill(color, rect, clip);
    }
    void DrawLine(int x0, int y0, int x1, int y1, html::Color color, int width) override {
        Target()->DrawLine(x0, y0, x1, y1, int(color), std::uint32_t(std::max(width, 1)));
    }
    void Blit(html::IHtmlBitmap* source, const cRZRect& sourceRect, const cRZRect& destination, const cRZRect* clip) override {
        if (!source) return;
        BlitClipped(static_cast<HtmlBitmap*>(source)->Pixels(), sourceRect, destination, clip);
    }
    void BlitStretched(html::IHtmlBitmap* source, const cRZRect& sourceRect, const cRZRect& destination, const cRZRect* clip) override {
        if (!source) return;
        const int w = destination.right - destination.left, h = destination.bottom - destination.top;
        const int sw = sourceRect.right - sourceRect.left, sh = sourceRect.bottom - sourceRect.top;
        if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
        if (w == sw && h == sh) { Blit(source, sourceRect, destination, clip); return; }
        cGZBuffer* from = static_cast<HtmlBitmap*>(source)->Pixels();
        cGZBuffer* scaled = NewPixels(w, h);   // nearest neighbour, as the software blitter scales
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                scaled->SetPixel(std::uint32_t(x), std::uint32_t(y),
                                 from->GetPixel(std::uint32_t(sourceRect.left + x * sw / w), std::uint32_t(sourceRect.top + y * sh / h)));
        BlitClipped(scaled, cRZRect{0, 0, w, h}, destination, clip);
        scaled->Release();
    }
    void Outline(const cRZRect& r, std::uint32_t color) override {
        Target()->Fill(color, cRZRect{r.left, r.top, r.right, r.top + 1}, nullptr);
        Target()->Fill(color, cRZRect{r.left, r.bottom - 1, r.right, r.bottom}, nullptr);
        Target()->Fill(color, cRZRect{r.left, r.top, r.left + 1, r.bottom}, nullptr);
        Target()->Fill(color, cRZRect{r.right - 1, r.top, r.right, r.bottom}, nullptr);
    }
    // RZDrawBltTiled: the source tiles the destination, starting `offset` pixels into the pattern.
    void BltTiled(html::IHtmlBitmap* source, const cRZRect& sourceRect, const cRZRect& destRect, int offsetX, int offsetY) override {
        if (!source) return;
        const int tw = sourceRect.right - sourceRect.left, th = sourceRect.bottom - sourceRect.top;
        if (tw <= 0 || th <= 0 || Empty(destRect)) return;
        cGZBuffer* from = static_cast<HtmlBitmap*>(source)->Pixels();
        const int startX = destRect.left - (((offsetX % tw) + tw) % tw);
        const int startY = destRect.top - (((offsetY % th) + th) % th);
        for (int y = startY; y < destRect.bottom; y += th)
            for (int x = startX; x < destRect.right; x += tw)
                BlitClipped(from, sourceRect, cRZRect{x, y, x + tw, y + th}, &destRect);
    }

private:
    // cGZBuffer::Blt only reads the destination's top-left corner, so the clip is applied to both rectangles here.
    void BlitClipped(cGZBuffer* from, const cRZRect& sourceRect, const cRZRect& destination, const cRZRect* clip) {
        cRZRect dest{destination.left, destination.top, destination.left + (sourceRect.right - sourceRect.left),
                     destination.top + (sourceRect.bottom - sourceRect.top)};
        dest = Intersect(dest, destination);
        if (clip) dest = Intersect(dest, *clip);
        if (Empty(dest)) return;
        const cRZRect src{sourceRect.left + (dest.left - destination.left), sourceRect.top + (dest.top - destination.top),
                          sourceRect.left + (dest.right - destination.left), sourceRect.top + (dest.bottom - destination.top)};
        Target()->Blt(from, src, dest, nullptr);
    }
    Buffer* buffer_;
};

// ---- fonts -------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kHtmlBold = 1, kHtmlItalic = 2;   // cHTMLDocument's style bits (the face's own; 4 / 8 are the font's)
constexpr int kFontEditableStyles = 0x3c;                 // cGZFont::SetStyle

// One cIGZFont reference per text segment. The cGZFont is shared, so the segment's own style bits are applied before every call.
class HtmlFontFace final : public html::IHtmlFont {
public:
    explicit HtmlFontFace(cGZFont* font) : font_(font) {}
    void Release() override { delete this; }
    void SetStyle(std::uint32_t style, bool enable) override {
        const std::uint32_t bits = style & std::uint32_t(kFontEditableStyles);
        style_ = enable ? (style_ | bits) : (style_ & ~bits);
    }
    int LineHeight() override { Apply(); return font_->GetLineHeight(); }
    int GlyphHeight(int a, int b) override { Apply(); return font_->CharHeight(reinterpret_cast<const void*>(std::intptr_t(a)), std::uint32_t(b)); }
    int Ascent() override { Apply(); return font_->GetSizeAboveBaseline(); }
    int CharsThatFit(const char* text, int length, int maxWidth, int a, int b) override {
        Apply();
        (void)a;   // the segment passes 0: no "fitted width" out-parameter
        return int(font_->CalculateWordsToFitInWidth(text, std::uint32_t(std::max(length, 0)), std::uint32_t(std::max(maxWidth, 0)), nullptr,
                                                     static_cast<cIGZFont::WordWrapMode>(b)));
    }
    int FirstWordLength(const char* text, int length, int maxWidth, int* wordWidth) override {
        Apply();
        std::uint32_t fitted = 0;
        const int n = int(font_->CalculateCharsToFitInWidth(text, std::uint32_t(std::max(length, 0)), std::uint32_t(std::max(maxWidth, 0)), &fitted));
        if (wordWidth) *wordWidth = int(fitted);
        return n;
    }
    int StringWidth(const char* text, int length) override { Apply(); return font_->GetStringWidth(text, std::uint32_t(std::max(length, 0))); }
    void DrawText(html::IHtmlSurface* surface, int x, int y, const char* text, int length, html::Color color, std::uint32_t flags,
                  const cRZRect* clip) override {
        if (!surface || length <= 0) return;
        Apply();
        font_->DrawTextLine(static_cast<Canvas*>(static_cast<IWinHtmlCanvas*>(surface))->TextTarget(), x, y, text, std::uint32_t(length),
                            color & 0xffffffu, flags, clip, nullptr);
    }

private:
    void Apply() {
        font_->SetStyle(kFontEditableStyles, false);
        font_->SetStyle(int(style_), true);
    }
    cGZFont* font_;   // shared, never released (HtmlFont's cache owns it)
    std::uint32_t style_ = 0;
};

class FontSystem final : public html::IHtmlFontSystem {
public:
    html::IHtmlFont* CreateFont(const std::string& faceName, std::uint32_t kind, std::uint32_t size, std::uint32_t style, bool) override {
        std::string face = faceName;
        // No face asked for: the document's default kinds (bit 1 = fixed width). The game ships no fixed-width face, so both use
        // the UI's default family.
        if (face.empty()) face = (kind & 2) ? "Courier New,Arta" : "Arta";
        // A font style's name (the advisor popups' system fonts) brings the style's face and parameters; else a face family.
        cGZFont* font = native::HtmlStyleFont(face, size);
        if (!font) font = native::HtmlFont(face, size, (style & kHtmlBold) != 0, (style & kHtmlItalic) != 0);
        if (HtmlDebug()) std::printf("html: font \"%s\" kind %u size %u style %x -> %s\n", faceName.c_str(), kind, size, style, font ? "ok" : "none");
        if (!font) return nullptr;
        auto* out = new HtmlFontFace(font);
        out->SetStyle(style, true);
        return out;
    }
};

// ---- the web system ----------------------------------------------------------------------------------------------------------

class WebSystem final : public html::IHtmlWebSystem, public cIGZMessageTarget2 {
public:
    std::uint32_t StartJob(const std::string&) override { return 0; }   // nothing to report to
    std::uint32_t StartJob(const std::string& url, html::IHtmlDownloadTarget* target) override {
        if (!target) return 0;
        Job job;
        job.target = target;
        job.result.url = url;
        std::uint32_t group = 0, instance = 0;
        if (ParseResourceUrl(url, group, instance)) {
            job.result.isFile = true;   // the decoder opens sc4:// names itself
            job.result.data = url;
        } else {
            std::string path = url;
            if (path.compare(0, 7, "file://") == 0) path.erase(0, 7);
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                if (HtmlDebug()) std::printf("html: web job %s refused (no such resource)\n", url.c_str());
                return 0;
            }
            // a page comes back as its text (the window parses job.data), a picture as its file
            const bool picture = url.size() > 4 && (url.find(".png") != std::string::npos || url.find(".bmp") != std::string::npos ||
                                                    url.find(".jpg") != std::string::npos);
            job.result.isFile = picture;
            job.result.data = picture ? path : std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }
        job.id = ++lastId_;
        queue_.push_back(job);
        if (HtmlDebug()) std::printf("html: web job %u %s\n", job.id, url.c_str());
        Wake();
        return job.id;
    }
    void CancelJob(std::uint32_t jobId) override {
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.id == jobId; }), queue_.end());
    }
    bool IsVisited(const std::string&) override { return false; }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0x452294aa || iid == 1) { *out = static_cast<cIGZMessageTarget2*>(this); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cIGZMessage2* message) override {
        if (!message || message->GetType() != kWebDeliverMessage) return false;
        posted_ = false;
        // A delivery may start or cancel jobs: take them one at a time from the front.
        std::size_t budget = queue_.size();
        while (budget-- > 0 && !queue_.empty()) {
            Job job = queue_.front();
            queue_.erase(queue_.begin());
            job.target->Downloaded(true, job.result);
        }
        if (!queue_.empty()) Wake();
        return true;
    }

private:
    struct Job {
        std::uint32_t id = 0;
        html::IHtmlDownloadTarget* target = nullptr;
        html::tDownloadedJob result;
    };
    void Wake() {
        cIGZMessageServer2* server = SL::spMS2;
        if (!server || posted_) return;
        if (!registered_) { server->AddNotification(this, kWebDeliverMessage); registered_ = true; }
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kWebDeliverMessage);
        posted_ = server->MessagePost(message, false);
        message->Release();
    }
    std::vector<Job> queue_;
    std::uint32_t lastId_ = 0;
    bool registered_ = false;
    bool posted_ = false;
};

// ---- the host ----------------------------------------------------------------------------------------------------------------

class WinHtmlHost final : public IWinHtmlHost {
public:
    IWinHtmlCanvas* Canvas(IWinBuffer* windowBuffer) override {
        if (!windowBuffer) return nullptr;
        std::unique_ptr<native::Canvas>& canvas = canvases_[windowBuffer];
        if (!canvas) canvas.reset(new native::Canvas(static_cast<Buffer*>(windowBuffer)));
        return canvas.get();
    }
    html::IHtmlFontSystem* Fonts() override { return &fonts_; }
    html::IHtmlImageDecoder* Images() override { return &images_; }
    html::IHtmlWebSystem* WebSystem() override { return &web_; }
    // cHTMLDocument::CreateBullet: a size x size picture, the background colour with a disc or a square of the text colour.
    html::IHtmlBitmap* CreateBullet(int size, html::Color background, html::Color foreground, bool disc) override {
        if (size <= 0) return nullptr;
        cGZBuffer* pixels = NewPixels(size, size);
        pixels->Fill(background | 0xff000000u, nullptr);   // the window colours carry no alpha
        const float c = (size - 1) * 0.5f, r2 = (size * 0.5f) * (size * 0.5f);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (!disc || (x - c) * (x - c) + (y - c) * (y - c) <= r2) pixels->SetPixel(std::uint32_t(x), std::uint32_t(y), foreground | 0xff000000u);
        return new HtmlBitmap(pixels);
    }

private:
    std::map<IWinBuffer*, std::unique_ptr<native::Canvas>> canvases_;
    FontSystem fonts_;
    ImageDecoder images_;
    native::WebSystem web_;
};

// ---- the advice lists --------------------------------------------------------------------------------------------------------

// The cGZWinHTML child as cSC4WinAdviceList drives it (one reference on the window).
class AdviceListHtml final : public sc4::IAdviceListHtml {
public:
    explicit AdviceListHtml(cIGZWinHTML* html) : html_(html) {}
    void Release() override { html_->Release(); delete this; }
    cIGZWin* AsIGZWin() override { return html_->AsIGZWin(); }
    int WindowWidth() override { return html_->AsIGZWin()->GetW(); }
    void SetArea(int l, int t, int r, int b) override { html_->AsIGZWin()->SetArea(l, t, r, b); }
    void GetScrollPosition(int& x, int& y) override { html_->GetScrollPosition(x, y); }
    void SetScrollPosition(int x, int y) override { html_->SetScrollPosition(x, y); }
    bool ScrollToAnchor(const cIGZString& anchor) override { return html_->ScrollToAnchor(anchor); }
    bool SetDocument(const std::string& text, const cIGZString& baseUrl) override {
        if (HtmlDebug()) std::printf("html: advice list document (%zu bytes)\n", text.size());
        return html_->SetDocument(text, cRZString(baseUrl));
    }
    void ClearFlag(int flag) override { html_->ClearFlag(flag); }
    void SetGutters(int l, int t, int r, int b) override { html_->SetGutters(l, t, r, b); }

private:
    cIGZWinHTML* html_;
};

class WinAdviceListHost final : public sc4::IWinAdviceListHost {
public:
    sc4::IAdviceListHtml* CreateHtmlWindow() override {
        void* out = nullptr;
        if (!GZCOM() || !GZCOM()->CreateInstance(gzcom::kCLSID_cGZWinHTML, kGZIID_cIGZWinHTML, &out) || !out) {
            std::printf("html: no cGZWinHTML class (advice list without its page)\n");
            return nullptr;
        }
        return new AdviceListHtml(static_cast<cIGZWinHTML*>(out));
    }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    void PlaySound(std::uint32_t id) override { PlayGameSound(id); }
    bool HandleUrl(const cIGZString& link) override { return native::NativeHtmlLinkDoURL(link); }   // cSC4HTMLLinkHandler::DoURL
};

// ---- the news ticker ---------------------------------------------------------------------------------------------------------

// cSC4View3DWin's +0x450 slot over the recovered cSC4NewsTickerWinProc (which attaches itself to the ticker's script window).
class NewsTickerWindow final : public sc4::INewsTickerWindow {
public:
    NewsTickerWindow() : proc_(new sc4::cSC4NewsTickerWinProc()) { proc_->AddRef(); }
    ~NewsTickerWindow() { proc_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinProc || iid == 1) { AddRef(); *out = static_cast<gz::cIGZWinProc*>(this); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override { return proc_->DoWinProcMessage(window, message); }
    bool Init(gz::cIGZWinGen* window) override {
        const bool ok = proc_->Init(window);
        if (HtmlDebug() || !ok) std::printf("ticker: init %s (advice list %s, flipper %s)\n", ok ? "ok" : "FAILED",
                                            proc_->AdviceList() ? "found" : "missing", proc_->FlipperList() ? "found" : "missing");
        return ok;
    }
    void ShowTicker() override { proc_->ShowTicker(); }
    void HideTicker() override { proc_->HideTicker(); }
    void Shutdown() { proc_->Shutdown(); }
    sc4::cSC4NewsTickerWinProc* Proc() { return proc_; }

private:
    std::uint32_t refs_ = 0;
    sc4::cSC4NewsTickerWinProc* proc_;
};

NewsTickerWindow* g_ticker = nullptr;

}  // namespace

namespace {
cIGZWinMgr* g_winMgr = nullptr;
}

void RegisterWinHtmlMakers(cIGZWinMgr* mgr) {
    g_winMgr = mgr;
    gz::RegisterDirectorMaker("cGZWinHTML", +[]() -> cIGZUnknown* {
        return static_cast<cIGZUnknown*>(static_cast<cIGZWin*>(new cGZWinHTML(g_winMgr)));
    });
    gz::RegisterDirectorMaker("cSC4WinAdviceList", +[]() -> cIGZUnknown* {
        return static_cast<cIGZUnknown*>(static_cast<cIGZWin*>(new sc4::cSC4WinAdviceList(g_winMgr)));
    });
}

void InstallWinHtmlHosts() {
    static WinHtmlHost html;
    SetWinHtmlHost(&html);
    static WinAdviceListHost lists;
    sc4::SetWinAdviceListHost(&lists);
}

sc4::INewsTickerWindow* CreateNewsTickerWindow() {
    auto* ticker = new NewsTickerWindow();
    ticker->AddRef();
    g_ticker = ticker;
    return ticker;
}

void ShutdownNewsTickerWindow(cIGZUnknown* proc) {
    if (!proc || proc != static_cast<cIGZUnknown*>(static_cast<gz::cIGZWinProc*>(g_ticker))) return;
    g_ticker->Shutdown();
    g_ticker = nullptr;
}

void NativeTickerReport() {
    sc4::cSC4NewsTickerWinProc* t = g_ticker ? g_ticker->Proc() : nullptr;
    if (!t) { std::printf("ticker: none\n"); return; }
    const cRZRect& a = t->FlipperArea();
    std::printf("ticker: init %d visible %d notifications %d messages %zu ring %u scroll %u line %d flipper area %d,%d-%d,%d\n",
                t->Initialised(), t->TickerVisible(), t->NotificationsRegistered(), t->Messages().size(), t->RingCount(),
                t->FlipperScroll(), t->LineHeight(), a.left, a.top, a.right, a.bottom);
}

}  // namespace native

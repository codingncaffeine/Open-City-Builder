// cGZWinHTML — see gz/WinHTML.h for the layout, the interface ids, the quirks and the seams.
#include "gz/WinHTML.h"

#include "gz/WinCtrlFactory.h"
#include "rz/cRZFile.h"

// @ 0x1005690a4: the 500 ms redraw timer's callback (vslot 0x2e0, InvalidateSelfAndParents)
static void PeriodicWindowRedrawCallback(void* window) { static_cast<cGZWinHTML*>(window)->InvalidateSelfAndParents(); }

namespace {

// The Lock flag the outline uses (0x8008; not one of the html::kSurface* values).
constexpr std::uint32_t kSurfaceOutline = 0x8008;

std::string ResolveHtmlUrl(const std::string& href, const std::string& base) {
    // cHTMLDocument resolves relative URLs with cRZURL::ChangeURL(href, base).
    cRZURL baseUrl{cRZString(base.c_str())};
    cRZURL url;
    url.ChangeURL(cRZString(href.c_str()), &baseUrl);
    return url.GetFullURL().str();
}

std::string MakeHtmlTempPath() {
    // The original hands cRZFile::MakeTempPathName an empty prefix (cHTMLDocument::ResourceHasBeenDownloaded).
    cRZString path;
    cRZFile::MakeTempPathName(path, nullptr);
    return path.str();
}

// The engine side has not been wired up yet (fonts, the Gimex image decoder, the WWW system and the buffer are all
// outside the recovered set); the app installs a host, tests install a recording one.
class DefaultWinHtmlHost final : public IWinHtmlHost {
public:
    IWinHtmlCanvas* Canvas(IWinBuffer*) override { return nullptr; }
    html::IHtmlFontSystem* Fonts() override { return nullptr; }
    html::IHtmlImageDecoder* Images() override { return nullptr; }
    html::IHtmlWebSystem* WebSystem() override { return nullptr; }
};

DefaultWinHtmlHost g_defaultHost;
IWinHtmlHost* g_host = nullptr;

}  // namespace

IWinHtmlHost* WinHtmlHost() { return g_host != nullptr ? g_host : &g_defaultHost; }
void SetWinHtmlHost(IWinHtmlHost* host) { g_host = host; }

// ---- construction ----------------------------------------------------------------------------------------------------

cGZWinHTML::cGZWinHTML(cIGZWinMgr* mgr) : cGZWin(mgr) {   // @ 0x100566a34
    // The original writes its three interface vtables, constructs the draw context, the lock, the cursors, the
    // missing-bitmap cRZString, the flags / gutters, the timer and the three URLs, then finishes with the gray fill
    // colour through the virtual SetFillColor (0x100566bc3).
    SetFillColor(0xc0, 0xc0, 0xc0);
}

cGZWinHTML::~cGZWinHTML() {   // @ 0x100566ccc (body) / 0x100566e64 (complete) / 0x100566e6e (deleting)
    // The original releases everything through Shutdown, then destroys the URLs, the timer, the dirty-bitmap string,
    // the lock and the draw context's inline-element list / margin stacks (all members here).
    Shutdown();
}

bool cGZWinHTML::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x100567392
    if (out == nullptr) return false;
    if (iid == 1 || iid == kGZIID_cIGZWinHTML) {
        *out = static_cast<cIGZWinHTML*>(this);
        AddRef();
        return true;
    }
    if (iid == kGZIID_cIGZWin) {
        *out = static_cast<cIGZWin*>(this);
        AddRef();
        return true;
    }
    if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return cGZWin::QueryInterface(iid, out);
}

bool cGZWinHTML::Init() {   // @ 0x100566e9c
    if (get_init_flag()) return true;
    const bool baseInit = cGZWin::Init();

    // The cursors come from the window manager's cursor manager: the hand (0x12, falling back to 0x11 and then to the
    // arrow) and the arrow. The original dereferences the manager unconditionally; the port tolerates a missing one.
    cIGZCursorManager* cursors = GetWindowManager() != nullptr ? GetWindowManager()->GetCursorManager() : nullptr;
    if (cursors != nullptr) {
        linkCursor_ = cursors->LoadCursor(0x12);
        if (linkCursor_ == nullptr) linkCursor_ = cursors->LoadCursor(0x11);
        if (linkCursor_ == nullptr) linkCursor_ = cursors->LoadCursor(1);
        if (linkCursor_ != nullptr) linkCursor_->AddRef();
        arrowCursor_ = cursors->LoadCursor(1);
        if (arrowCursor_ != nullptr) arrowCursor_->AddRef();
    }
    missingBitmapPath_ = cRZString("Missing.jpg");
    if (imageDecoder_ == nullptr) imageDecoder_ = WinHtmlHost()->Images();
    SetCaption(cRZString("HTML Window"));
    if (webSystem_ == nullptr) webSystem_ = WinHtmlHost()->WebSystem();

    drawContext_.graphics = this;   // the original stores the window itself; its MakeColor is cGZWin::MakeFillColor
    IWinHtmlCanvas* canvas = WinHtmlHost()->Canvas(static_cast<IWinBuffer*>(GetBufferToDrawTo()));
    canvas_ = canvas;
    drawContext_.surface = canvas_;   // the original stores the draw buffer (+0x98)
    SetHTMLPaintArea();
    if (!url_.GetFullURL().str().empty() && SetURL(url_, false)) {
        // The original resets the scroll rectangle (the same inlined block as in InternalSetDocument).
        drawContext_.documentRect = cRZRect{0, 0, drawContext_.screenRect.right - drawContext_.screenRect.left,
                                            drawContext_.screenRect.bottom - drawContext_.screenRect.top};
    }
    return baseInit;
}

bool cGZWinHTML::Shutdown() {   // @ 0x100567248
    if (!get_init_flag()) return true;
    CancelCurrentLoadRequest();
    timer_.Stop();
    // The original releases the image decoder (GZCOM), the WWW system (cRZSysServPtr), the two cursors, the document,
    // the two scrollbars and the two scrollbar images, in this order. The decoder / web system / canvas are borrowed
    // from the host here, so they are only cleared.
    imageDecoder_ = nullptr;
    webSystem_ = nullptr;
    if (linkCursor_ != nullptr) {
        linkCursor_->Release();
        linkCursor_ = nullptr;
    }
    if (arrowCursor_ != nullptr) {
        arrowCursor_->Release();
        arrowCursor_ = nullptr;
    }
    if (document_ != nullptr) {
        document_->Shutdown();
        document_->Release();
        document_ = nullptr;
    }
    if (vScroll_ != nullptr) {
        vScroll_->Release();
        vScroll_ = nullptr;
    }
    if (hScroll_ != nullptr) {
        hScroll_->Release();
        hScroll_ = nullptr;
    }
    if (vScrollImage_ != nullptr) {
        vScrollImage_->Release();
        vScrollImage_ = nullptr;
    }
    if (hScrollImage_ != nullptr) {
        hScrollImage_->Release();
        hScrollImage_ = nullptr;
    }
    canvas_ = nullptr;
    drawContext_.surface = nullptr;
    return cGZWin::Shutdown();
}

bool cGZWinHTML::SetHTMLPaintArea() {   // @ 0x10056718e
    const cRZRect old = drawContext_.screenRect;
    drawContext_.screenRect = drawArea_;
    drawContext_.screenRect.left += gutterLeft_;
    drawContext_.screenRect.top += gutterTop_;
    drawContext_.screenRect.right -= gutterRight_;
    drawContext_.screenRect.bottom -= gutterBottom_;
    // The document rect keeps its top-left (the scroll position); its size becomes the screen rect's size.
    drawContext_.documentRect.right =
        (drawContext_.screenRect.right - drawContext_.screenRect.left) + drawContext_.documentRect.left;
    drawContext_.documentRect.bottom =
        (drawContext_.screenRect.bottom - drawContext_.screenRect.top) + drawContext_.documentRect.top;
    return old.left != drawContext_.screenRect.left || old.top != drawContext_.screenRect.top ||
           old.right != drawContext_.screenRect.right || old.bottom != drawContext_.screenRect.bottom;
}

bool cGZWinHTML::CancelCurrentLoadRequest() {   // @ 0x100567406
    const std::int32_t previous = loadStatus_;
    if (previous == kHtmlLoadLoading) {
        // Port: the facade's job is cancelled too, so it never reports to a window that moved on or shut down.
        if (webSystem_ != nullptr && pageJob_ != 0) webSystem_->CancelJob(pageJob_);
        pageJob_ = 0;
        loadStatus_ = kHtmlLoadIdle;
        cGZMessage message{kWinHtmlMsgLoadStatus, reinterpret_cast<std::uint64_t>(this), 0, 1};
        SendMsg(notify_, message);
    }
    return previous != kHtmlLoadLoading;
}

bool cGZWinHTML::DoLinkJump(const cIGZString& link) {   // @ 0x1005674b6
    auto* url = new cRZString(link);
    url->AddRef();
    cGZMessage message{kWinHtmlMsgLinkJump, reinterpret_cast<std::uint64_t>(this),
                       reinterpret_cast<std::uint64_t>(url), 0};
    const bool result = SendMsg(notify_, message);
    if (result && (flags_ & kHtmlFollowLinks) != 0) SetURL(static_cast<const cRZString&>(*url), false);
    url->Release();
    return result;
}

void cGZWinHTML::SetFlag(int flag) {   // @ 0x10056761e
    if ((flags_ & flag) != 0) return;
    SetFlags(flags_ | flag);
}

void cGZWinHTML::ClearFlag(int flag) {   // @ 0x100567668
    if ((flags_ & flag) == 0) return;
    SetFlags(flags_ & ~flag);
}

void cGZWinHTML::SetFlags(int flags) {   // @ 0x1005676d6
    if (flags_ != flags) flags_ = flags;
}

void cGZWinHTML::GetGutters(int& left, int& top, int& right, int& bottom) {   // @ 0x100567716
    left = gutterLeft_;
    top = gutterTop_;
    right = gutterRight_;
    bottom = gutterBottom_;
}

void cGZWinHTML::SetGutters(int left, int top, int right, int bottom) {   // @ 0x100567766
    gutterLeft_ = left;
    gutterTop_ = top;
    gutterRight_ = right;
    gutterBottom_ = bottom;
    SetHTMLPaintArea();
    if (document_ != nullptr) {
        document_->MarkPageLayoutStatus(false);
        InvalidateSelfAndParents();
    }
}

void cGZWinHTML::SetTextSizeTable(const std::uint32_t* sizes) {   // @ 0x1005677da
    for (int i = 0; i < 7; ++i) textSizeTable_[i] = sizes[i];
}

bool cGZWinHTML::DoMessage(cIGZMessage2* message) {   // @ 0x10056783a
    if (message == nullptr) return false;
    if (message->GetType() != kWinHtmlMsgDownloaded) return false;
    // The payload is read through the standard-message slots without checking the message's class (as the original).
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const bool succeeded = standard->GetData2() != 0;
    (void)standard->GetVoid1();   // read and discarded by the original
    // The original reads the finished job from GetVoid3: a cIGZWWWSystem::Job whose +0x40 is the "is a file" flag and
    // whose +0x48 body text is the page. The port uses the recovered document's download record for that pair.
    auto* job = static_cast<const html::tDownloadedJob*>(standard->GetVoid3());
    return OnDownloaded(succeeded, job);
}

void cGZWinHTML::Downloaded(bool succeeded, const html::tDownloadedJob& job) {
    pageJob_ = 0;
    OnDownloaded(succeeded, &job);
}

bool cGZWinHTML::OnDownloaded(bool succeeded, const html::tDownloadedJob* job) {
    if (!succeeded) {
        if (loadStatus_ != kHtmlLoadLoading) return true;
        loadStatus_ = document_ != nullptr ? kHtmlLoadLoaded : kHtmlLoadIdle;
        cGZMessage notification{kWinHtmlMsgLoadStatus, reinterpret_cast<std::uint64_t>(this),
                                static_cast<std::uint64_t>(loadStatus_), 1};
        SendMsg(notify_, notification);
    } else {
        if (loadStatus_ != kHtmlLoadLoading) return true;
        if (job != nullptr && !job->data.empty()) {
            if (!InternalSetDocument(job->data, newUrl_.GetFullURL())) return true;
        }
        loadStatus_ = kHtmlLoadLoaded;
        cGZMessage notification{kWinHtmlMsgLoadStatus, reinterpret_cast<std::uint64_t>(this), 2, 1};
        SendMsg(notify_, notification);
    }
    InvalidateSelf();
    return true;
}

bool cGZWinHTML::InternalSetDocument(const std::string& text, const cRZString& baseUrl) {   // @ 0x10056796c
    // The document's services: the recovered cHTMLDocument reaches the framework through HtmlServices.
    html::HtmlServices services;
    services.graphics = this;
    services.fonts = WinHtmlHost()->Fonts();
    services.images = imageDecoder_;
    services.resolveUrl = ResolveHtmlUrl;
    services.makeTempPath = MakeHtmlTempPath;
    services.createBullet = [](int size, html::Color background, html::Color foreground, bool disc) {
        return WinHtmlHost()->CreateBullet(size, background, foreground, disc);
    };

    auto* document = new html::cHTMLDocument(services);
    document->AddRef();
    document->Init(this, webSystem_, baseUrl);
    if (textSizeTable_[0] != 0) document->SetFontSizeTable(textSizeTable_, 7);
    cRZString documentText;
    documentText.AssignFromChars(text.data(), static_cast<std::uint32_t>(text.size()));
    if (!document->ParseSGMLElements(documentText)) {
        document->Release();
        return false;
    }
    lock_.Lock();
    // A blinking page needs the 500 ms redraw timer; a page without blinking stops it. The original tests the timer's
    // running byte (+0x300) directly.
    if (!document->IsBlinkingPresent()) {
        if (timer_.IsRunning()) timer_.Stop();
    } else if (!timer_.IsRunning()) {
        timer_.SetCallbackFunction(PeriodicWindowRedrawCallback, this);
        timer_.StartPeriodic(500, nullptr);
    }
    SetBaseURL(baseUrl);
    drawContext_.documentRect = cRZRect{0, 0, drawContext_.screenRect.right - drawContext_.screenRect.left,
                                        drawContext_.screenRect.bottom - drawContext_.screenRect.top};
    if (document_ != nullptr) {
        document_->Shutdown();
        document_->Release();
        document_ = nullptr;
    }
    document_ = document;
    drawContext_.window = this;
    url_ = newUrl_;
    RemoveScrollBar(true, true);
    lock_.Unlock();
    return true;
}

bool cGZWinHTML::GZOnCommand(std::uint32_t command, std::uint64_t data) {   // @ 0x100567ba4
    if (command != kWinHtmlCmdScrollbarChanged) return true;
    if (vScroll_ != nullptr &&
        static_cast<std::uint64_t>(vScroll_->AsIGZWin()->GetID()) == data) {
        const std::int32_t height = drawContext_.documentRect.bottom - drawContext_.documentRect.top;
        drawContext_.documentRect.top = vScroll_->GetValue();
        drawContext_.documentRect.bottom = height + drawContext_.documentRect.top;
        InvalidateSelfAndParents();
        return true;
    }
    if (hScroll_ != nullptr &&
        static_cast<std::uint64_t>(hScroll_->AsIGZWin()->GetID()) == data) {
        const std::int32_t width = drawContext_.documentRect.right - drawContext_.documentRect.left;
        drawContext_.documentRect.left = hScroll_->GetValue();
        drawContext_.documentRect.right = width + drawContext_.documentRect.left;
        InvalidateSelfAndParents();
    }
    return true;
}

bool cGZWinHTML::GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) {   // @ 0x100567c72
    // The target sees every key first and can claim it (message 0xe4a85d8d: window, key, modifiers).
    cGZMessage notification{kWinHtmlMsgKeyDown, reinterpret_cast<std::uint64_t>(this), key, modifiers};
    if (SendMsg(notify_, notification)) return true;
    if (document_ == nullptr) return true;
    html::cHTMLBODY* body = document_->Body();
    if (body == nullptr) return true;

    const std::int32_t pageHeight =
        ((drawContext_.screenRect.bottom - drawContext_.screenRect.top) * 9) / 10;
    const std::int32_t lineHeight = (drawContext_.screenRect.bottom - drawContext_.screenRect.top) / 25;
    const std::int32_t lineWidth = (drawContext_.screenRect.right - drawContext_.screenRect.left) / 25;
    switch (key) {
        case 0x21: {   // Page Up
            if ((flags_ & kHtmlKeyboardScroll) == 0) return true;
            if (drawContext_.documentRect.top < 1) return true;
            const std::int32_t height =
                drawContext_.documentRect.bottom - drawContext_.documentRect.top;
            drawContext_.documentRect.top -= pageHeight;
            drawContext_.documentRect.bottom = drawContext_.documentRect.top + height;
            if (drawContext_.documentRect.top < 0) {
                drawContext_.documentRect.top = 0;
                drawContext_.documentRect.bottom = height;
            }
            break;
        }
        case 0x22: {   // Page Down
            if ((flags_ & kHtmlKeyboardScroll) == 0) return true;
            if (drawContext_.documentRect.bottom >= body->drawRect().bottom) return true;
            const std::int32_t height =
                drawContext_.documentRect.bottom - drawContext_.documentRect.top;
            const std::int32_t oldTop = drawContext_.documentRect.top;
            drawContext_.documentRect.top = oldTop + pageHeight;
            drawContext_.documentRect.bottom = drawContext_.documentRect.bottom + pageHeight;
            if (drawContext_.documentRect.bottom > body->drawRect().bottom) {
                drawContext_.documentRect.top = body->drawRect().bottom - height;
                drawContext_.documentRect.bottom = body->drawRect().bottom;
            }
            break;
        }
        case 0x23: {   // End
            if ((flags_ & kHtmlKeyboardScroll) == 0) return true;
            const std::int32_t documentHeight = body->drawRect().bottom;
            if (drawContext_.documentRect.bottom == documentHeight) return true;
            const std::int32_t height =
                drawContext_.documentRect.bottom - drawContext_.documentRect.top;
            drawContext_.documentRect.top = documentHeight - height;
            drawContext_.documentRect.bottom = documentHeight;
            drawContext_.documentRect.left = 0;
            // Quirk kept: the original stores the *height* into the visible right edge (0x100567e07).
            drawContext_.documentRect.right = height;
            if (documentHeight - height < 0) {
                drawContext_.documentRect.top = 0;
                drawContext_.documentRect.bottom = height;
            }
            break;
        }
        case 0x24: {   // Home
            if ((flags_ & kHtmlKeyboardScroll) == 0) return true;
            if (drawContext_.documentRect.top == 0 && drawContext_.documentRect.left == 0) return true;
            drawContext_.documentRect.bottom -= drawContext_.documentRect.top;
            drawContext_.documentRect.top = 0;
            drawContext_.documentRect.right -= drawContext_.documentRect.left;
            drawContext_.documentRect.left = 0;
            break;
        }
        case 0x25: {   // Left
            if ((flags_ & kHtmlKeyboardScroll) == 0) return true;
            if (drawContext_.documentRect.left < 1) return true;
            const std::int32_t width =
                drawContext_.documentRect.right - drawContext_.documentRect.left;
            drawContext_.documentRect.left -= lineWidth;
            drawContext_.documentRect.right = drawContext_.documentRect.left + width;
            if (drawContext_.documentRect.left < 0) {
                drawContext_.documentRect.left = 0;
                drawContext_.documentRect.right = width;
            }
            break;
        }
        case 0x26: {   // Up
            if ((flags_ & kHtmlKeyboardScroll) == 0) return true;
            if (drawContext_.documentRect.top < 1) return true;
            const std::int32_t height =
                drawContext_.documentRect.bottom - drawContext_.documentRect.top;
            drawContext_.documentRect.top -= lineHeight;
            drawContext_.documentRect.bottom = drawContext_.documentRect.top + height;
            if (drawContext_.documentRect.top < 0) {
                drawContext_.documentRect.top = 0;
                drawContext_.documentRect.bottom = height;
            }
            break;
        }
        case 0x27: {   // Right
            if ((flags_ & kHtmlKeyboardScroll) == 0) return true;
            if (drawContext_.documentRect.right >= body->drawRect().right) return true;
            const std::int32_t width =
                drawContext_.documentRect.right - drawContext_.documentRect.left;
            drawContext_.documentRect.left += lineWidth;
            drawContext_.documentRect.right += lineWidth;
            if (drawContext_.documentRect.right > body->drawRect().right) {
                drawContext_.documentRect.left = body->drawRect().right - width;
                drawContext_.documentRect.right = body->drawRect().right;
            }
            break;
        }
        case 0x28: {   // Down
            if ((flags_ & kHtmlKeyboardScroll) == 0) return true;
            if (drawContext_.documentRect.bottom >= body->drawRect().bottom) return true;
            const std::int32_t height =
                drawContext_.documentRect.bottom - drawContext_.documentRect.top;
            drawContext_.documentRect.top += lineHeight;
            drawContext_.documentRect.bottom += lineHeight;
            if (drawContext_.documentRect.bottom > body->drawRect().bottom) {
                drawContext_.documentRect.top = body->drawRect().bottom - height;
                drawContext_.documentRect.bottom = body->drawRect().bottom;
            }
            break;
        }
        default: {
            if (key != 0x52) return true;             // Ctrl+R: reload the current URL
            if ((modifiers & 7) != 2) return true;
            cRZString current;
            GetURL(current);
            SetURL(current, true);
            return true;
        }
    }
    SetScrollBarValues(true);
    InvalidateSelfAndParents();
    return true;
}

void cGZWinHTML::SetScrollBarValues(bool resetRange) {   // @ 0x100568100
    if (document_ == nullptr) return;
    html::cHTMLBODY* body = document_->Body();
    if (body == nullptr) return;
    if (vScroll_ != nullptr) {
        if (resetRange) {
            const std::int32_t documentHeight = body->drawRect().bottom;
            const std::int32_t visibleHeight =
                drawContext_.documentRect.bottom - drawContext_.documentRect.top;
            std::int32_t range = documentHeight - visibleHeight;
            if (range < 0) range = 0;
            vScroll_->SetMinMax(0, range);
            // The original divides by the visible height without a guard (a fault at 0); the port keeps the signed
            // math but answers one page for an empty viewport.
            const std::uint32_t pages = visibleHeight != 0
                                            ? static_cast<std::uint32_t>(documentHeight / visibleHeight + 1)
                                            : 1u;
            vScroll_->SetLineAndPageCount(static_cast<std::uint32_t>(documentHeight / 0x14), pages);
        }
        vScroll_->SetValue(drawContext_.documentRect.top);
    }
    if (hScroll_ != nullptr) {
        if (resetRange) {
            const std::int32_t documentWidth = body->drawRect().right;
            const std::int32_t visibleWidth =
                drawContext_.documentRect.right - drawContext_.documentRect.left;
            std::int32_t range = documentWidth - visibleWidth;
            if (range < 0) range = 0;
            // Quirk kept: the horizontal range, line/page counts and value all go into the *vertical* scrollbar
            // (0x100568225..0x10056824e). The original faults when a horizontal bar exists without a vertical one.
            if (vScroll_ != nullptr) {
                vScroll_->SetMinMax(0, range);
                const std::uint32_t pages = visibleWidth != 0
                                                ? static_cast<std::uint32_t>(documentWidth / visibleWidth + 1)
                                                : 1u;
                vScroll_->SetLineAndPageCount(static_cast<std::uint32_t>(documentWidth / 0xf), pages);
                vScroll_->SetValue(drawContext_.documentRect.left);
            }
        }
    }
}

bool cGZWinHTML::GZOnMouseDownL(int x, int y, std::uint32_t) {   // @ 0x100568270
    AddRef();
    lock_.Lock();
    html::cHTMLElement* hit = nullptr;
    if (PickElement(hit, x, y)) hit->OnMouseDownL(drawContext_);
    lock_.Unlock();
    Release();
    return true;
}

bool cGZWinHTML::PickElement(html::cHTMLElement*& hit, int x, int y) {   // @ 0x10056831c
    if (document_ == nullptr) return false;
    html::cHTMLBODY* body = document_->Body();
    if (body == nullptr || !body->laidOut()) return false;
    if (!IsPointInWindowWindowCoordinates(x, y)) return false;
    const int documentX = (x + drawContext_.documentRect.left) - drawContext_.screenRect.left;
    const int documentY = (y + drawContext_.documentRect.top) - drawContext_.screenRect.top;
    return body->Pick(hit, documentX, documentY) == 2;
}

bool cGZWinHTML::GZOnMouseUpL(int x, int y, std::uint32_t) {   // @ 0x1005683b6
    AddRef();
    lock_.Lock();
    html::cHTMLElement* hit = nullptr;
    if (PickElement(hit, x, y)) {
        hit->OnMouseUpL(drawContext_);
        if (hit->type() == html::kTypeAnchor) {
            auto* anchor = static_cast<html::cHTMLAnchor*>(hit);
            const cRZString* href = anchor->hrefAbsolute() != nullptr ? anchor->hrefAbsolute() : anchor->href();
            if (href != nullptr) DoLinkJump(*href);
        }
    }
    lock_.Unlock();
    Release();
    return true;
}

bool cGZWinHTML::GZOnMouseMove(int x, int y, std::uint32_t) {   // @ 0x10056848c
    lock_.Lock();
    if ((flags_ & kHtmlFollowLinks) != 0) {
        html::cHTMLElement* hit = nullptr;
        bool overLink = false;
        if (PickElement(hit, x, y) && hit->type() == html::kTypeAnchor) {
            auto* anchor = static_cast<html::cHTMLAnchor*>(hit);
            overLink = anchor->href() != nullptr || anchor->hrefAbsolute() != nullptr;
        }
        SetCursor(overLink ? linkCursor_ : arrowCursor_, true);
    }
    lock_.Unlock();
    return true;
}

bool cGZWinHTML::GZOnMouseWheel(int, int, std::uint32_t, int delta) {   // @ 0x100568556
    if (GZAmCapturing()) return true;
    // Quirk kept: the flag word is read before the key repeats and written back afterwards, so a flag cleared by
    // AddScrollBar during a repeat is resurrected (0x100568574 / 0x1005685f5).
    const std::int32_t savedFlags = flags_;
    if (delta > 99) {
        for (int i = 0; i < delta / 100; ++i) GZOnKeyDown(0x26, 0);
    } else if (delta < -99) {
        const int count = (-delta) / 100;
        for (int i = 0; i < count; ++i) GZOnKeyDown(0x28, 0);
    }
    flags_ = savedFlags | kHtmlKeyboardScroll;
    return true;
}

bool cGZWinHTML::GZPaint() {   // @ 0x100568608
    // The surface is the window's current draw buffer (+0x98); rebind it whenever the buffer changed.
    IWinHtmlCanvas* canvas = WinHtmlHost()->Canvas(static_cast<IWinBuffer*>(GetBufferToDrawTo()));
    if (canvas != canvas_) {
        canvas_ = canvas;
        drawContext_.surface = canvas_;
    }
    if (loadStatus_ != kHtmlLoadLoaded) return true;

    html::cHTMLBODY* body = document_ != nullptr ? document_->Body() : nullptr;
    if (body == nullptr) {
        if ((flags_ & kHtmlFillBackground) != 0 && canvas_ != nullptr && canvas_->Begin(html::kSurfaceFill)) {
            canvas_->FillRect(GetFillColor(), drawArea_, nullptr);
            canvas_->End(html::kSurfaceFill);
        }
        return true;
    }
    if (SetHTMLPaintArea()) document_->MarkPageLayoutStatus(false);
    if (body->hasBackgroundColor) SetFillColorColor(body->backgroundColor);
    if (body->background() != nullptr) {
        html::IHtmlBitmap* image = body->background();
        if (canvas_ != nullptr && canvas_->Begin(html::kSurfaceBlit)) {
            int offsetY = 0;
            if (!body->fixedBackground) {
                const int height = image->Height();
                // offsetY = height - scrollY % height (signed remainder, as the original's idivl).
                offsetY = height - (drawContext_.documentRect.top % height);
            }
            canvas_->BltTiled(image, image->Bounds(), drawArea_, 0, offsetY);
            canvas_->End(html::kSurfaceBlit);
        }
    } else if ((flags_ & kHtmlFillBackground) != 0 && canvas_ != nullptr && canvas_->Begin(html::kSurfaceFill)) {
        canvas_->FillRect(GetFillColor(), drawArea_, nullptr);
        canvas_->End(html::kSurfaceFill);
    }
    if ((flags_ & kHtmlOutline) != 0 && canvas_ != nullptr && canvas_->Begin(kSurfaceOutline)) {
        canvas_->Outline(drawArea_, outlineColor_);
        canvas_->End(kSurfaceOutline);
    }
    if (!document_->IsPageLayoutDone()) {
        drawContext_.InitForFreshPainting();
        body->Layout(drawContext_);
        body->PostLayout();
        document_->MarkPageLayoutStatus(true);
        TestForPossibleScrollBarAdditionOrRemoval();
    }
    drawContext_.InitForFreshPainting();
    body->Draw(drawContext_);
    return true;
}

bool cGZWinHTML::TestForPossibleScrollBarAdditionOrRemoval() {   // @ 0x1005688e8
    if (document_ == nullptr) return true;
    html::cHTMLBODY* body = document_->Body();
    if (body == nullptr) return true;
    bool removeVertical = false;
    bool addVertical = false;
    if ((flags_ & kHtmlVScroll) != 0) {
        const std::int32_t visibleHeight =
            drawContext_.documentRect.bottom - drawContext_.documentRect.top;
        removeVertical = vScroll_ != nullptr && body->drawRect().bottom <= visibleHeight;
        addVertical = vScroll_ == nullptr && visibleHeight < body->drawRect().bottom;
    }
    bool removeHorizontal = false;
    bool addHorizontal = false;
    if ((flags_ & kHtmlHScroll) != 0) {
        const std::int32_t visibleWidth =
            drawContext_.documentRect.right - drawContext_.documentRect.left;
        if (visibleWidth < body->drawRect().right) {
            addHorizontal = hScroll_ == nullptr;
        } else {
            removeHorizontal = hScroll_ != nullptr;
        }
    }
    if (removeHorizontal || removeVertical) RemoveScrollBar(removeVertical, removeHorizontal);
    if (addHorizontal || addVertical) AddScrollBar(addVertical, addHorizontal);
    return true;
}

bool cGZWinHTML::SetWWWSystem(html::IHtmlWebSystem* web) {   // @ 0x1005689d6
    if (webSystem_ != web) {
        // The original holds a cRZSysServPtr (AddRef the new system, Release the old). The recovered html::IHtmlWebSystem
        // is a plain interface without reference counting, so the setter only swaps the borrowed pointer.
        webSystem_ = web;
    }
    return true;
}

bool cGZWinHTML::SetBaseURL(const cRZString& url) {   // @ 0x100568a66
    baseUrl_ = cRZURL(url);
    return true;
}

void cGZWinHTML::SetFillColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {   // @ 0x100568ae6
    cGZWin::SetFillColor(r, g, b);
    drawContext_.backgroundColor = MakeFillColor(r, g, b);   // +0x1a8
}

void cGZWinHTML::SetArea(int l, int t, int r, int b) {   // @ 0x100568b26
    const std::int32_t oldWidth = area_.right - area_.left;
    const std::int32_t oldHeight = area_.bottom - area_.top;
    cGZWin::SetArea(l, t, r, b);
    SetHTMLPaintArea();
    if (oldWidth == area_.right - area_.left && oldHeight == area_.bottom - area_.top) {
        // Quirk kept: an unchanged size invalidates the parent, not this window.
        if (parent_ == nullptr) return;
        parent_->InvalidateSelfAndParents();
    } else {
        if (document_ == nullptr) return;
        document_->MarkPageLayoutStatus(false);
        InvalidateSelfAndParents();
    }
}

bool cGZWinHTML::SetRelativeURL(const cRZString& relative) {   // @ 0x100568bc4
    cRZString url = url_.GetFullURL();
    url.AppendString(relative);
    return SetURL(url, false);
}

bool cGZWinHTML::GetURL(cRZString& url) {   // @ 0x100568c9c
    url = url_.GetFullURL();
    return !url.str().empty();
}

bool cGZWinHTML::GetNewURL(cRZString& url) {   // @ 0x100568cee
    url = newUrl_.GetFullURL();
    return !url.str().empty();
}

bool cGZWinHTML::SetURL(const cRZString& url, bool reload) {   // @ 0x100568d40
    cRZURL parsed(url);
    return SetURL(parsed, reload);
}

bool cGZWinHTML::SetURL(cRZURL& url, bool reload) {   // @ 0x100568dca
    if (!get_init_flag() || webSystem_ == nullptr) return false;
    if (url.GetFullURL().str().empty()) return false;
    bool same = false;
    if (!reload && url_.GetFullURL().str() == url.GetFullURL().str()) {
        same = true;
    } else if (url_.GetFullURL().str() == url.GetFullURL().str()) {
        newUrl_ = url_;
        same = true;
    } else {
        newUrl_.ChangeURL(url.GetFullURL(), &baseUrl_);
    }
    if (loadStatus_ == kHtmlLoadLoading) {
        if (same) return true;
        CancelCurrentLoadRequest();
    }
    loadStatus_ = kHtmlLoadLoading;
    cGZMessage started{kWinHtmlMsgLoadStatus, reinterpret_cast<std::uint64_t>(this), 1, 1};
    SendMsg(notify_, started);
    // The original passes the URL, the +0x158 message target, two zeros and "resolve against the base URL" to the WWW
    // system's vslot 0x88; the facade takes the absolute URL and this window as the job's html::IHtmlDownloadTarget.
    pageJob_ = webSystem_->StartJob(newUrl_.GetFullURL().str(), this);
    if (pageJob_ == 0) {
        loadStatus_ = kHtmlLoadIdle;
        cGZMessage failed{kWinHtmlMsgLoadStatus, reinterpret_cast<std::uint64_t>(this), 0, 1};
        SendMsg(notify_, failed);
    }
    return true;
}

void cGZWinHTML::RemoveScrollBar(bool vertical, bool horizontal) {   // @ 0x1005690b2
    bool removed = false;
    if (vertical && vScroll_ != nullptr) {
        ChildDelete(vScroll_->AsIGZWin());
        vScroll_->Release();
        vScroll_ = nullptr;
        removed = true;
    }
    if (horizontal && hScroll_ != nullptr) {
        ChildDelete(hScroll_->AsIGZWin());
        hScroll_->Release();
        hScroll_ = nullptr;
    } else if (!removed) {
        return;
    }
    if (document_ != nullptr) {
        document_->MarkPageLayoutStatus(false);
        InvalidateSelfAndParents();
    }
}

void cGZWinHTML::SetMissingBitmapPath(const cIGZString& path) {   // @ 0x100569c36
    // +0x238: cRZString::operator=(const cIGZString&).
    missingBitmapPath_.AssignFromIGZString(path);
}

bool cGZWinHTML::GetDocument(std::string& out) {   // @ 0x10056918a
    // The original dereferences the document without a check; the port answers false when there is none.
    if (document_ == nullptr) return false;
    const cRZString& text = document_->GetDocument();
    out.assign(text.Data(), text.Strlen());
    return true;
}

bool cGZWinHTML::SetDocument(const std::string& text, const cRZString& baseUrl) {   // @ 0x100569222
    if (!InternalSetDocument(text, baseUrl)) return false;
    if (loadStatus_ != kHtmlLoadLoaded) {
        loadStatus_ = kHtmlLoadLoaded;
        cGZMessage notification{kWinHtmlMsgLoadStatus, reinterpret_cast<std::uint64_t>(this), 2, 1};
        SendMsg(notify_, notification);
    }
    InvalidateSelfAndParents();
    return true;
}

void cGZWinHTML::GetScrollPosition(int& x, int& y) {   // @ 0x1005692a2
    x = drawContext_.documentRect.left;
    y = drawContext_.documentRect.top;
}

void cGZWinHTML::SetScrollPosition(int x, int y) {   // @ 0x1005692c8
    gz::cRZSize size;
    GetDocumentSize(size);   // side effect: the original lays the page out and tests the bars here too
    if (document_ == nullptr) return;
    html::cHTMLBODY* body = document_->Body();
    if (body == nullptr) return;
    int currentX = 0;
    int currentY = 0;
    GetScrollPosition(currentX, currentY);
    const std::int32_t visibleWidth = drawContext_.documentRect.right - drawContext_.documentRect.left;
    const std::int32_t visibleHeight = drawContext_.documentRect.bottom - drawContext_.documentRect.top;
    std::int32_t newX = body->drawRect().right - visibleWidth;
    if (x < newX) newX = x;
    if (newX < 0) newX = 0;
    std::int32_t newY = body->drawRect().bottom - visibleHeight;
    if (y < newY) newY = y;
    if (newY < 0) newY = 0;
    if (currentX != newX || currentY != newY) {
        drawContext_.documentRect.left = newX;
        drawContext_.documentRect.top = newY;
        drawContext_.documentRect.right = visibleWidth + newX;
        drawContext_.documentRect.bottom = visibleHeight + newY;
        if (hScroll_ != nullptr) hScroll_->SetValue(newX);
        if (vScroll_ != nullptr) vScroll_->SetValue(newY);
        InvalidateSelfAndParents();
    }
}

bool cGZWinHTML::GetAnchorPosition(const cIGZString& name, cRZRect& rect) {   // @ 0x1005693d6
    if (document_ == nullptr) return false;
    html::cHTMLAnchor* anchor = document_->FindAnchor(name);
    if (anchor == nullptr || !anchor->laidOut()) return false;
    rect = anchor->drawRect();
    return true;
}

bool cGZWinHTML::ScrollToAnchor(const cIGZString& name) {   // @ 0x100569452
    if (document_ == nullptr) return false;
    html::cHTMLAnchor* anchor = document_->FindAnchor(name);
    if (anchor == nullptr || !anchor->laidOut()) return false;
    const cRZRect& target = anchor->drawRect();
    int x = 0;
    int y = 0;
    GetScrollPosition(x, y);
    const std::int32_t viewWidth = (area_.right - area_.left) - gutterLeft_ - gutterRight_;
    const std::int32_t viewHeight = (area_.bottom - area_.top) - gutterTop_ - gutterBottom_;
    const std::int32_t visibleRight = viewWidth + x;
    if (visibleRight < target.right) x = visibleRight - target.right + target.left;
    if (target.left < x) x = target.left;
    const std::int32_t visibleBottom = viewHeight + y;
    if (visibleBottom < target.bottom) y = visibleBottom - target.bottom + target.top;
    if (target.top < y) y = target.top;
    SetScrollPosition(x, y);
    return true;
}

void cGZWinHTML::GetDocumentSize(gz::cRZSize& size) {   // @ 0x10056956e
    html::cHTMLBODY* body = document_ != nullptr ? document_->Body() : nullptr;
    if (body == nullptr) {
        size = gz::cRZSize{};
        return;
    }
    if (!document_->IsPageLayoutDone()) {
        drawContext_.InitForFreshPainting();
        body->Layout(drawContext_);
        body->PostLayout();
        document_->MarkPageLayoutStatus(true);
        TestForPossibleScrollBarAdditionOrRemoval();
    }
    size.width = body->drawRect().right;
    size.height = body->drawRect().bottom;
}

bool cGZWinHTML::SetScrollBarImageVertical(cIGZBuffer* image, const cRZRect& area) {   // @ 0x10056961a
    if (image != nullptr) image->AddRef();
    if (vScrollImage_ != nullptr) vScrollImage_->Release();
    vScrollImage_ = image;
    vScrollImageRect_ = area;
    if (image == nullptr || vScroll_ == nullptr) return true;
    cRZRect use = area;
    if (!(use.left < use.right && use.top < use.bottom)) use = *image->GetBufferArea();
    vScroll_->ImageSetup(use, image, 0);
    return true;
}

bool cGZWinHTML::GetScrollBarImageVertical(cIGZBuffer*& image, cRZRect& area) const {   // @ 0x1005696fa
    image = vScrollImage_;
    area = vScrollImageRect_;
    return image != nullptr;
}

bool cGZWinHTML::SetScrollBarImageHorizontal(cIGZBuffer* image, const cRZRect& area) {   // @ 0x100569752
    if (image != nullptr) image->AddRef();
    if (hScrollImage_ != nullptr) hScrollImage_->Release();
    hScrollImage_ = image;
    hScrollImageRect_ = area;
    if (image == nullptr || hScroll_ == nullptr) return true;
    cRZRect use = area;
    if (!(use.left < use.right && use.top < use.bottom)) use = *image->GetBufferArea();
    hScroll_->ImageSetup(use, image, 0);
    return true;
}

bool cGZWinHTML::GetScrollBarImageHorizontal(cIGZBuffer*& image, cRZRect& area) const {   // @ 0x100569832
    image = hScrollImage_;
    area = hScrollImageRect_;
    return image != nullptr;
}

bool cGZWinHTML::AddScrollBar(bool vertical, bool horizontal) {   // @ 0x10056988a
    bool added = false;
    if (vertical && (flags_ & kHtmlVScroll) != 0 && vScroll_ == nullptr) {
        gz::IWinCtrlFactory* factory = gz::WinCtrlFactory();
        vScroll_ = factory != nullptr ? factory->CreateScrollbar(0, 2, 0, 100, 0) : nullptr;
        if (vScroll_ == nullptr) {
            ClearFlag(kHtmlVScroll);
        } else {
            if (vScrollImage_ != nullptr) {
                cRZRect area = vScrollImageRect_;
                if (!(area.left < area.right && area.top < area.bottom)) area = *vScrollImage_->GetBufferArea();
                vScroll_->ImageSetup(area, vScrollImage_, 0);
            }
            ChildAdd(vScroll_->AsIGZWin());
        }
        added = true;   // a failed creation still counts as an attempt in the original
    }
    if (horizontal && (flags_ & kHtmlHScroll) != 0 && hScroll_ == nullptr) {
        gz::IWinCtrlFactory* factory = gz::WinCtrlFactory();
        hScroll_ = factory != nullptr ? factory->CreateScrollbar(1, 1, 0, 100, 0) : nullptr;
        if (hScroll_ == nullptr) {
            ClearFlag(kHtmlHScroll);
        } else {
            if (hScrollImage_ != nullptr) {
                cRZRect area = hScrollImageRect_;
                if (!(area.left < area.right && area.top < area.bottom)) area = *hScrollImage_->GetBufferArea();
                hScroll_->ImageSetup(area, hScrollImage_, 0);
            }
            ChildAdd(hScroll_->AsIGZWin());
        }
    } else if (!added) {
        return true;
    }
    if (document_ != nullptr) {
        document_->MarkPageLayoutStatus(false);
        InvalidateSelfAndParents();
    }
    SetScrollBarSizes();
    SetScrollBarValues(true);
    return true;
}

void cGZWinHTML::SetScrollBarSizes() {   // @ 0x100569ae2
    if (vScroll_ != nullptr) {
        cIGZWin* bar = vScroll_->AsIGZWin();
        bar->SetArea((GetW() + 1) - bar->GetW(), 0, GetW(), GetH());
    }
    if (hScroll_ != nullptr) {
        cIGZWin* bar = hScroll_->AsIGZWin();
        bar->SetArea(0, GetH() - bar->GetH(), GetW(), GetH());
    }
}

// ---- html::IHtmlWindow / html::IHtmlGraphics ---------------------------------------------------------

html::cHTMLBODY* cGZWinHTML::Body() { return document_ != nullptr ? document_->Body() : nullptr; }

html::Color cGZWinHTML::DefaultActiveLinkColor() {
    return document_ != nullptr ? document_->GetActiveLinkColor() : 0;
}

html::Color cGZWinHTML::DefaultVisitedLinkColor() {
    return document_ != nullptr ? document_->GetVisitedLinkColor() : 0;
}

void cGZWinHTML::Invalidate() { InvalidateSelfAndParents(); }

html::Color cGZWinHTML::MakeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return MakeFillColor(r, g, b);
}

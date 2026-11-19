#include "sc4/WinSnapshot.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

#include "gz/Buffer.h"
#include "gz/DrawContext.h"
#include "gz/WinBtn.h"
#include "gz/WinFont.h"
#include "gz/WinGen.h"
#include "gz/WinListBox.h"
#include "gz/WinText.h"
#include "gz/WinTextEdit.h"
#include "rz/PathUtils.h"
#include "rz/TextEncoding.h"
#include "rz/cRZDirectory.h"
#include "rz/cRZFile.h"
#include "rz/cRZKeyboard.h"
#include "sc4/SC4Utilities.h"

namespace sc4 {
namespace {

ISnapshotHost* g_host = nullptr;

bool Trace() {
    static const bool on = std::getenv("SC4_SNAPSHOTDEBUG") != nullptr;
    return on;
}

constexpr std::uint32_t kStringGroup = 0x6a231eaa;
constexpr std::uint32_t kUIScriptGroup = 0x96a006b0;
constexpr std::uint32_t kImageType = 0x856ddbac;
constexpr std::uint32_t kWindowGenIid = 0x5386d516;
constexpr std::uint32_t kMsgCommand = 3, kCommandButton = 0x287259f6, kCommandListSelect = 0x08710e0f;
constexpr std::uint32_t kMsgWinProcShutdown = 0xa2bf8ad1, kMsgWinProcMinMax = 0xa2bf8ad2, kMsgWinProcInit = 0xa2bf8ad3;
constexpr std::uint32_t kWinFlagModalDialog = 0x1000;
constexpr const char kSanityCheck[] = "SanityCheck";
constexpr const char kAlbumNameFormat[] = "%s--%u";
constexpr const char kAlbumSeparator[] = "--";
constexpr const char kAlbumIni[] = "album.ini";

std::string Str(const cIGZString& s) { return std::string(s.Data(), s.Strlen()); }
cRZString Rz(const std::string& s) { return cRZString(s.c_str()); }

template <class T>
void Drop(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Splitpath: the path without its extension (directory and name), then a new extension.
std::string WithExtension(const cRZString& file, const char* extension) {
    std::string path = Str(file);
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) path.erase(dot);
    return path + extension;
}

std::string LoadText(std::uint32_t instance) {
    cRZString text;
    if (g_host) g_host->LoadString(kStringGroup, instance, text);
    return Str(text);
}

bool ReadWholeFile(const cRZString& path, std::string& out) {
    out.clear();
    cRZFile file{cRZString(path)};
    if (!file.Open(1, 2, 1)) return false;
    char chunk[0x200];
    for (;;) {
        std::uint32_t count = sizeof chunk;
        if (!file.ReadWithCount(chunk, count) || count == 0) break;
        out.append(chunk, count);
    }
    file.Close();
    return true;
}

bool WriteWholeFile(const cRZString& path, const std::string& data) {
    cRZFile file{cRZString(path)};
    if (!file.Open(2, 3, 1)) return false;
    file.SetLength(0);
    file.SeekToPosition(0);
    file.Write(data.data(), static_cast<std::uint32_t>(data.size()));
    file.Close();
    return true;
}

}  // namespace

ISnapshotHost* SnapshotHost() { return g_host; }
void SetSnapshotHost(ISnapshotHost* host) { g_host = host; }

// ==================================================================================================================
// cWinBMPViewGVExtra
// ==================================================================================================================

std::uint64_t (*cWinBMPViewGVExtra::clock)() = [] {
    return static_cast<std::uint64_t>(std::clock()) * 1000 / CLOCKS_PER_SEC;
};

cWinBMPViewGVExtra::cWinBMPViewGVExtra(cIGZWinMgr* mgr) : cWinBMPView(mgr) {}

bool cWinBMPViewGVExtra::Init() {
    const bool ok = cWinBMPView::Init();
    SetWinBMPFlag(2, true);
    SetSizeInOut(true);
    return ok;
}

bool cWinBMPViewGVExtra::SetSizeInOut(bool sizedIn) {
    if (sizedIn_ == sizedIn) return false;
    sizedIn_ = sizedIn;
    PullToFront();
    // Scroll Lock (0x91) held at "sized out" shows the picture at its own size even when it should fit.
    const bool fit = sizedIn_ || !cRZKeyboard::IsToggleKeySetStatic(0x91) ? fitAtSizeIn_ : false;
    SetShouldFitImageInView(fit);
    SetAlignment(kAlignCenter);
    EnableZooming(false);
    EnableScrolling(false);
    const cRZRect& area = sizedIn_ ? sizeInArea_ : sizeOutArea_;
    SetArea(area.left, area.top, area.right, area.bottom);
    SendMsg(GetNotificationTarget(), kSnapshotSizedInMessage, GetID(), sizedIn_ ? 1 : 0, 0);
    return true;
}

void cWinBMPViewGVExtra::SetShouldFitImageAtSizeIn(bool fit) {
    fitAtSizeIn_ = fit;
    if (!sizedIn_ && cRZKeyboard::IsToggleKeySetStatic(0x91)) SetShouldFitImageInView(false);
    else SetShouldFitImageInView(fit);
}

bool cWinBMPViewGVExtra::GZOnMouseDownL(int x, int y, std::uint32_t flags) {
    const std::uint64_t now = clock();
    if (!timerRunning_ || now - timerStart_ > 499) {   // not a double click: restart the timer
        timerRunning_ = true;
        timerStart_ = now;
        return cWinBMPView::GZOnMouseDownL(x, y, flags);
    }
    timerRunning_ = false;
    SetSizeInOut(!sizedIn_);
    return true;
}

// ==================================================================================================================
// cSC4WinSnapshotTaker
// ==================================================================================================================

cSC4WinSnapshotTaker::cSC4WinSnapshotTaker(cIGZWinMgr* mgr) : cGZWin(mgr) { SetFillColor(0xff, 0xff, 0xff); }

cSC4WinSnapshotTaker::~cSC4WinSnapshotTaker() { Shutdown(); }

bool cSC4WinSnapshotTaker::CreateWinSnapshotTaker(cIGZWin* parent, std::uint32_t size) {
    if (cIGZWin* existing = parent->GetChildWindowFromID(kSnapshotTakerId)) {
        existing->ShowWindow();
        existing->PullToFront();
        return true;
    }
    auto* taker = new cSC4WinSnapshotTaker(parent->GetWindowManager());
    taker->AddRef();
    parent->ChildAdd(taker);
    taker->PullToFront();
    if (size != 0) taker->GoToSizeIndex(size - 1);
    taker->Release();
    cGZMessage begin;   // the parent hears what a modal window would tell it (0x62ba7b42)
    begin.type = 0x62ba7b42;
    parent->DoMessage(begin);
    return true;
}

bool cSC4WinSnapshotTaker::Init() {
    if (initialized_) return true;
    SetID(kSnapshotTakerId);
    SetFlag(kWinPrivateBuffer, false);
    if (!font_ && g_host) {
        font_ = g_host->Font();
        if (font_) font_->AddRef();
    }
    if (sizes_.empty()) {
        cIGZWin* parent = GetParentWin();
        const int width = parent ? parent->GetW() : 640, height = parent ? parent->GetH() : 480;
        auto add = [this](int w, int h) { sizes_.push_back(cRZRect{0, 0, w, h}); };
        add(160, 120);
        add(320, 240);
        add(480, 360);
        if (width > 640) add(640, 480);
        if (width > 800) add(800, 600);
        if (width > 1024) add(1024, 768);
        if (width > 1280) add(1280, 1024);
        if (width > 1600) add(1600, 1200);
        if (width > 1800) add(1800, 1350);
        if (width > 2048) add(2048, 1536);
        add(width, height);
    }
    if (help_.empty()) {
        help_ += LoadText(0x2a56de25);
        help_ += '\n';
        help_ += LoadText(0xca56de46);
        help_ += '\n';
        help_ += LoadText(0x0a56de50);
        help_ += '\n';
    }
    const cRZRect largest = sizes_.back();
    int x = 0, y = 0;
    if (cIGZWinMgr* manager = GetWindowManager()) manager->GetCursorPos(GetParentWin(), x, y);
    GZWinMoveTo(x - largest.right / 2, y - largest.bottom / 2);
    SetSize(largest.right, largest.bottom);
    AdjustPosition();
    if (cIGZWinMgr* manager = GetWindowManager())
        if (cIGZCursorManager* cursors = manager->GetCursorManager()) SetCursor(cursors->LoadCursor(0x11), false);
    initialized_ = true;
    const bool ok = cGZWin::Init();
    GZSetCapture();
    SetFocus();
    GoToSizeIndex(static_cast<std::uint32_t>(sizes_.size() - 1));
    return ok;
}

bool cSC4WinSnapshotTaker::Shutdown() {
    if (!initialized_) return true;
    Drop(font_);
    initialized_ = false;
    return cGZWin::Shutdown();
}

void cSC4WinSnapshotTaker::AdjustPosition() {
    cIGZWin* parent = GetParentWin();
    int dx = 0, dy = 0;
    if (GetL() < 0) dx = -GetL();
    else if (parent && parent->GetW() < GetR()) dx = parent->GetW() - GetR();
    if (GetT() < 0) dy = -GetT();
    else if (parent && parent->GetH() < GetB()) dy = parent->GetH() - GetB();
    GZWinMoveTo(GetL() + dx, GetT() + dy);
}

void cSC4WinSnapshotTaker::RemoveSelf() {
    cIGZWin* parent = GetParentWin();
    cIGZWinMgr* manager = GetWindowManager();
    if (GZAmCapturing()) GZReleaseCapture();
    if (manager && parent) {
        parent->ChildRemove(this);
        manager->GZSetFocus(parent);
    }
}

bool cSC4WinSnapshotTaker::GoToSizeIndex(std::uint32_t index) {
    if (sizes_.empty()) return false;
    cRZRect area;
    GetArea(area);
    if (index >= sizes_.size()) index = static_cast<std::uint32_t>(sizes_.size() - 1);
    sizeIndex_ = index;
    const cRZRect& size = sizes_[index];
    cRZRect next;
    next.left = (area.left + area.right) / 2 - size.right / 2;
    next.top = (area.top + area.bottom) / 2 - size.bottom / 2;
    next.right = size.right + next.left;
    next.bottom = size.bottom + next.top;
    SetArea(next);
    AdjustPosition();
    return true;
}

bool cSC4WinSnapshotTaker::CycleSize() {
    std::uint32_t next = sizeIndex_ + 1;
    if (next >= sizes_.size()) next = 0;
    return GoToSizeIndex(next);
}

bool cSC4WinSnapshotTaker::GZOnKeyDown(std::uint32_t key, std::uint32_t flags) {
    if (key == 0x20) {
        CycleSize();
    } else if (key == 0x1b) {
        RemoveSelf();
    } else if (key == 0x0d) {
        if (IsVisible()) {
            HideWindow();
            const bool closeAfter = (flags & 7) == 0;   // Shift (1), Ctrl (2) or Alt (4) keeps the frame for more
            PostMsg(this, kSnapshotTakeMessage, closeAfter ? 1 : 0, 0, 0);
            if (g_host) g_host->PlayEvent(0xaa8b82a9);
        }
    } else if (key - 0x30 < 10) {
        // the digits: 0 -> 10 .. 9 -> 100 (written as key * 10 - 470, at least 10, at most 100)
        std::uint32_t quality = key * 10 - 0x1d6;
        if (quality < 10) quality = 10;
        quality_ = std::min<std::uint32_t>(quality, 100);
    }
    return true;
}

bool cSC4WinSnapshotTaker::GZOnCharacter(char) { return true; }

bool cSC4WinSnapshotTaker::GZOnMouseDownL(int, int, std::uint32_t flags) { return GZOnKeyDown(0x0d, flags); }

bool cSC4WinSnapshotTaker::GZOnMouseDownR(int, int, std::uint32_t) {
    RemoveSelf();
    return true;
}

bool cSC4WinSnapshotTaker::GZOnMouseMove(int x, int y, std::uint32_t) {
    WindowToWindowCoordinates(GetParentWin(), x, y);
    GZWinMoveTo(x - GetW() / 2, y - GetH() / 2);
    AdjustPosition();
    return true;
}

bool cSC4WinSnapshotTaker::GZPaint() {
    auto* context = static_cast<gz::cIGZDrawContext*>(drawContext_);
    if (!context) return cGZWin::GZPaint();
    // The outline in the fill color (white), one pixel inside the frame's edges, then the help text inset by 4.
    const int l = drawArea_.left, t = drawArea_.top, r = drawArea_.right - 1, b = drawArea_.bottom - 1;
    context->SetColor(GetFillColorRGB());
    context->Line(l, t, r, t);
    context->Line(l, t, l, b);
    context->Line(r, t, r, b);
    context->Line(l, b, r, b);
    if (font_ && drawBuffer_) {
        const cRZRect box{drawArea_.left + 4, drawArea_.top + 4, drawArea_.right - 4, drawArea_.bottom - 4};
        font_->DrawTextBox(drawBuffer_, box, help_.data(), static_cast<std::uint32_t>(help_.size()), 0, GetFillColor(),
                           gz::kWinUnsetTextColor, nullptr, 0x8000000);
    }
    return true;
}

bool cSC4WinSnapshotTaker::DoMessage(cGZMessage& message) {
    if (message.type != kSnapshotTakeMessage) return cGZWin::DoMessage(message);
    if (message.param2 == 3) {
        if (message.param1 == 0) {
            TakeSnapshot(false);
            ShowWindow();
        } else {
            TakeSnapshot(true);
            RemoveSelf();
        }
    } else {
        // three frames later, once the hidden frame is out of the picture
        PostMsg(this, kSnapshotTakeMessage, message.param1, message.param2 + 1, 0);
    }
    return true;
}

bool cSC4WinSnapshotTaker::TakeSnapshot(bool close) {
    cRZString name;
    cIGZBuffer* picture = nullptr;
    cIGZWin* view = GetParentWin();
    if (g_host && view) {
        int x = GetL(), y = GetT(), w = GetR() - GetL(), h = GetB() - GetT();
        if (view->GetW() == w && view->GetH() == h && std::abs(x) < 6 && std::abs(y) < 6) x = y = 0;   // the whole view
        picture = g_host->CaptureView(view, x, y, w, h);
        if (picture && picture->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) {
            // the alpha channel forced opaque for the 16- and 32-bit formats with alpha
            auto* row = static_cast<std::uint8_t*>(picture->GetColorSurfaceBits());
            const std::uint32_t stride = picture->GetColorSurfaceStride();
            const cRZRect* area = picture->GetBufferArea();
            const int width = area->right - area->left, height = area->bottom - area->top;
            const std::int32_t type = picture->GetColorType().type;
            for (int line = 0; row && line < height; ++line, row += stride) {
                for (int i = 0; i < width; ++i) {
                    if (type == 9) {
                        row[i * 4 + 3] = 0xff;
                    } else if (type == 7 || type == 6) {
                        std::uint16_t p;
                        std::memcpy(&p, row + i * 2, 2);
                        p |= type == 7 ? 0xf000 : 0x8000;
                        std::memcpy(row + i * 2, &p, 2);
                    }
                }
            }
            picture->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
        }
    }
    cIGZWinMgr* manager = GetWindowManager();
    cIGZWin* main = manager ? manager->GetMainWindow() : nullptr;
    if (close) HideWindow();
    if (cSC4WinSnapshotAlbum* album = ShowSnapshotAlbum(true, close, main, nullptr)) {
        album->GoToAlbumForCurrentCity(true);
        if (picture && album->GetNewTempFileNameForCurrentAlbum(name)) {
            if (!cWinBMPView::WriteBMPToFile(picture, 1, name)) cRZFile::Remove(name);
            else AddNewGraphicForCurrentCity(album, name, false);
        }
        album->DoModalWin();
        album->Release();
    }
    if (picture) picture->Release();
    return true;
}

bool cSC4WinSnapshotTaker::GetGraphicInfoForCurrentCity(cRZString& out) {
    out.AssignFromCString("");
    if (!g_host) return false;
    SnapshotCityInfo city;
    g_host->CityInfo(city);
    if (!city.hasCity) return false;
    cRZString user;
    g_host->UserName(user);
    char number[64];
    std::string day, population, funds, cheats, view;
    if (city.hasSimulator) {
        std::snprintf(number, sizeof number, "%d", city.simDay);
        day = number;
    }
    if (city.hasPopulation) {
        std::snprintf(number, sizeof number, "%d", city.population);
        population = number;
    }
    if (city.hasFunds) {
        std::snprintf(number, sizeof number, "%lld", static_cast<long long>(city.funds));
        funds = number;
    }
    if (city.hasCheats) {
        std::snprintf(number, sizeof number, "%d", city.cheats);
        cheats = number;
    }
    if (city.hasView) {
        std::snprintf(number, sizeof number, "%u,%u,%u,%u", city.zoom, city.rotation, city.centerX, city.centerZ);
        view = number;
    }
    std::string text;
    for (const std::string& field : {std::string(kSanityCheck), Str(user), Str(city.city), Str(city.mayor), day,
                                     Str(city.dateText), population, funds, cheats, view}) {
        text += field;
        text += '\n';
    }
    out = Rz(text);
    return true;
}

bool cSC4WinSnapshotTaker::AddNewGraphicForCurrentCity(cSC4WinSnapshotAlbum* album, const cRZString& file, bool move) {
    cRZString info;
    GetGraphicInfoForCurrentCity(info);
    SnapshotCityInfo city;
    if (g_host) g_host->CityInfo(city);
    return album->AddNewGraphic(file, info, cRZString(), city.city, city.hasCity ? city.number : kSnapshotRegionAlbumNumber, move);
}

cSC4WinSnapshotAlbum* cSC4WinSnapshotTaker::ShowSnapshotAlbum(bool create, bool show, cIGZWin* parent, const cIGZString* file) {
    if (!create || !parent || parent->GetChildWindowFromID(kSnapshotAlbumWindowId)) return nullptr;
    auto* album = new cSC4WinSnapshotAlbum();
    album->AddRef();
    cIGZWin* window = nullptr;
    if (album->CreateSnapshotAlbumWindow(window) && window) {
        parent->ChildAdd(window);
        window->Release();
    }
    window = album->Window();
    if (window) {
        if (show) window->ShowWindow();
        else window->HideWindow();
        window->PullToFront();
    }
    if (!file) {
        album->GoToAlbumForCurrentCity(false);
        return album;
    }
    album->SetCurrentFile(cRZString(*file), true);
    return album;
}

// ==================================================================================================================
// cSC4WinSnapshotAlbum
// ==================================================================================================================

cSC4WinSnapshotAlbum::cSC4WinSnapshotAlbum() {
    for (const char* extension : {".png", ".jpg", ".jpeg", ".jp2", ".gif", ".bmp"}) extensions_.emplace_back(extension);
}

cSC4WinSnapshotAlbum::~cSC4WinSnapshotAlbum() { on_shutdown(); }

bool cSC4WinSnapshotAlbum::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == gz::kGZIID_cIGZWinProc || iid == 1) {
        *out = static_cast<gz::cIGZWinProc*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4WinSnapshotAlbum::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

cRZString cSC4WinSnapshotAlbum::AlbumPath() const { return Rz(Str(root_) + Str(currentAlbum_)); }

void cSC4WinSnapshotAlbum::CityNameToAlbumName(const cRZString& city, cRZString& out) {
    SC4SanitizeFileName(city, out);
    SC4LimitStringLength(out, out, 0x30);
}

bool cSC4WinSnapshotAlbum::ConvertAlbumDirectoryNameToAlbumName(const cRZString& directory, cRZString& out) {
    const std::string name = Str(directory);
    if (name.empty()) return false;
    const std::size_t at = name.find(kAlbumSeparator);
    if (at == std::string::npos || at > name.size() - 3) return false;
    out = Rz(name.substr(0, at));
    return true;
}

void cSC4WinSnapshotAlbum::ObfuscateData(const cRZString& in, cRZString& out) {
    std::string data = Str(in);
    for (char& c : data) c = static_cast<char>(c ^ kSnapshotInfoKey);
    out.AssignFromChars(data.data(), static_cast<std::uint32_t>(data.size()));
}

void cSC4WinSnapshotAlbum::GetPathForInfoFile(const cRZString& file, cRZString& out) { out = Rz(WithExtension(file, ".info")); }

void cSC4WinSnapshotAlbum::GetPathForDescriptionFile(const cRZString& file, cRZString& out) { out = Rz(WithExtension(file, ".txt")); }

bool cSC4WinSnapshotAlbum::SaveDescriptionOfFile(const cRZString& file, const cRZString& text) {
    cRZString path;
    GetPathForDescriptionFile(file, path);
    if (text.Strlen() == 0) {
        cRZFile::Remove(path);
        return true;
    }
    return WriteWholeFile(path, Str(text));
}

bool cSC4WinSnapshotAlbum::SaveDescriptionOfFile() {
    if (!description_ || currentFile_.Strlen() == 0) return false;
    const cRZString file = Rz(Str(root_) + Str(currentAlbum_) + Str(currentFile_));
    cRZString text;
    description_->GetText(text);
    if (Str(text) != Str(description_text_)) {   // a description that never changed is not written back
        description_text_ = text;
        SaveDescriptionOfFile(file, text);
    }
    return true;
}

bool cSC4WinSnapshotAlbum::GetDescriptionOfFile(const cRZString& file, cRZString& out) {
    cRZString path;
    GetPathForDescriptionFile(file, path);
    std::string text;
    if (!ReadWholeFile(path, text)) {
        out = Rz(LoadText(0xca56ddf9));   // the "type a description" placeholder
        return false;
    }
    out = Rz(text);
    return true;
}

bool cSC4WinSnapshotAlbum::SaveInformationOfStandardFile(const cRZString& file, const cRZString& info) {
    cRZString path, data;
    GetPathForInfoFile(file, path);
    ObfuscateData(info, data);
    return WriteWholeFile(path, Str(data));
}

bool cSC4WinSnapshotAlbum::GetInformationOfStandardFile(const cRZString& file, cRZString& out) {
    cRZString path;
    GetPathForInfoFile(file, path);
    std::string caption, raw;
    bool ok = false;
    if (ReadWholeFile(path, raw)) {
        cRZString plain;
        ObfuscateData(Rz(raw), plain);
        std::vector<std::string> fields;
        std::string rest = Str(plain);
        for (int i = 0; i < 11; ++i) {
            const std::size_t at = rest.find('\n');
            fields.push_back(rest.substr(0, at));
            rest = at == std::string::npos ? std::string() : rest.substr(at + 1);
        }
        if (fields[0] == kSanityCheck && g_host) {
            auto line = [&caption](std::uint32_t format, const std::string& value) {
                char buffer[512];
                std::snprintf(buffer, sizeof buffer, LoadText(format).c_str(), value.c_str());
                caption += buffer;
                caption += '\n';
            };
            line(0xea946efe, fields[2]);   // the city
            line(0xea946efd, fields[3]);   // the mayor
            cRZString date;
            g_host->SimDateText(std::atoi(fields[4].c_str()), date);
            line(0xea946efc, Str(date));   // the date of the day number
            line(0xea946efb, fields[6]);   // the population
            cRZString money;
            g_host->MoneyString(std::atoi(fields[7].c_str()), money);
            line(0xea946efa, Str(money));  // the funds, in the money format
        }
        ok = true;
    }
    cRZString name;
    cRZFile::GetPathName(file, name, true);
    caption += '\n';
    caption += Str(name);
    caption += '\n';
    out = Rz(caption);
    return ok;
}

bool cSC4WinSnapshotAlbum::AlbumExists(const cRZString& albumName, std::uint32_t number) {
    char folder[512];
    std::snprintf(folder, sizeof folder, "%s%s--%u", root_.ToChar(), albumName.ToChar(), number);
    return cRZDirectory::DoesDirectoryExist(cRZString(folder));
}

bool cSC4WinSnapshotAlbum::CreateAlbum(const cRZString& albumName, std::uint32_t number) {
    if (AlbumExists(albumName, number)) return true;
    char folder[512];
    std::snprintf(folder, sizeof folder, kAlbumNameFormat, albumName.ToChar(), number);
    const std::string path = Str(root_) + folder + "/";
    if (!cRZDirectory::CreateDirectoryA(Rz(path))) return false;
    cRZFile::FileCreate(Rz(path + kAlbumIni), false);
    bool changed = false;
    update_list_of_album_directories(changed, false);
    return true;
}

bool cSC4WinSnapshotAlbum::album_directory_has_album_entries(const cRZString& directory, bool needPictures) {
    const std::string folder = Str(root_) + Str(directory);
    if (!cRZFile::FileExists(Rz(folder + kAlbumIni))) return false;
    return !needPictures || cRZDirectory::DoesAnyEntryExistThatMatchesPattern(Rz(folder + "*.png"));
}

bool cSC4WinSnapshotAlbum::convert_album_directory_name_to_album_index(const cRZString& directory, std::uint32_t& index) {
    for (std::uint32_t i = 0; i < albums_.size(); ++i) {
        if (Str(albums_[i]) == Str(directory)) {
            index = i;
            return true;
        }
    }
    index = 0xffffffffu;
    return false;
}

bool cSC4WinSnapshotAlbum::update_list_of_album_directories(bool& changed, bool needPictures) {
    cRZDirectory directory(cRZString(), false, false, false);
    std::list<cRZString> entries;
    const std::vector<cRZString> previous = albums_;
    changed = false;
    albums_.clear();
    if (list_) list_->RemoveAllStrings();
    directory.ChangeDirectory(root_, false);
    directory.ReadDirectoryDirectoryEntriesIntoStringList(entries, 4);
    std::string selected;
    entries.sort([](const cRZString& a, const cRZString& b) { return Str(a) < Str(b); });
    for (cRZString& entry : entries) {
        std::string name = Str(entry);
        if (name.empty()) continue;
        if (name.back() != '/') name += '/';
        const cRZString folder = Rz(name);
        cRZString albumName;
        if (!ConvertAlbumDirectoryNameToAlbumName(folder, albumName)) continue;
        if (!album_directory_has_album_entries(folder, needPictures)) continue;
        albums_.push_back(folder);
        if (std::none_of(previous.begin(), previous.end(), [&](const cRZString& p) { return Str(p) == name; })) changed = true;
        if (name == Str(currentAlbum_)) selected = Str(albumName);
        if (list_) list_->InsertString(albumName, -2, 0);
    }
    if (albums_.size() != previous.size()) changed = true;
    if (!list_ || selected.empty()) {
        currentAlbum_.AssignFromCString("");
        if (!albums_.empty()) {
            currentAlbum_ = albums_[0];
            set_image_count_text(0, static_cast<std::uint32_t>(graphics_.size()));
            SetCurrentFile(cRZString(), true);
        } else {
            set_to_no_image_file(true);
        }
        if (list_) {
            list_->SetSelection(0, false);
            list_->PutIndexInView(0);
        }
    } else {
        int index = 0;
        list_->GetIndexFromString(index, Rz(selected));
        list_->SetSelection(index, false);
        list_->PutIndexInView(index);
    }
    return true;
}

bool cSC4WinSnapshotAlbum::DeleteAlbum(std::uint32_t index) {
    if (index >= albums_.size()) return false;
    const std::string folder = Str(root_) + Str(albums_[index]);
    cRZDirectory directory(cRZString(), false, false, false);
    directory.ChangeDirectory(Rz(folder), false);
    for (const char* filter : {"*.png", "*.info", "*.txt"}) {
        std::list<cRZString> files;
        directory.SetNewDirectoryEntryFilter(cRZString(filter), false);
        directory.ReadDirectoryFileEntriesIntoStringList(files);
        for (const cRZString& f : files) cRZFile::Remove(Rz(folder + Str(f)));
    }
    cRZFile::Remove(Rz(folder + kAlbumIni));
    std::list<cRZString> rest;
    directory.SetNewDirectoryEntryFilter(cRZString("*.*"), false);
    directory.ReadDirectoryFileEntriesIntoStringList(rest);
    if (rest.empty()) cRZDirectory::RemoveDirectoryA(Rz(folder), true);
    bool changed = false;
    update_list_of_album_directories(changed, true);
    return true;
}

bool cSC4WinSnapshotAlbum::RefreshGraphicList(bool* changed) {
    if (root_.Strlen() == 0 || (window_ && !window_->IsVisible())) return true;
    std::list<cRZString> before;
    if (currentAlbum_.Strlen() == 0) {
        if (!list_) return false;
        if (list_->GetLineCount() == 0) {
            set_to_no_image_file(true);
            return false;
        }
        const int selection = list_->GetSelection();
        if (selection < 0 || static_cast<std::size_t>(selection) >= albums_.size()) return false;
        currentAlbum_ = albums_[static_cast<std::size_t>(selection)];
    }
    if (changed) {
        *changed = false;
        before = graphics_;
    }
    graphics_.clear();
    cRZDirectory directory(cRZString(), false, false, false);
    directory.ChangeDirectory(AlbumPath(), false);
    directory.ReadDirectoryFileEntriesIntoStringList(graphics_);
    graphics_.sort([](const cRZString& a, const cRZString& b) { return Str(a) < Str(b); });
    if (graphics_.empty()) {
        if (list_) DeleteAlbum(static_cast<std::uint32_t>(list_->GetSelection()));
        set_to_no_image_file(true);
    } else {
        // only the recognized picture extensions stay (compared over the shorter length, case-insensitive)
        for (auto it = graphics_.begin(); it != graphics_.end();) {
            cRZString extension;
            cRZFile::GetPathExtension(*it, extension);
            const std::string ext = Str(extension);
            const bool known = std::any_of(extensions_.begin(), extensions_.end(), [&](const cRZString& e) {
                const std::size_t n = std::min<std::size_t>(ext.size(), e.Strlen());
                return strncasecmp(e.Data(), ext.c_str(), n) == 0;
            });
            it = known ? std::next(it) : graphics_.erase(it);
        }
        set_image_count_text();
    }
    if (changed) {
        *changed = before.size() != graphics_.size() ||
                   !std::equal(before.begin(), before.end(), graphics_.begin(),
                               [](const cRZString& a, const cRZString& b) { return Str(a) == Str(b); });
        // Quirk kept: the original answers "equal" as the change flag when the sizes match.
        if (before.size() == graphics_.size()) *changed = !*changed;
    }
    return true;
}

void cSC4WinSnapshotAlbum::set_image_count_text(std::uint32_t index, std::uint32_t count) {
    if (!count_) return;
    char text[64] = "";
    if (count != 0) std::snprintf(text, sizeof text, "(%u/%u)", index + 1, count);
    count_->SetCaption(cRZString(text));
}

void cSC4WinSnapshotAlbum::set_image_count_text() {
    std::uint32_t index = 0;
    for (const cRZString& g : graphics_) {
        if (g.IsEqualToStringRef(currentFile_, false)) break;
        ++index;
    }
    set_image_count_text(index, static_cast<std::uint32_t>(graphics_.size()));
}

void cSC4WinSnapshotAlbum::set_to_no_image_file(bool showImage) {
    shownFile_.AssignFromCString("");
    if (view_) {
        if (showImage) {
            if (!noImage_ && g_host) {
                noImage_ = g_host->LoadImage(0x46a006b0, 0x2558a4cd);
            }
            view_->SetBMP(noImage_);
        } else {
            view_->ClearBMP();
        }
    }
    if (info_) info_->SetText(cRZString(), false);
    if (description_) description_->SetText(cRZString(), false);
    set_image_count_text(0, 0);
}

void cSC4WinSnapshotAlbum::enable_button(std::uint32_t id, bool on) {
    if (!window_) return;
    if (cIGZWin* button = window_->GetChildWindowFromIDRecursive(id)) button->SetFlag(kWinEnabled, on);
}

bool cSC4WinSnapshotAlbum::GoToGraphic(const cRZString& file) {
    const cRZString path = Rz(Str(AlbumPath()) + Str(file));
    if (!cRZFile::FileExists(path)) return false;
    shownFile_ = file;
    currentFile_ = file;
    if (view_) view_->SetBMPFromFile(path);
    if (info_) {
        cRZString caption;
        GetInformationOfStandardFile(path, caption);
        info_->SetText(caption, false);
    }
    if (description_) {
        GetDescriptionOfFile(path, description_text_);
        description_->SetText(description_text_, false);
    }
    set_image_count_text();
    return true;
}

bool cSC4WinSnapshotAlbum::SetCurrentFile(const cRZString& file, bool refresh) {
    cRZString name = file;
    if (name.Strlen() != 0 && !cRZFile::FileExists(Rz(Str(AlbumPath()) + Str(name)))) return false;
    SaveDescriptionOfFile();
    if (refresh) RefreshGraphicList(nullptr);
    if (name.Strlen() == 0) {
        if (graphics_.empty()) return false;
        name = graphics_.front();
    }
    const cRZString path = Rz(Str(AlbumPath()) + Str(name));
    shownFile_ = name;
    if (view_) {
        const bool loaded = view_->SetBMPFromFile(path);
        if (Trace()) {
            cIGZWin* parent = view_->GetParentWin();
            std::fprintf(stderr, "snapshot album: picture %s %s; view %d,%d %dx%d visible %d in %08x (visible %d)\n", name.ToChar(),
                         loaded ? "loaded" : "NOT LOADED", view_->GetL(), view_->GetT(), view_->GetW(), view_->GetH(),
                         view_->IsVisible(), parent ? parent->GetID() : 0, parent ? parent->IsVisible() : 0);
        }
    }
    currentFile_ = name;
    if (info_) {
        cRZString caption;
        GetInformationOfStandardFile(path, caption);
        info_->SetText(caption, false);
    }
    if (description_) {
        GetDescriptionOfFile(path, description_text_);
        description_->SetText(description_text_, false);
    }
    enable_button(kSnapshotButtonDelete, !graphics_.empty());
    if (graphics_.size() < 2) {
        if (browseEnabled_) {
            browseEnabled_ = false;
            enable_button(kSnapshotButtonPrevious, false);
            enable_button(kSnapshotButtonNext, false);
        }
    } else if (!browseEnabled_) {
        browseEnabled_ = true;
        enable_button(kSnapshotButtonPrevious, true);
        enable_button(kSnapshotButtonNext, true);
    }
    return true;
}

bool cSC4WinSnapshotAlbum::SetInitialFile() {
    if (root_.Strlen() == 0 || albums_.empty()) return true;
    if (currentAlbum_.Strlen() == 0 && list_) {
        const int selection = list_->GetSelection();
        if (selection < 0 || static_cast<std::size_t>(selection) >= albums_.size()) list_->SetSelection(0, true);
    }
    const cRZString path = Rz(Str(AlbumPath()) + Str(currentFile_));
    if (graphics_.empty() || currentFile_.Strlen() == 0 || !cRZFile::FileExists(path)) {
        RefreshGraphicList(nullptr);
        if (graphics_.empty()) return false;
        currentFile_ = graphics_.front();
    }
    const cRZString file = currentFile_;
    currentFile_.AssignFromCString("");
    set_image_count_text(0, static_cast<std::uint32_t>(graphics_.size()));
    return SetCurrentFile(file, false);
}

bool cSC4WinSnapshotAlbum::GoToNextGraphic() {
    if (!view_) return false;
    if (currentFile_.Strlen() == 0 || graphics_.empty()) {
        RefreshGraphicList(nullptr);
        if (graphics_.empty()) {
            set_to_no_image_file(true);
            return false;
        }
        currentFile_ = graphics_.front();
    }
    SaveDescriptionOfFile();
    std::uint32_t index = 0;
    auto it = graphics_.begin();
    for (; it != graphics_.end(); ++it) {
        ++index;
        if (it->IsEqualToStringRef(currentFile_, true)) {
            ++it;
            break;
        }
    }
    if (it == graphics_.end()) {   // past the last one: the first again
        it = graphics_.begin();
        index = 0;
    }
    if (it == graphics_.end()) return false;
    set_image_count_text(index, static_cast<std::uint32_t>(graphics_.size()));
    return GoToGraphic(cRZString(*it));
}

bool cSC4WinSnapshotAlbum::GoToPreviousGraphic() {
    if (!view_) return false;
    if (currentFile_.Strlen() == 0 || graphics_.empty()) return GoToNextGraphic();
    SaveDescriptionOfFile();
    std::uint32_t index = static_cast<std::uint32_t>(graphics_.size()) - 1;
    auto it = graphics_.rbegin();
    for (; it != graphics_.rend(); ++it) {
        --index;
        if (it->IsEqualToStringRef(currentFile_, true)) {
            ++it;
            break;
        }
    }
    if (it == graphics_.rend()) {   // before the first one: the last again
        it = graphics_.rbegin();
        index = static_cast<std::uint32_t>(graphics_.size()) - 1;
    }
    if (it == graphics_.rend()) return false;
    set_image_count_text(index, static_cast<std::uint32_t>(graphics_.size()));
    return GoToGraphic(cRZString(*it));
}

bool cSC4WinSnapshotAlbum::GoToAlbum(std::uint32_t index) {
    if (index >= albums_.size()) return false;
    SaveDescriptionOfFile();
    if (list_ && static_cast<std::uint32_t>(list_->GetSelection()) != index) list_->SetSelection(static_cast<int>(index), false);
    currentAlbum_ = albums_[index];
    RefreshGraphicList(nullptr);
    if (graphics_.empty()) {
        set_to_no_image_file(true);
    } else {
        set_image_count_text(0, static_cast<std::uint32_t>(graphics_.size()));
        SetCurrentFile(cRZString(graphics_.front()), false);
    }
    return true;
}

bool cSC4WinSnapshotAlbum::GoToAlbum(const cRZString& city, std::uint32_t number, bool create) {
    cRZString albumName;
    CityNameToAlbumName(city, albumName);
    if (!AlbumExists(albumName, number)) {
        if (!create) return false;
        CreateAlbum(albumName, number);
    }
    if (!AlbumExists(albumName, number)) return false;
    char folder[512];
    std::snprintf(folder, sizeof folder, kAlbumNameFormat, albumName.ToChar(), number);
    std::uint32_t index = 0;
    if (!convert_album_directory_name_to_album_index(Rz(std::string(folder) + "/"), index)) return false;
    return GoToAlbum(index);
}

bool cSC4WinSnapshotAlbum::GoToAlbumForCurrentCity(bool create) {
    SnapshotCityInfo city;
    if (g_host) g_host->CityInfo(city);
    return GoToAlbum(city.city, city.hasCity ? city.number : kSnapshotRegionAlbumNumber, create);
}

bool cSC4WinSnapshotAlbum::DeleteCurrentGraphic() {
    if (!view_ || currentFile_.Strlen() == 0) return false;
    const cRZString file = currentFile_;
    const cRZString path = Rz(Str(AlbumPath()) + Str(file));
    if (!cRZFile::Remove(path)) {
        if (g_host) g_host->Notify(Rz(LoadText(0xaa56dd9d)), Rz(LoadText(0x4a56ddc0)));
        return false;
    }
    cRZString companion;
    GetPathForInfoFile(path, companion);
    cRZFile::Remove(companion);
    GetPathForDescriptionFile(path, companion);
    cRZFile::Remove(companion);
    if (graphics_.size() < 2) {
        graphics_.clear();
        set_to_no_image_file(true);
        if (deleteEmptyAlbums_ && list_) DeleteAlbum(static_cast<std::uint32_t>(list_->GetSelection()));
    } else {
        GoToNextGraphic();
        graphics_.remove_if([&](const cRZString& g) { return Str(g) == Str(file); });
        RefreshGraphicList(nullptr);
        if (graphics_.size() < 2 && browseEnabled_) {
            browseEnabled_ = false;
            enable_button(kSnapshotButtonPrevious, false);
            enable_button(kSnapshotButtonNext, false);
        }
    }
    return true;
}

bool cSC4WinSnapshotAlbum::GetNewTempFileName(const cRZString& directory, cRZString& out) {
    SnapshotCityInfo city;
    if (g_host) g_host->CityInfo(city);
    std::string name = Str(city.city);
    if (name.size() > 0x20) {   // cut to 32 bytes, then back to a valid character boundary
        name.resize(0x20);
        while (!name.empty() && !IsStringValid(name.data(), static_cast<unsigned>(name.size()), -2)) name.pop_back();
    }
    cRZString albumName;
    CityNameToAlbumName(Rz(name), albumName);
    std::string prefix = Str(albumName);
    prefix += '-';
    prefix += Str(city.fileDate);
    cRZString valid;
    MakeFileNameValid(Rz(prefix), valid, nullptr);
    out = valid;
    const cRZString extension(".png");
    if (!cRZFile::MakeTempFileName(directory, out, &extension)) return false;
    return true;
}

bool cSC4WinSnapshotAlbum::GetNewTempFileNameForCurrentAlbum(cRZString& out) {
    cRZString name;
    if (!GetNewTempFileName(AlbumPath(), name)) return false;
    out = name;
    return true;
}

bool cSC4WinSnapshotAlbum::AddNewGraphic(const cRZString& file, const cRZString& info, const cRZString& description,
                                         const cRZString& city, std::uint32_t number, bool move) {
    cRZString albumName;
    CityNameToAlbumName(city, albumName);
    if (!AlbumExists(albumName, number) && !CreateAlbum(albumName, number)) {
        if (move) cRZFile::Remove(file);
        return false;
    }
    // The file's folder. The Mac build's cRZFile::GetPathDirectory always answers empty, so there every picture was
    // copied a second time; the Windows game compares the real folders (snapshots.md 3.3), as this does.
    const std::string filePath = Str(file);
    const cRZString fileDirectory = Rz(filePath.substr(0, filePath.find_last_of('/') + 1));
    char folder[512];
    std::snprintf(folder, sizeof folder, kAlbumNameFormat, albumName.ToChar(), number);
    const std::string albumDirectory = Str(root_) + folder + "/";
    std::string target;
    cRZString name;
    auto bare = [](std::string path) {   // the folder without its trailing separator (GetPathDirectory leaves it on or off)
        while (!path.empty() && path.back() == '/') path.pop_back();
        return path;
    };
    if (bare(Str(fileDirectory)) == bare(albumDirectory)) {   // already in the album: it keeps its name
        cRZFile::GetPathName(file, name, true);
        target = albumDirectory + Str(name);
    } else {
        if (!GetNewTempFileName(Rz(albumDirectory), name)) return false;
        target = Str(name);
        if (!cRZFile::Copy(file, Rz(target), true)) return false;
        if (move) cRZFile::Remove(file);
        cRZFile::GetPathName(Rz(target), name, true);
    }
    SaveInformationOfStandardFile(Rz(target), info.Strlen() ? info : name);
    if (description.Strlen() != 0) SaveDescriptionOfFile(Rz(target), description);
    GoToAlbum(city, number, true);
    RefreshGraphicList(nullptr);
    cRZString shown;
    cRZFile::GetPathName(Rz(target), shown, true);
    return GoToGraphic(shown);
}

// ---- the window ----------------------------------------------------------------------------------------------------

bool cSC4WinSnapshotAlbum::CreateSnapshotAlbumWindow(cIGZWin*& out) {
    out = nullptr;
    if (!g_host) return false;
    gz::cIGZWinGen* gen = nullptr;
    const cGZPersistResourceKey key{0, kUIScriptGroup, kSnapshotAlbumScript};
    if (!g_host->CreateWindowFromScript(key, nullptr, kSnapshotAlbumWindowId, kWindowGenIid, reinterpret_cast<void**>(&gen)) || !gen)
        return false;
    script_ = gen;
    out = gen->AsIGZWin();
    cIGZWin* main = g_host->MainWindow();
    const int width = main ? main->GetW() : 640, height = main ? main->GetH() : 480;
    out->GZWinMoveTo(0, 0);
    out->SetW(width);
    out->SetH(height);
    for (std::uint32_t id : {kSnapshotAlbumSmall, kSnapshotAlbumLarge}) {
        if (cIGZWin* layout = out->GetChildWindowFromIDRecursive(id))
            layout->GZWinMoveTo(static_cast<int>(static_cast<unsigned>(width - layout->GetW()) >> 1),
                                static_cast<int>(static_cast<unsigned>(height - layout->GetH()) >> 1));
    }
    gen->SetWinProc(static_cast<gz::cIGZWinProc*>(this));
    window_ = out;
    window_->AddRef();
    out->AddRef();   // the caller's reference (the script's own goes with the window)
    return on_init(out);
}

bool cSC4WinSnapshotAlbum::on_init(cIGZWin* window) {
    if (!window) return false;
    if (initialized_) return true;
    for (std::uint32_t id : {kSnapshotButtonPrevious, kSnapshotButtonNext, kSnapshotButtonClose, kSnapshotButtonDelete,
                             kSnapshotButtonEnlarge, kSnapshotButtonShrink, kSnapshotButtonShrinkAlt, kSnapshotAlbumList})
        if (cIGZWin* control = window->GetChildWindowFromIDRecursive(id)) control->SetNotificationTarget(window_);
    // each control by id anywhere in the window (slot 0x8c), then asked for its interface (slot 0)
    auto control = [window](std::uint32_t id, std::uint32_t iid, auto*& out) {
        cIGZWin* child = window->GetChildWindowFromIDRecursive(id);
        if (child) child->QueryInterface(iid, reinterpret_cast<void**>(&out));
    };
    control(kSnapshotAlbumList, gz::kGZIID_cIGZWinListBox, list_);
    control(kSnapshotInfoText, gz::kGZIID_cIGZWinTextEdit, info_);
    control(kSnapshotDescription, gz::kGZIID_cIGZWinTextEdit, description_);
    control(kSnapshotCountText, gz::kGZIID_cIGZWinText, count_);
    control(kSnapshotButtonEnlarge, gz::kGZIID_cIGZWinBtn, enlarge_);
    if (g_host) {
        background_ = g_host->LoadImage(0x1abe787d, 0x2558a4cb);
    }
    cIGZWin* large = window->GetChildWindowFromIDRecursive(kSnapshotAlbumLarge);
    view_ = new cWinBMPViewGVExtra(window->GetWindowManager());
    view_->AddRef();
    if (large) large->ChildAdd(view_);
    view_->SetAlignment(cWinBMPView::kAlignCenter);
    view_->SetShouldFitImageAtSizeIn(true);
    view_->SetArea(12, 39, 304, 257);
    cIGZWin* largeRect = window->GetChildWindowFromIDRecursive(kSnapshotAlbumLargeRect);
    cIGZWin* smallRect = window->GetChildWindowFromIDRecursive(kSnapshotAlbumSmallRect);
    view_->SetSizeInAndOutAreas(smallRect ? smallRect->GetArea() : cRZRect{0, 0, 16, 16},
                                largeRect ? largeRect->GetArea() : cRZRect{0, 0, 32, 32});
    view_->SetZoomRangeMinMax(0.125f, 4.0f);
    if (g_host) view_->SetFillColor(g_host->Color(0x88963fb4));
    view_->SetFlag(kWinIgnoreCursor, true);
    view_->SetNotificationTarget(window_);
    view_->SetSizeInOut(false);
    view_->SetSizeInOut(true);
    if (root_.Strlen() == 0 && g_host) g_host->AlbumsDirectory(root_);
    bool changed = false;
    update_list_of_album_directories(changed, false);
    SetInitialFile();
    initialized_ = true;
    if (Trace())
        std::fprintf(stderr, "snapshot album: controls list %p info %p description %p count %p view %p, root \"%s\", %zu albums\n",
                     static_cast<void*>(list_), static_cast<void*>(info_), static_cast<void*>(description_),
                     static_cast<void*>(count_), static_cast<void*>(view_), root_.ToChar(), albums_.size());
    return true;
}

bool cSC4WinSnapshotAlbum::on_shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    graphics_.clear();
    albums_.clear();
    if (window_ && g_host) {
        if (cIGZWin* large = window_->GetChildWindowFromIDRecursive(kSnapshotAlbumLarge)) g_host->SaveWindowLocation(large);
        if (cIGZWin* small = window_->GetChildWindowFromIDRecursive(kSnapshotAlbumSmall)) g_host->SaveWindowLocation(small);
    }
    Drop(window_);
    Drop(noImage_);
    Drop(background_);
    Drop(sizeOutBackground_);
    Drop(view_);
    Drop(list_);
    Drop(info_);
    Drop(count_);
    Drop(description_);
    Drop(enlarge_);
    return true;
}

void cSC4WinSnapshotAlbum::end_dialog(cIGZWin* window, std::uint32_t result) {
    if (!window) return;
    cIGZWinMgr* manager = window->GetWindowManager();
    if (!manager) return;
    if (window->GetFlag(kWinFlagModalDialog)) manager->EndDialog(0x0f, window, result);
    if (cIGZWin* parent = window->GetParentWin()) parent->ChildRemove(window);   // slot 0x3c (remove, 1, window)
}

void cSC4WinSnapshotAlbum::DoModalWin() {
    if (!window_) return;
    if (g_host) g_host->PauseGame(true);
    if (g_host) {
        if (cIGZWin* small = window_->GetChildWindowFromIDRecursive(kSnapshotAlbumSmall)) g_host->RestoreWindowLocation(small);
        if (cIGZWin* large = window_->GetChildWindowFromIDRecursive(kSnapshotAlbumLarge)) g_host->RestoreWindowLocation(large);
        g_host->RestoreWindowLocation(window_);
    }
    cIGZWin* window = window_;
    window->AddRef();
    if (Trace())
        std::fprintf(stderr, "snapshot album: modal with %zu albums (current \"%s\"), %zu pictures, file \"%s\", list lines %d\n",
                     albums_.size(), currentAlbum_.ToChar(), graphics_.size(), currentFile_.ToChar(), list_ ? list_->GetLineCount() : -1);
    if (cIGZWinMgr* manager = window->GetWindowManager()) manager->RunPopup(window);
    if (g_host) g_host->SaveWindowLocation(window);
    window->Release();
    if (g_host) g_host->PauseGame(false);
}

void cSC4WinSnapshotAlbum::MoveViewTo(std::uint32_t from, std::uint32_t to, bool sizedIn) {
    if (!window_ || !view_) return;
    cIGZWin* source = window_->GetChildWindowFromIDRecursive(from);
    cIGZWin* target = window_->GetChildWindowFromIDRecursive(to);
    if (source) source->HideWindow();
    view_->AddRef();
    if (source) source->ChildRemove(view_);
    if (target) target->ChildAdd(view_);
    view_->Release();
    if (target) target->ShowWindow();
    view_->SetSizeInOut(sizedIn);
    if (view_->FitAtSizeIn()) SetCurrentFile(currentFile_, false);
}

bool cSC4WinSnapshotAlbum::on_button_activated(cIGZWin* window, std::uint32_t id) {
    switch (id) {
        case kSnapshotButtonPrevious: GoToPreviousGraphic(); return true;
        case kSnapshotButtonNext: GoToNextGraphic(); return true;
        case kSnapshotButtonDelete: DeleteCurrentGraphic(); return true;
        case kSnapshotButtonClose:
        case kSnapshotButtonCloseAlt:
            SaveDescriptionOfFile();
            end_dialog(window, kSnapshotButtonCloseAlt);
            return true;
        case kSnapshotButtonEnlarge: MoveViewTo(kSnapshotAlbumSmall, kSnapshotAlbumLarge, true); return true;
        case kSnapshotButtonShrink:
        case kSnapshotButtonShrinkAlt: MoveViewTo(kSnapshotAlbumLarge, kSnapshotAlbumSmall, false); return true;
        default: return false;
    }
}

bool cSC4WinSnapshotAlbum::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    const std::uint32_t type = message.type;
    if (type == kSnapshotSizedInMessage) {
        if (view_) {
            if (view_->FitAtSizeIn()) SetCurrentFile(currentFile_, false);
            if (message.param2 == 0 && !sizeOutBackground_ && g_host) {
                sizeOutBackground_ = g_host->LoadImage(0x46a006b0, 0x2558a4cc);
            }
            view_->PullToFront();
        }
        return true;
    }
    if (type == kMsgCommand) {
        if (message.param1 == kCommandListSelect) {
            if (list_ && list_->AsIGZWin()->GetID() == static_cast<std::uint32_t>(message.param2)) {
                GoToAlbum(static_cast<std::uint32_t>(list_->GetSelection()));
                return true;
            }
        } else if (message.param1 == kCommandButton) {
            return on_button_activated(window, static_cast<std::uint32_t>(message.param2));
        }
        return false;
    }
    if (type == kGZMsgKeyDown) {
        const auto key = static_cast<std::uint32_t>(message.param1);
        if (key == 0x1b || ((key == 0x73 || key == 0x0d) && (message.param2 & 4))) {   // Esc, Alt+F4, Alt+Enter
            end_dialog(window, kSnapshotButtonCloseAlt);
            return true;
        }
        return false;
    }
    if (type == kMsgWinProcMinMax) {
        auto* sizes = reinterpret_cast<std::int32_t*>(static_cast<std::uintptr_t>(message.param1));
        if (!window || !sizes) return false;
        sizes[0] = 100;
        sizes[1] = 100;
        sizes[2] = 1000;
        sizes[3] = 1000;
        return true;
    }
    if (type == kMsgWinProcInit) return true;
    if (type == kMsgWinProcShutdown) return on_shutdown();
    return false;
}

}  // namespace sc4

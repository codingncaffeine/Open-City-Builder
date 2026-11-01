// cGZWin — the base of every GZ window (183 functions, 0x100168870..0x10016d3e6): a node in the window tree with
// a rectangle (parent coordinates, plus the cached absolute rectangle), a visibility / behaviour flag word,
// children kept front-to-back in a snapshot-able list, named parameters (variants), message filters, a
// keyboard accelerator and cursor, fill / shade colours and an optional private drawing buffer.
//
// Layout (0x150 bytes): +8 window manager, +0x10 keyboard, +0x18 refcount, +0x1c id, +0x20 absolute area,
// +0x30 draw area, +0x40 accumulated draw offset, +0x50 child list, +0x58 parent, +0x60 notification target
// (defaults to the parent), +0x68 caption, +0x90 private buffer, +0x98 buffer drawn to, +0xa0 draw context,
// +0xa8 invalid, +0xa9 draws to own buffer, +0xb0 cursor, +0xb8 accelerator, +0xc0 parameter map,
// +0xd8 message filters, +0xf0 filters waiting for removal, +0x108 dispatching, +0x10c/+0x110 fade in / out ms,
// +0x114 key equivalent, +0x118 mouse-down offset, +0x120 area, +0x130 blit area, +0x140 flags (initially 0x8903),
// +0x144 instance id, +0x148 shade colour (0xffffffff), +0x14c fill colour.
//
// The child list is copy-on-write for readers: iteration holds a "lock" on the list that was current; a writer
// that finds locks outstanding switches to a fresh copy and the stale list is freed by its last reader.
//
// The rendering pipeline (PlotComposite, PlotPresent, PrivateBuffer, the fade effect and the cGZFadeTmpWin that
// keeps a closing fading window on screen) talks to the buffers and the draw system only through the small
// IWinBuffer / IWinGraphics seams below.
#pragma once
#include <list>
#include <map>

#include "gz/WinInterfaces.h"
#include "rz/cRZString.h"

class cIGZBuffer;
class cIGZBufferDrawTarget;
namespace gz { class IDrawTargetBuffer; }

// A drawing buffer as far as the windows use it (cIGZBuffer).
class IWinBuffer : public cIGZUnknown {
public:
    virtual const cRZRect* GetBounds() const = 0;                                          // vslot 0x68
    virtual std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;   // vslot 0x100
    virtual void NativeToRGB(std::uint32_t native, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) = 0;   // vslot 0x108
    virtual void* GetDrawContext() = 0;                              // QueryInterface(0xab300b2b) vslot 0x20
    virtual bool SetFadeAlpha(std::uint8_t alpha) = 0;               // QueryInterface(0x86d72b57) vslot 0x18; false when unsupported
    // --- rendering pipeline (defaults describe a buffer that does nothing) ---
    virtual bool Init(int width, int height) { (void)width; (void)height; return true; }   // vslot 0x28 (colour type 1 = display)
    virtual void Shutdown() {}                                       // vslot 0x30
    virtual bool Lock(std::uint32_t flags) { (void)flags; return true; }     // vslot 0x40
    virtual void Unlock(std::uint32_t flags) { (void)flags; }                // vslot 0x48
    virtual bool IsReady() { return true; }                          // vslot 0x148
    virtual bool Fill(std::uint32_t color, const cRZRect& rect) { (void)color; (void)rect; return true; }   // vslot 0xa0 (no clip)
    virtual void SetTransparency(std::uint32_t color) { (void)color; }       // vslot 0xc8
    virtual bool GetTransparency() { return false; }                 // vslot 0xd0
    virtual bool IsPixelTransparent(int x, int y) { (void)x; (void)y; return false; }   // vslot 0xd8
    virtual bool GetTransparentColor(std::uint32_t& color) { (void)color; return false; }   // vslot 0xe0
    virtual void RemoveTransparency() {}                             // vslot 0xe8
    virtual bool Blt(IWinBuffer* source, const cRZRect& sourceRect, const cRZRect& destRect) { (void)source; (void)sourceRect; (void)destRect; return true; }   // vslot 0xf8 (no clip)
    virtual bool GetShadeColor(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a) { (void)r; (void)g; (void)b; (void)a; return false; }   // QueryInterface(0x86d72b57) vslot 0x20
    virtual cIGZBufferDrawTarget* QueryDrawTarget() { return nullptr; }      // QueryInterface(0xab300b2b)
    // The buffer itself as the font renderers draw into it (cIGZFont::DrawTextLine's target); null when there is none.
    virtual cIGZBuffer* AsGZBuffer() { return nullptr; }
    virtual gz::IDrawTargetBuffer* AsDrawSource() { return nullptr; }        // the buffer as a draw-context blit source
};
// What a control hands cIGZFont::DrawTextLine: the buffer's cIGZBuffer (the original's drawBuffer_ is one), else the window buffer.
inline void* FontTarget(IWinBuffer* buffer) {
    cIGZBuffer* gz = buffer ? buffer->AsGZBuffer() : nullptr;
    return gz ? static_cast<void*>(gz) : static_cast<void*>(buffer);
}

// Services of the graphics system the windows need.
class IWinGraphics {
public:
    virtual ~IWinGraphics() = default;
    virtual std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;          // RZ::GS() vslot 0x118
    virtual void NativeToRGB(std::uint32_t native, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) = 0;   // vslot 0x120
    virtual IWinBuffer* PrimaryBuffer() = 0;          // the screen buffer (cIGZGraphicSystem vslot 0x40)
    virtual bool DrawSystemActive() = 0;              // cIGZDrawSystem service present
    // Creates a display-format buffer of the given size through the draw system (service 0xeb2ea3cb vslot 0x28);
    // returns null when it fails. Only used while DrawSystemActive().
    virtual IWinBuffer* DrawSystemCreateBuffer(int width, int height) { (void)width; (void)height; return nullptr; }
    // A fresh uninitialised buffer from the graphics system (RZ::GS() vslot 0x20), used without a draw system.
    virtual IWinBuffer* GraphicsCreateBuffer() { return nullptr; }
    // The colour windows with a transparent private buffer key out (RZ::WCM() vslot 0xf8 for id 0x08963f6c);
    // false when the window class manager is missing.
    virtual bool GetPrivateBufferKeyColor(std::uint32_t& nativeColor) { (void)nativeColor; return false; }
};
IWinGraphics* WinGraphics();
void SetWinGraphics(IWinGraphics* graphics);
// Millisecond clock the fade effect uses (clock() * 1000 / CLOCKS_PER_SEC in the original).
using WinClockFn = std::uint32_t (*)();
void SetWinClock(WinClockFn fn);

class cGZWin : public cIGZWin {
public:
    cGZWin(cIGZWinMgr* mgr, cIGZKeyboard* keyboard);
    explicit cGZWin(cIGZWinMgr* mgr) : cGZWin(mgr, mgr ? mgr->GetKeyboard() : nullptr) {}
    virtual ~cGZWin();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool DoMessage(cGZMessage& message) override;
    bool Init() override;
    bool Shutdown() override;
    cIGZWinMgr* GetWindowManager() override { return mgr_; }
    bool SetWindowManager(cIGZWinMgr* mgr) override { mgr_ = mgr; return mgr != nullptr; }
    cIGZKeyboard* GetKeyboard() override { return keyboard_; }
    bool SetKeyboard(cIGZKeyboard* keyboard) override { keyboard_ = keyboard; return keyboard != nullptr; }
    cIGZWin* GetMainWindow() override { return mgr_->GetMainWindow(); }
    cIGZWin* GetParentWin() const override { return parent_; }
    bool SetParentWin(cIGZWin* parent) override;
    std::uint32_t GetChildCount() const override;
    bool ChildAdd(cIGZWin* child) override { return ChildAddAbsolute(child); }
    bool ChildRemove(cIGZWin* child) override { return ChildRemoveAbsolute(child); }
    bool ChildDelete(cIGZWin* child) override { return mgr_->DestroyWindow(child); }
    bool ChildDeleteAbsolute(cIGZWin* child) override;
    bool ChildDeleteAll() override;
    bool ChildExists(cIGZWin* child) override;
    bool ChildExists(std::uint32_t id) override;
    bool IsWinInParentChain(cIGZWin* window) override;
    bool IsWinInChildChain(cIGZWin* window) override;
    bool PullToFront() override { return parent_ ? parent_->ChildToFront(this) : false; }
    bool SendToBack() override { return parent_ ? parent_->ChildToBack(this) : false; }
    bool ChildToFront(cIGZWin* child) override;
    bool ChildToFront(std::uint32_t id) override { return ChildToFront(GetChildWindowFromID(id)); }
    bool ChildToBack(cIGZWin* child) override;
    bool ChildStepFront(cIGZWin* child) override;
    bool ChildStepBack(cIGZWin* child) override;
    bool MoveRelativeTo(cIGZWin* other, bool before) override;
    bool ChildMoveRelative(cIGZWin* child, cIGZWin* other, bool before) override;
    bool EnumChildren(std::uint32_t iid, EnumChildFn fn, void* context) override;
    bool SortChildren(SortFn fn, void* context, bool includeFixed) override;
    cIGZWin* GetChildWindowFromID(std::uint32_t id) override;
    cIGZWin* GetChildWindowFromIDRecursive(std::uint32_t id) override;
    bool GetChildAs(std::uint32_t id, std::uint32_t iid, void** out) override;
    bool GetChildAsRecursive(std::uint32_t id, std::uint32_t iid, void** out) override;
    cIGZWin* GetWindowFromPoint(int x, int y) override { return mgr_->GetWindowFromPoint(x, y); }
    cIGZWin* GetChildWindowFromPoint(int x, int y) override;
    cIGZWin* GetChildWindowFromCursorPoint(int x, int y) override;
    int GetW() const override { return area_.right - area_.left; }
    int GetH() const override { return area_.bottom - area_.top; }
    int GetL() const override { return area_.left; }
    int GetT() const override { return area_.top; }
    int GetR() const override { return area_.right; }
    int GetB() const override { return area_.bottom; }
    const cRZRect& GetArea() const override { return area_; }
    void GetArea(cRZRect& out) const override { out = area_; }
    const cRZRect& GetAreaAbsolute() const override { return absoluteArea_; }
    void GetAreaAbsolute(cRZRect& out) const override { out = absoluteArea_; }
    void SetW(int w) override { SetArea(area_.left, area_.top, area_.left + w, area_.bottom); }
    void SetH(int h) override { SetArea(area_.left, area_.top, area_.right, area_.top + h); }
    void SetSize(int w, int h) override { SetArea(area_.left, area_.top, area_.left + w, area_.top + h); }
    void SetArea(int l, int t, int r, int b) override;
    void SetArea(const cRZRect& area) override { SetArea(area.left, area.top, area.right, area.bottom); }
    void GZWinMoveTo(int x, int y) override { SetArea(x, y, x + GetW(), y + GetH()); }
    void GZWinOffset(int dx, int dy) override { SetArea(area_.left + dx, area_.top + dy, dx + area_.right, dy + area_.bottom); }
    bool FitRectToWindow(cRZRect& rect, int margin) override;
    void ScreenToWindowCoordinates(int& x, int& y) const override;
    void WindowToScreenCoordinates(int& x, int& y) const override { x += absoluteArea_.left; y += absoluteArea_.top; }
    bool WindowToWindowCoordinates(cIGZWin* other, int& x, int& y) const override;
    bool IsPointInWindowScreenCoordinates(int x, int y) const override;
    std::uint32_t GetID() const override { return id_; }
    void SetID(std::uint32_t id) override { id_ = id; }
    std::uint32_t GetInstanceID() const override { return instanceId_; }
    bool SetInstanceID(std::uint32_t id) override { instanceId_ = id; return true; }
    bool GetFlag(std::uint32_t flag) const override { return (flags_ & flag) != 0; }
    std::uint32_t SetFlag(std::uint32_t flag, bool on) override;
    void ShowWindow() override;
    void HideWindow() override;
    bool IsVisible() const override { return GetFlag(kWinVisible); }
    bool IsEnabled() const override { return GetFlag(kWinEnabled); }
    const cIGZString& GetCaption() const override { return caption_; }
    void SetCaption(const cIGZString& caption) override { caption_.AssignFromIGZString(caption); }
    cIGZWinKeyAccelerator* GetKeyboardAccelerator() const override { return accelerator_; }
    void SetKeyboardAccelerator(cIGZWinKeyAccelerator* accelerator) override;
    bool AccelerateKeyboardMsg(cGZMessage& message) override { return accelerator_ ? accelerator_->ProcessAccelerator(message) : false; }
    std::uint32_t GetKeyEquivalent() override { return keyEquivalent_; }
    void SetKeyEquivalent(std::uint32_t key) override { keyEquivalent_ = key; }
    bool CheckKeyEquivalent(std::uint32_t key, std::uint32_t modifiers) override { return MakeKeyEquivalent(key, modifiers) == keyEquivalent_; }
    std::uint32_t MakeKeyEquivalent(std::uint32_t key, std::uint32_t modifiers) override { return (key & 0xffff) | (modifiers << 16); }
    cIGZWin* IsChildKeyEquivalent(std::uint32_t key, std::uint32_t modifiers) override;
    bool ProcessCursorMessage(cGZMessage& message) override;
    void UpdateCursor() override;
    void SetCursor(cIGZCursor* cursor, bool updateNow) override { cursor_ = cursor; if (updateNow) UpdateCursor(); }
    void SetNotificationTarget(cIGZWin* target) override { notify_ = target; }
    cIGZWin* GetNotificationTarget() const override { return notify_; }
    bool GZPaint() override { return true; }
    bool Plot() override;
    void CalcAbsoluteArea() override;
    void InvalidateSelf() override { invalid_ = true; }
    void InvalidateSelfAndParents() override;
    void* GetDrawContext() override { return drawContext_; }
    void* GetBufferToDrawTo() override { return drawBuffer_; }
    void SetBufferToDrawTo() override;
    void SetBufferToDrawToRecursive() override;
    void SetAreaToDrawTo() override;
    void SetAreaToDrawToRecursive() override;
    const cRZRect& GetAreaToDrawTo() override { return drawArea_; }
    bool PrivateBuffer(bool enable) override;
    void* GetPrivateBuffer() override { return privateBuffer_; }
    void GetFillColor(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const override;
    void SetFillColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { fillColor_ = (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b; }
    std::uint32_t GetFillColor() const override;
    void SetFillColor(std::uint32_t nativeColor) override;
    bool GetFillColor(std::uint32_t& rgb) const override { rgb = fillColor_; return true; }
    bool SetFillColorColor(std::uint32_t rgb) override { fillColor_ = rgb; return true; }
    std::uint32_t MakeFillColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override;
    void SetFadeEffectPeriod(int fadeIn, int fadeOut) override { fadeInMs_ = fadeIn; fadeOutMs_ = fadeOut; }
    void GetFadeEffectPeriod(int& fadeIn, int& fadeOut) const override { fadeIn = fadeInMs_; fadeOut = fadeOutMs_; }
    void SetShadeColor(std::uint32_t color) override { shadeColor_ = color; }
    void GetShadeColor(std::uint32_t& color) const override { color = shadeColor_; }
    bool GetParam(std::uint32_t key, cIGZVariant** out) const override;
    bool SetParam(std::uint32_t key, cIGZVariant* value) override;
    bool EnumParams(EnumParamFn fn, void* context) override;
    bool AddMessageFilter(cIGZWinMessageFilter* filter) override;
    bool RemoveMessageFilter(cIGZWinMessageFilter* filter) override;
    void SetSize(const cRZPoint& size) override { SetSize(size.x, size.y); }
    void CenterWindowInRect(const cRZRect* rect) override { if (rect) CenterWindowInRect(*rect); }
    void CenterWindowInRect(const cRZRect& rect) override;
    bool IsPointInWindowWindowCoordinates(int x, int y) const override;
    bool IsPointInWindowParentCoordinates(int x, int y) const override;
    bool PlotComposite() override;
    bool PlotPresent() override;
    void SetFillColorRGB(std::uint32_t rgb) override { fillColor_ = rgb; }
    std::uint32_t GetFillColorRGB() const override { return fillColor_; }
    std::uint32_t ConvertPackedRGBToNative(std::uint32_t rgb) const override;
    std::uint32_t ConvertNativeToPackedRGB(std::uint32_t native) const override;

    bool GZOnCharacter(char) override { return false; }
    bool GZOnKeyDown(std::uint32_t, std::uint32_t) override { return false; }
    bool GZOnKeyUp(std::uint32_t, std::uint32_t) override { return false; }
    bool GZOnSetFocus(cIGZWin* previous) override;
    bool GZOnKillFocus(cIGZWin*) override { return false; }
    bool GZOnMouseDownL(int, int, std::uint32_t) override { return false; }
    bool GZOnMouseDownR(int, int, std::uint32_t) override { return false; }
    bool GZOnMouseUpL(int, int, std::uint32_t) override { return false; }
    bool GZOnMouseUpR(int, int, std::uint32_t) override { return false; }
    bool GZOnMouseMove(int, int, std::uint32_t) override { return false; }
    bool GZOnMouseWheel(int, int, std::uint32_t, int) override { return false; }
    bool GZOnCaptureChanged(cIGZWin*, cIGZWin*) override { return false; }
    bool GZOnMouseEnter(cIGZWin*) override { return false; }
    bool GZOnMouseExit(cIGZWin*) override { return false; }
    bool GZOnCommand(std::uint32_t, std::uint64_t) override { return false; }
    bool SendMsg(cIGZWin* target, cGZMessage& message) override;
    void SendMsg(cIGZWin* target, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) override;
    bool PostMsg(cIGZWin* target, cGZMessage& message) override;
    void PostMsg(cIGZWin* target, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) override;
    bool is_point_transparent(int x, int y) const override;
    bool create_private_buffer(int w, int h) override;

    // non-virtual helpers
    bool GZAmCapturing() { return mgr_->GZGetCapture() == this; }
    void GZSetCapture() { mgr_->GZSetCapture(this); }
    void GZReleaseCapture() { mgr_->GZReleaseCapture(this); }
    cIGZWin* GetFocus() { return mgr_->GZGetFocus(); }
    void SetFocus() { mgr_->GZSetFocus(this); }
    bool SetFocusTo(cIGZWin* window);
    cIGZCursorManager* GetCursorManager() { return mgr_->GetCursorManager(); }
    bool get_init_flag() const { return GetFlag(kWinInitialized); }
    void set_init_flag(bool on) { const_cast<cGZWin*>(this)->SetFlag(kWinInitialized, on); }
    void set_mouse_down_offset(int x, int y) { mouseDownX_ = x; mouseDownY_ = y; }
    void InvalidateAllParents();
    bool ChildAddAbsolute(cIGZWin* child);
    bool ChildRemoveAbsolute(cIGZWin* child);
    static bool lessfunc_id(cIGZWin* a, cIGZWin* b, void*) { return a->GetID() < b->GetID(); }
    static bool add_delayedplot_windows(cIGZWin* parent, std::uint32_t id, void* child, void* manager);

protected:
    struct ChildList {
        std::list<cIGZWin*> items;     // front = topmost window
        int locks = 0;
    };
    ChildList* LockChildListRead() const { ++children_->locks; return children_; }
    void UnlockChildListRead(ChildList* list) const;
    ChildList* LockChildListWrite();
    bool child_private_buffers(bool on);
    void set_fade_state(std::uint8_t alpha, int periodMs);
    bool fade_in_progress();
    void fade_advance();
    bool set_private_buffer_transparency(bool on);
    bool fill_private_buffer_with_transparent_color();
    void recalc_blt_area();

    cIGZWinMgr* mgr_;                  // +8
    cIGZKeyboard* keyboard_;           // +0x10
    std::uint32_t refCount_ = 0;       // +0x18
    std::uint32_t id_ = 0;             // +0x1c
    cRZRect absoluteArea_;             // +0x20
    cRZRect drawArea_;                 // +0x30
    cRZRect accumulatedArea_;          // +0x40
    mutable ChildList* children_;      // +0x50
    cIGZWin* parent_ = nullptr;        // +0x58
    cIGZWin* notify_ = nullptr;        // +0x60
    cRZString caption_;                // +0x68
    IWinBuffer* privateBuffer_ = nullptr;   // +0x90
    IWinBuffer* drawBuffer_ = nullptr;      // +0x98
    void* drawContext_ = nullptr;      // +0xa0
    bool invalid_ = true;              // +0xa8
    bool usesOwnBuffer_ = false;       // +0xa9
    cIGZCursor* cursor_ = nullptr;     // +0xb0
    cIGZWinKeyAccelerator* accelerator_ = nullptr;   // +0xb8
    std::map<std::uint32_t, cIGZVariant*> params_;   // +0xc0
    std::list<cIGZWinMessageFilter*> filters_;       // +0xd8 (each holds a reference)
    std::list<cIGZWinMessageFilter*> pendingRemove_; // +0xf0
    bool dispatching_ = false;         // +0x108
    int fadeInMs_ = 500, fadeOutMs_ = 500;   // +0x10c, +0x110
    std::uint32_t keyEquivalent_ = 0;  // +0x114
    int mouseDownX_ = 0, mouseDownY_ = 0;    // +0x118
    cRZRect area_;                     // +0x120
    cRZRect bltArea_;                  // +0x130
    std::uint32_t flags_ = 0x8903;     // +0x140
    std::uint32_t instanceId_ = 0;     // +0x144
    std::uint32_t shadeColor_ = 0xffffffff;   // +0x148
    std::uint32_t fillColor_ = 0;      // +0x14c
};

// The temporary window that takes over a closing window's private buffer so the fade-out can finish after the
// window itself is gone (0x10016d3fc..0x10016d4f8). Only Plot differs from cGZWin. The original constructor takes
// the window manager from the global service; here the stolen-from window's manager is passed in.
class cGZFadeTmpWin : public cGZWin {
public:
    explicit cGZFadeTmpWin(cIGZWinMgr* mgr);                      // 0x10016d3fc
    ~cGZFadeTmpWin() override = default;                          // 0x10016d4c0 / 0x10016d4ca
    bool Plot() override;                                         // 0x10016d470
    void StealBuffer(cIGZWin* source);                            // 0x10016c104
};

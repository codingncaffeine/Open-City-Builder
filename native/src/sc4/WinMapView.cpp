#include "sc4/WinMapView.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <type_traits>
#include <utility>

#include "gz/Font.h"
#include "gz/StreamOps.h"
#include "gz/WinBtn.h"
#include "gz/WinFlatRect.h"
#include "gz/WinText.h"
#include "gz/WinUtils.h"
#include "gz/cIGZMessage2.h"
#include "sc4/GrowthDeveloper.h"
#include "sc4/SimGridOps.h"

namespace sc4 {

// ---------------------------------------------------------------------------------------------------------------
// The host: a do-nothing default (no message server, no simulators) that tests replace.
// ---------------------------------------------------------------------------------------------------------------
namespace {

class cNullWinMapViewHost final : public IWinMapViewHost {
public:
    std::uint32_t NowMilliseconds() override { return 0; }
    cIGZMessageServer2* MessageServer2() override { return nullptr; }
    void AddAnimationTarget(cIGZMessageTarget2*) override {}
    void RemoveAnimationTarget(cIGZMessageTarget2*) override {}
    void SetAnimationNotifications(cIGZMessageTarget2*, std::uint32_t) override {}
    void CollectHighlights(std::uint32_t, std::vector<WinMapHighlight>&) override {}
    IWinMapViewMarkedCells* CreateMarkedCells() override { return nullptr; }
    bool AddMarkedCells(IWinMapViewMarkedCells*) override { return false; }
    bool RemoveMarkedCells(IWinMapViewMarkedCells*) override { return false; }
    bool CreateEffect(const cRZString&, IWinMapViewEffect** out) override { *out = nullptr; return false; }
    bool LoadLegendData(std::uint32_t, std::vector<std::uint32_t>&, std::vector<std::uint32_t>&,
                        std::uint32_t&) override {
        return false;
    }
    void LoadString(std::uint32_t, std::uint32_t, cRZString& out) override { out = cRZString(); }
    cIGZFont* LegendFont() override { return nullptr; }
    IWinMapViewColorSink* ViewColorSink(cIGZWin*) override { return nullptr; }
    bool OccupantLotPosition(cISC4Occupant*, float[3]) override { return false; }
    bool IsTransportationCell(int, int) override { return false; }
    int CitySizeXInCells() override { return 0; }
    int CitySizeZInCells() override { return 0; }
    cIGZBuffer* CreatePotholeDecal() override { return nullptr; }
    int MapRotation() override { return 0; }
    void SendCommand(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    bool FilterUICommand(const cGZMessage&) override { return true; }
};

cNullWinMapViewHost g_nullHost;
IWinMapViewHost* g_host = &g_nullHost;

// One reference on an interface obtained from QueryInterface (the original releases by hand).
template <class T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    Ref(Ref&& other) noexcept : p(other.p) { other.p = nullptr; }
    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            if (p) p->Release();
            p = other.p;
            other.p = nullptr;
        }
        return *this;
    }
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

Ref<gz::cIGZWinText> QueryText(cIGZWin* parent, std::uint32_t id) {
    Ref<gz::cIGZWinText> result;
    if (!parent) return result;
    cIGZWin* window = parent->GetChildWindowFromIDRecursive(id);
    void* out = nullptr;
    if (window && window->QueryInterface(gz::kGZIID_cIGZWinText, &out))
        result.p = static_cast<gz::cIGZWinText*>(out);
    return result;
}
Ref<gz::cIGZWinFlatRect> QueryFlat(cIGZWin* parent, std::uint32_t id) {
    Ref<gz::cIGZWinFlatRect> result;
    if (!parent) return result;
    cIGZWin* window = parent->GetChildWindowFromIDRecursive(id);
    void* out = nullptr;
    if (window && window->QueryInterface(gz::kGZIID_cIGZWinFlatRect, &out)) result.p = static_cast<gz::cIGZWinFlatRect*>(out);
    return result;
}
Ref<gz::cIGZWinBtn> QueryButton(cIGZWin* parent, std::uint32_t id) {
    Ref<gz::cIGZWinBtn> result;
    if (!parent) return result;
    cIGZWin* window = parent->GetChildWindowFromIDRecursive(id);
    void* out = nullptr;
    if (window && window->QueryInterface(gz::kGZIID_cIGZWinBtn, &out)) result.p = static_cast<gz::cIGZWinBtn*>(out);
    return result;
}

constexpr std::uint32_t kWinMapStringGroup = 0x6a231eaa;   // cSLString group of every legend string
constexpr std::uint32_t kWinMapTitleInstance = 0x2a5e854f; // the "no data view" title
constexpr std::uint32_t kWinMapLegendInstanceBase = 0x4a0b6800;

}  // namespace

IWinMapViewHost* WinMapViewHost() { return g_host; }
void SetWinMapViewHost(IWinMapViewHost* host) { g_host = host ? host : &g_nullHost; }

// The 82 control ids ShowWindow points at the view (0x100a1d8b0, read from the binary).
const std::uint32_t kWinMapButtonIds[82] = {
    0x4a32ca92, 0xea8b80a6, 0x0a32cac3, 0xea8b80a7, 0xea8b80a8, 0xa32cacd4, 0xca2871f1, 0xea2871e9, 0xea2871d9,
    0x00005111, 0x00005011, 0x00005110, 0x00005010, 0x0000510f, 0x0000500f, 0x0000510e, 0x0000500e, 0x0000510d,
    0x0000500d, 0x0000510c, 0x0000500c, 0x0000510b, 0x0000500b, 0x0000510a, 0x0000500a, 0x00005109, 0x00005009,
    0x00005108, 0x00005008, 0x00005107, 0x00005007, 0x00005106, 0x00005006, 0x00005105, 0x00005005, 0x00005104,
    0x00005004, 0x00005103, 0x00005003, 0x00005102, 0x00005002, 0x00005101, 0x00005001, 0x8a2871d5, 0x00006001,
    0x00006101, 0x00006002, 0x00006102, 0x00006003, 0x00006103, 0x00006004, 0x00006104, 0x00006005, 0x00006105,
    0x00006006, 0x00006106, 0x00006007, 0x00006107, 0x00006008, 0x00006108, 0x00006009, 0x00006109, 0x0000600a,
    0x0000610a, 0x0000600b, 0x0000610b, 0x0000600c, 0x0000610c, 0x00008001, 0x00008002, 0x00008003, 0x00008004,
    0x00008005, 0x00008006, 0x00008007, 0x00008008, 0x00008009, 0x0000800a, 0x0000800b, 0x0000800c, 0x00008100,
    0x00008101,
};

// ---------------------------------------------------------------------------------------------------------------
// WinMapHighlightInfo: the set element. The original stores a cRZAutoRefCount for the occupant and the effect and
// copies the position as three floats; the set's comparator only looks at the occupant pointer.
// ---------------------------------------------------------------------------------------------------------------
WinMapHighlightInfo::WinMapHighlightInfo(cISC4Occupant* occ, IWinMapViewEffect* fx, float s,
                                         const float position[3])
    : occupant(occ), effect(fx), scale(s), x(position[0]), y(position[1]), z(position[2]) {
    if (occupant) occupant->AddRef();
    if (effect) effect->AddRef();
}
WinMapHighlightInfo::WinMapHighlightInfo(const WinMapHighlightInfo& other)
    : occupant(other.occupant), effect(other.effect), scale(other.scale), x(other.x), y(other.y), z(other.z) {
    if (occupant) occupant->AddRef();
    if (effect) effect->AddRef();
}
WinMapHighlightInfo& WinMapHighlightInfo::operator=(const WinMapHighlightInfo& other) {
    if (this == &other) return *this;
    if (other.occupant) other.occupant->AddRef();
    if (other.effect) other.effect->AddRef();
    if (occupant) occupant->Release();
    if (effect) effect->Release();
    occupant = other.occupant;
    effect = other.effect;
    scale = other.scale;
    x = other.x;
    y = other.y;
    z = other.z;
    return *this;
}
WinMapHighlightInfo::~WinMapHighlightInfo() {
    if (occupant) occupant->Release();
    if (effect) effect->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// Construction / destruction.
// ---------------------------------------------------------------------------------------------------------------
cSC4WinMapView::cSC4WinMapView(cIGZWinMgr* mgr) : gz::cGZWinGen(mgr) {   // 0x1003494a6
    // The original writes every field by hand; the member initialisers cover the same stores. The palettes are
    // filled by SetDataView, exactly like the original (which leaves them untouched here).
    lastUpdateMs_ = g_host->NowMilliseconds();   // cRZTime::GetMillisecondSystemTime, +0xae8
}

cSC4WinMapView::~cSC4WinMapView() {   // 0x100349726
    // The original releases the loaded view exemplar (+0xad8), clears the two highlight sets and frees the message
    // vector and the effect name; the members do that here. The cGZWinGen base follows.
    if (viewExemplar_) {
        viewExemplar_->Release();
        viewExemplar_ = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// cIGZUnknown. The original answers 0x67c5bede with `this`, 0x452294aa with `this + 8`, and forwards the rest to
// cGZWinGen (0x1003498c8).
// ---------------------------------------------------------------------------------------------------------------
bool cSC4WinMapView::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZIID_cISC4ViewManager) {
        *out = static_cast<cISC4ViewManager*>(this);
        AddRef();
        return true;
    }
    if (iid == kGZIID_cIGZMessageTarget2_) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return gz::cGZWinGen::QueryInterface(iid, out);
}

// ---------------------------------------------------------------------------------------------------------------
// Window helpers.
// ---------------------------------------------------------------------------------------------------------------
cIGZWin* cSC4WinMapView::parentWin() const { return const_cast<cSC4WinMapView*>(this)->GetParentWin(); }
cIGZWin* cSC4WinMapView::childWindow(std::uint32_t id) const {
    cIGZWin* parent = parentWin();
    return parent ? parent->GetChildWindowFromIDRecursive(id) : nullptr;   // vslot 0x118
}
void cSC4WinMapView::setWindowVisible(std::uint32_t id, bool visible) {
    cIGZWin* parent = parentWin();
    if (parent) nsGZWinUtils::SetWindowVisible(parent, id, visible, false);
}
bool cSC4WinMapView::windowVisible(std::uint32_t id) const {
    cIGZWin* parent = parentWin();
    return parent ? nsGZWinUtils::GetWindowVisible(parent, id) : false;
}
void cSC4WinMapView::removeWatchedNotifications() {
    if (!messages_) return;
    for (std::uint32_t type : watchedMessages_) messages_->RemoveNotification(messageTarget(), type);   // vslot 0x30
    watchedMessages_.clear();
}

// ---------------------------------------------------------------------------------------------------------------
// Init (0x1003499a4) / Shutdown (0x100349b3a).
// ---------------------------------------------------------------------------------------------------------------
bool cSC4WinMapView::Init() {
    SetWinGenFlag(gz::kGenMovable, true);          // SetWinGenFlag(1, true)
    SetWinGenFlag(gz::kGenCloseButton, false);     // (0x10, false)
    SetWinGenFlag(gz::kGenCloseDisabled, true);    // (0x100, true)
    SetWinGenFlag(gz::kGenEscapeCloses, false);    // (4, false)
    gz::cGZWinGen::Init();                         // 0x10024de48
    if (!started_) {
        started_ = true;   // quirk: set before the failure check, so a failed Init never retries
        if (!messages_) {
            messages_ = g_host->MessageServer2();   // SL::spMS2
            if (!messages_) return false;
            messages_->AddRef();
            messages_->AddNotification(messageTarget(), kWinMapMsgViewActivate);     // vslot 0x28
            messages_->AddNotification(messageTarget(), kWinMapMsgViewDeactivate);
            messages_->AddNotification(messageTarget(), kWinMapMsgViewMoved);
        }
        if (!markedCells_) {
            markedCells_ = g_host->CreateMarkedCells();   // new SC4MarkedCellView, 0x78 bytes
            if (markedCells_) {
                markedCells_->Init();
                markedCells_->SetShouldIgnoreDepthBuffer(true);
                markedCells_->SetShouldUseWaterAltitude(true);
            }
        }
    }
    return true;
}

bool cSC4WinMapView::Shutdown() {
    if (started_) {
        started_ = false;
        if (cIGZWinMgr* manager = GetWindowManager()) {
            if (cIGZWin* parent = parentWin()) {
                // The seven map panels the view owns in the parent window.
                for (std::uint32_t id : {kWinMapIdMap2, kWinMapIdMap1, kWinMapIdLargePanel, 0x4200u,
                                         kWinMapIdDesirablePanel, kWinMapIdLegendPanel, kWinMapIdTrafficLegend}) {
                    if (cIGZWin* child = parent->GetChildWindowFromIDRecursive(id)) manager->DestroyWindow(child);
                }
            }
        }
        if (markedCells_) {
            if (markedCellsRegistered_) {
                g_host->RemoveMarkedCells(markedCells_);   // renderer vslot 0x108
                markedCellsRegistered_ = false;
            }
            markedCells_->Shutdown();
            delete markedCells_;   // the original releases it (a COM-style object); this one is not reference counted
            markedCells_ = nullptr;
        }
        if (messages_) {
            removeWatchedNotifications();
            messages_->RemoveNotification(messageTarget(), kWinMapMsgViewMoved);
            messages_->RemoveNotification(messageTarget(), kWinMapMsgViewDeactivate);
            messages_->RemoveNotification(messageTarget(), kWinMapMsgViewActivate);
            messages_->Release();
            messages_ = nullptr;
        }
        if (occupancyGrid_) {
            occupancyGrid_->Shutdown();
            occupancyGrid_->Release();
            occupancyGrid_ = nullptr;
        }
    }
    return gz::cGZWinGen::Shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// Message handling. The original is a cIGZMessageTarget2 registered for the three update messages (Init) and for
// the messages a data view listens to (HookUpdateMessages).
// ---------------------------------------------------------------------------------------------------------------
bool cSC4WinMapView::DoMessage(cIGZMessage2* message) {   // 0x100349df6
    if (!message) return false;
    const std::uint32_t type = message->GetType();   // vslot 0x20
    if (std::binary_search(watchedMessages_.begin(), watchedMessages_.end(), type)) dirty_ = true;
    // The original compares as a signed int, so the high message ids (0x86ad10ee, 0xa6b79602, ...) take the
    // first branch and only the 0x26d31ec1/0x26d31ec2 activate pair takes the second.
    if (static_cast<std::int32_t>(type) < static_cast<std::int32_t>(kWinMapMsgViewActivate)) {
        if (type == kWinMapMsgAnimationTick) {
            if (UpdateHighlights()) dirty_ = true;
            std::uint32_t interval;
            if (dataView_ == kWinMapViewTraffic) {
                dirty_ = true;   // the traffic route view always redraws
                interval = 0xfa;
            } else {
                interval = 1000;
                if (!dirty_) return true;
            }
            const std::uint32_t now = g_host->NowMilliseconds();
            if (interval <= now - lastUpdateMs_) Update();   // Update stores the new timestamp
        } else if (type == kWinMapMsgViewMoved) {
            // The two payload words are rectangle pointers; only their second int is compared (the original does not
            // check them for null).
            auto* standard = static_cast<cIGZMessage2Standard*>(message);
            const auto* first = static_cast<const int*>(standard->GetVoid1());   // vslot 0x50
            const auto* second = static_cast<const int*>(standard->GetVoid2());  // vslot 0x78
            if (first && second && first[1] != second[1]) dirty_ = true;
        }
    } else if (type == kWinMapMsgViewActivate) {
        g_host->AddAnimationTarget(messageTarget());   // ATM vslot 0x28
        viewActive_ = true;
    } else if (type == kWinMapMsgViewDeactivate) {
        g_host->RemoveAnimationTarget(messageTarget());   // ATM vslot 0x30
        viewActive_ = false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// UpdateHighlights (0x100349f5c): collect what the current data view wants highlighted, add the occupants to the
// new-highlight set and merge that with the active set. The simulator queries themselves are the host's
// CollectHighlights (see IWinMapViewHost).
// ---------------------------------------------------------------------------------------------------------------
bool cSC4WinMapView::UpdateHighlights() {
    std::vector<WinMapHighlight> highlights;
    g_host->CollectHighlights(highlightView_, highlights);
    for (const WinMapHighlight& highlight : highlights) {
        if (highlight.occupant) {
            AddNewHighlight(highlight.occupant, highlight.scale);
            continue;
        }
        // The crime hot spot (case 5): create the effect at once and put the record straight into the active set.
        const float position[3] = {highlight.x, highlight.y, highlight.z};
        IWinMapViewEffect* effect = nullptr;
        if (effectName_.Strlen() != 0) {
            if (g_host->CreateEffect(effectName_, &effect) && effect) {
                effect->Place(position, highlight.scale);
                effect->Activate(false);
            }
        }
        newHighlights_.insert(WinMapHighlightInfo(nullptr, effect, highlight.scale, position));
        if (effect) effect->Release();
    }
    return MergeNewHighlights();
}

// ---------------------------------------------------------------------------------------------------------------
// AddNewHighlight (0x1003507c4): the occupant's lot position (or its own) becomes the effect position; the
// occupant is told it is highlighted and the record joins the new-highlight set.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::AddNewHighlight(cISC4Occupant* occupant, float scale) {
    float position[3] = {0.0f, 0.0f, 0.0f};
    if (!g_host->OccupantLotPosition(occupant, position)) occupant->GetPosition(position);   // vslot 0x40
    occupant->SetHighlight(1, false);                                                         // vslot 0x88
    newHighlights_.insert(WinMapHighlightInfo(occupant, nullptr, scale, position));
}

// ---------------------------------------------------------------------------------------------------------------
// ActivateHighlight (0x100350fa4): create the named effect at the record's position (the original doubles the
// scale) and store it in the record.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::ActivateHighlight(WinMapHighlightInfo& info) {
    if (effectName_.Strlen() == 0) return;
    IWinMapViewEffect* effect = nullptr;
    if (!g_host->CreateEffect(effectName_, &effect) || !effect) return;
    const float position[3] = {info.x, info.y, info.z};
    effect->Place(position, info.scale + info.scale);   // the original's 2 * scale
    effect->Activate(false);                            // effect vslot 0x18, parameter 0
    info.effect = effect;                               // operator= releases the old effect and keeps this one
    effect->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// MergeNewHighlights (0x100350a1c): a two-set merge ordered by occupant. New occupants get an effect, stale ones
// are cleared, and the null-occupant (crime spot) record is refreshed in place. The original answers a changed bit;
// the port answers "the sets differed".
// ---------------------------------------------------------------------------------------------------------------
bool cSC4WinMapView::MergeNewHighlights() {
    bool changed = false;
    auto active = activeHighlights_.begin();
    auto fresh = newHighlights_.begin();
    while (active != activeHighlights_.end() && fresh != newHighlights_.end()) {
        if (fresh->occupant < active->occupant) {
            WinMapHighlightInfo info = *fresh;
            ActivateHighlight(info);
            activeHighlights_.insert(info);
            changed = true;
            ++fresh;
        } else if (active->occupant < fresh->occupant) {
            if (active->occupant) active->occupant->SetHighlight(0, false);
            active = activeHighlights_.erase(active);
            changed = true;
        } else {
            if (active->occupant == nullptr) {
                // Both records are the crime spot: replace the active one (its key does not change).
                WinMapHighlightInfo info = *fresh;
                ActivateHighlight(info);
                auto next = std::next(active);
                activeHighlights_.erase(active);
                activeHighlights_.insert(info);
                active = next;
                changed = true;
            } else {
                ++active;
            }
            ++fresh;
        }
    }
    while (fresh != newHighlights_.end()) {
        WinMapHighlightInfo info = *fresh;
        ActivateHighlight(info);
        activeHighlights_.insert(info);
        changed = true;
        ++fresh;
    }
    while (active != activeHighlights_.end()) {
        if (active->occupant) active->occupant->SetHighlight(0, false);
        active = activeHighlights_.erase(active);
        changed = true;
    }
    newHighlights_.clear();
    return changed;
}

// ---------------------------------------------------------------------------------------------------------------
// Update (0x10034a848; i386 0x0027db2e): draw the current data view's simulator grid into a new map buffer and hand
// it to the panel's minimap (child 0x4203, cISC4WinMiniMap) as its overlay; data view 0 clears the overlay.
//   PARTIAL: views 0x1d (weather) and 0x4c (traffic routes) draw nothing; crime incident icons, the lot shading (+0x9c0 colour) and the
//   highlight circles (+0x9c4 colour) are not drawn; the custom view (vslot 0x14) is not created.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::Update() {
    if (!viewActive_) return;
    dirty_ = false;
    if (useMarkedCells_ && markedCells_ && !markedCellsRegistered_) {
        // renderer vslot 0x100: AddMarkedCellView(markedCells, 5, 1000)
        if (g_host->AddMarkedCells(markedCells_)) markedCellsRegistered_ = true;
    }
    IWinMapViewColorSink* map = g_host->ViewColorSink(parentWin());
    int width = 0, height = 0;
    if (map && map->MapSize(width, height)) {
        if (dataView_ == 0) {
            map->SetOverlay(nullptr);
        } else if (cIGZBuffer* buffer = g_host->CreateMapBuffer()) {
            cGZBufferColorType type;
            type.type = kBufferColorARGB8888;   // Init(w, h, 9, 0x20)
            type.bitsPerPixel = 0x20;
            buffer->Init(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), type);
            buffer->Fill(0, nullptr);
            WinMapGrid grid;
            auto u8 = [&](int source, int argument = 0) {
                if (g_host->SimulatorGrid(source, argument, grid) && grid.u8 && !grid.u8->empty())
                    DisplayStandardUint8Grid(*grid.u8, buffer, grid.cellSizeX, grid.cellSizeZ);
            };
            auto s16 = [&](int source, int argument = 0) {
                if (g_host->SimulatorGrid(source, argument, grid) && grid.s16 && !grid.s16->empty())
                    DisplayStandardSint16GridScaled(*grid.s16, buffer, grid.cellSizeX, grid.cellSizeZ);
            };
            switch (dataView_) {
                case 1:
                    if (g_host->SimulatorGrid(kGridAuraMayorRating, 0, grid) && grid.s8 && !grid.s8->empty())
                        DisplayStandardSint8Grid(*grid.s8, buffer, grid.cellSizeX, grid.cellSizeZ);
                    break;
                case 2: u8(kGridLandValue); break;
                case 3: u8(kGridPolice); break;
                case 4: u8(kGridCrime); break;   // PARTIAL: the incident icons
                case 5: u8(kGridFireProtection); break;
                case 6: u8(kGridFlammability); break;
                case 7: u8(kGridPower); break;
                case 8: s16(kGridPollution, 0); break;
                case 9:
                    u8(kGridTraffic);
                    DrawTransportationPotholeDecals(buffer);
                    break;
                case 10: u8(kGridPlumbing); break;
                case 0xc: s16(kGridAura); break;
                case 0x18: s16(kGridPollution, 1); break;
                case 0x19: s16(kGridPollution, 2); break;
                case 0x1a: s16(kGridPollution, 3); break;
                case 0x1b:
                case 0x1c: {
                    WinMapGrid average;
                    if (g_host->SimulatorGrid(dataView_ == 0x1b ? kGridEducation : kGridHealth, 0, average) && average.f32 && !average.f32->empty())
                        DisplayStandardFloat32GridAsUint8(*average.f32, buffer, average.cellSizeX, average.cellSizeZ);
                    if (average.owner) average.owner->Release();
                    break;
                }
                case 0x1e: u8(kGridResidentialAge); break;
                case 0xb: break;   // draws nothing in the original too
                default: {
                    // The growth developer views: developer type by view, then the grid the layer style asks for.
                    static constexpr std::uint32_t kDeveloperTypes[12] = {0x1010, 0x1020, 0x1030, 0x3110, 0x3120, 0x3130,
                                                                          0x3320, 0x3330, 0x4100, 0x4200, 0x4300, 0x4400};
                    const cISC4GrowthDeveloper* developer =
                        dataView_ >= 0x40 && dataView_ <= 0x4b ? g_host->GrowthDeveloper(kDeveloperTypes[dataView_ - 0x40]) : nullptr;
                    if (!developer) {
                        // Quirk kept: the original returns here, before SetOverlay and the timestamp (and leaks the buffer).
                        buffer->Release();
                        return;
                    }
                    const std::uint32_t style = keepGridWhileUpdating_ ? 6 : layerStyle_;
                    if (std::getenv("SC4_MAPVIEWDEBUG")) {
                        const cSC4SimGrid<std::int16_t>* d = developer->GetDesirabilityMap();
                        int lo = 1 << 30, hi = -(1 << 30);
                        if (d) for (const auto& col : d->GetGridData()) for (std::int16_t v : col) { lo = std::min<int>(lo, v); hi = std::max<int>(hi, v); }
                        std::fprintf(stderr, "map view: developer view 0x%x style %u desirability %dx%d tract %.0f range %d..%d scale %lld\n", dataView_, style,
                                     d ? d->GetTractCountX() : 0, d ? d->GetTractCountZ() : 0, d ? d->GetTractWidthX() : 0.0f, lo, hi, static_cast<long long>(s16GridScale_));
                    }
                    if (style > 6) break;
                    switch (style) {
                        case 0:
                        case 1: break;
                        case 2:
                            if (const cSC4SimGrid<std::uint16_t>* g = developer->GetCapacityMap())
                                DisplayStandardUint16GridScaled(g->GetGridData(), buffer, g->GetTractWidthX(), g->GetTractWidthZ());
                            break;
                        case 3:
                        case 4:
                            if (const cSC4SimGrid<std::int16_t>* g = developer->GetDesirabilityMap())
                                DisplayStandardSint16GridScaled(g->GetGridData(), buffer, g->GetTractWidthX(), g->GetTractWidthZ());
                            break;
                        case 5:
                            UpdateOccupancyMap(developer);
                            if (occupancyGrid_)
                                DisplayStandardSint16GridScaled(occupancyGrid_->GetGridData(), buffer, occupancyGrid_->GetTractWidthX(),
                                                                occupancyGrid_->GetTractWidthZ());
                            break;
                        default: {
                            // The growth list: each listed tract shows its style x 12, on a grid shaped like the desirability map; the
                            // city-size resample is switched off meanwhile.
                            const cSC4SimGrid<std::int16_t>* shape = developer->GetDesirabilityMap();
                            if (!shape) break;
                            const bool clamp = clampGridToCity_;
                            clampGridToCity_ = false;
                            std::vector<std::vector<std::int16_t>> listed(static_cast<std::size_t>(shape->GetTractCountX()),
                                                                          std::vector<std::int16_t>(static_cast<std::size_t>(shape->GetTractCountZ()), 0));
                            if (developer->GrowthAndDeclineListsAreValid()) {
                                for (const GrowthItem& item : developer->GetGrowthList()) {
                                    if (item.x >= 0 && item.x < static_cast<int>(listed.size()) && item.z >= 0 &&
                                        item.z < static_cast<int>(listed[item.x].size()))
                                        listed[item.x][item.z] = static_cast<std::int16_t>(item.style * 12);
                                }
                                DisplayStandardSint16GridScaled(listed, buffer, shape->GetTractWidthX(), shape->GetTractWidthZ());
                            }
                            clampGridToCity_ = clamp;
                            break;
                        }
                    }
                    break;
                }
            }
            map->SetOverlay(buffer);
            if (dataView_ == kWinMapViewTraffic) map->Refresh();
            buffer->Release();
        }
    }
    lastUpdateMs_ = g_host->NowMilliseconds();   // cRZTime::GetMillisecondSystemTime, +0xae8
}

// ---------------------------------------------------------------------------------------------------------------
// Window layout of the parent's panels (0x10034d6aa / 0x10034e070 / 0x10034e12c / 0x10034e1f2).
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::ShowLargeWindow() {
    setWindowVisible(kWinMapIdLargePanel, true);
    setWindowVisible(kWinMapIdMap2, true);
    setWindowVisible(kWinMapIdLegendPanel, true);
    setWindowVisible(kWinMapIdMap1, false);
    setWindowVisible(kWinMapIdDesirablePanel, false);
    setWindowVisible(0x8100, false);
    setWindowVisible(0x8101, false);
    setWindowVisible(kWinMapIdTrafficLegend, showTrafficLegend_);
}

void cSC4WinMapView::ShowDesirableWindow() {
    setWindowVisible(kWinMapIdDesirablePanel, true);
    setWindowVisible(kWinMapIdMap2, true);
    setWindowVisible(kWinMapIdLegendPanel, true);
    setWindowVisible(kWinMapIdMap1, false);
    setWindowVisible(kWinMapIdLargePanel, false);
    setWindowVisible(kWinMapIdTrafficLegend, false);
    setWindowVisible(kWinMapIdShowTraffic, false);
    setWindowVisible(0x8100, false);
    setWindowVisible(0x8101, false);
}

void cSC4WinMapView::ShowTrafficWindow() {
    setWindowVisible(kWinMapIdDesirablePanel, true);
    setWindowVisible(kWinMapIdMap2, true);
    setWindowVisible(kWinMapIdLegendPanel, true);
    setWindowVisible(kWinMapIdMap1, false);
    setWindowVisible(kWinMapIdLargePanel, false);
    setWindowVisible(kWinMapIdTrafficLegend, false);
    setWindowVisible(kWinMapIdShowTraffic, true);
    setWindowVisible(0x8100, true);
    setWindowVisible(0x8101, true);
}

void cSC4WinMapView::ShowSmallWindow() {
    setWindowVisible(kWinMapIdMap1, true);
    setWindowVisible(kWinMapIdMap2, true);
    setWindowVisible(kWinMapIdLargePanel, false);
    setWindowVisible(kWinMapIdDesirablePanel, false);
    setWindowVisible(kWinMapIdLegendPanel, false);
    setWindowVisible(kWinMapIdTrafficLegend, false);
}

// ---------------------------------------------------------------------------------------------------------------
// Mode-button handlers (0x10034cd56 / 0x10034d75e / 0x10034d7a6).
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::OnViewModeButtonSelected(std::uint32_t id, int view) {
    cIGZWin* parent = parentWin();
    if (!parent) return;
    SC4DoButtonExclusion(parent, 0x5001, 0x11, id);
    SetBaseView(static_cast<std::uint32_t>(view));
    DisplayLegend(static_cast<std::uint32_t>(view));
    if (cIGZWin* desirable = parent->GetChildWindowFromIDRecursive(kWinMapIdDesirablePanel)) {
        if (desirable->GetFlag(kWinVisible)) ShowLargeWindow();
    }
    viewView_ = static_cast<std::uint32_t>(view);
}

void cSC4WinMapView::OnDesirabilityModeButtonSelected(std::uint32_t id, int view) {
    cIGZWin* parent = parentWin();
    if (!parent) return;
    SC4DoButtonExclusion(parent, 0x6001, 0xc, id);
    SetBaseView(static_cast<std::uint32_t>(view));
    DisplayLegend(static_cast<std::uint32_t>(view));
    desirabilityView_ = static_cast<std::uint32_t>(view);
}

void cSC4WinMapView::OnTrafficModeButtonSelected(std::uint32_t id, int view) {
    cIGZWin* parent = parentWin();
    if (!parent) return;
    SC4DoButtonExclusion(parent, 0x8003, 10, id);
    SetBaseView(static_cast<std::uint32_t>(view));
    DisplayLegend(static_cast<std::uint32_t>(view));
    trafficView_ = static_cast<std::uint32_t>(view);
}

// ---------------------------------------------------------------------------------------------------------------
// The view stack (0x100351790 / 0x1003517de / 0x100351846) and ResetDataView (0x10034e316).
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::PushView(std::uint32_t view) {
    viewStack_.push_back(view);
    ResetDataView();
}

void cSC4WinMapView::PopView(std::uint32_t view) {
    auto found = viewStack_.end();
    for (auto it = viewStack_.begin(); it != viewStack_.end(); ++it) {
        if (*it == view) found = it;   // the original removes the last matching entry
    }
    if (found == viewStack_.end()) return;
    viewStack_.erase(found);
    ResetDataView();
}

void cSC4WinMapView::SetBaseView(std::uint32_t view) {
    baseView_ = view;
    ResetDataView();
}

void cSC4WinMapView::ResetDataView() {
    std::uint32_t view;
    bool fromStack;
    if (viewStack_.empty()) {
        if (!shown_) {
            view = kWinMapViewNone;
            fromStack = true;
        } else {
            view = baseView_;
            fromStack = false;
        }
    } else {
        view = viewStack_.back();
        fromStack = true;
    }
    SetDataView(view);
    cIGZWin* parent = parentWin();
    if (!parent) return;
    if (cIGZWin* large = parent->GetChildWindowFromIDRecursive(kWinMapIdButtonView)) {
        large->SetFlag(kWinVisible, !fromStack);
    }
    if (cIGZWin* small = parent->GetChildWindowFromIDRecursive(kWinMapIdButtonSmall)) {
        small->SetFlag(kWinVisible, fromStack);
        if (fromStack) small->PullToFront();   // vslot 0xb8
    }
    if (windowVisible(kWinMapIdLegendPanel)) DisplayLegend(view);
}

// ---------------------------------------------------------------------------------------------------------------
// DisplayLegend (0x10034ce8c): hide the nine swatches/labels, set the panel title and, for a data view, fill the
// swatches and labels from its exemplar.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::DisplayLegend(std::uint32_t view) {
    for (std::uint32_t i = 0; i < 9; ++i) {
        setWindowVisible(0x8a909e10 + i, false);
        setWindowVisible(0x8a909e00 + i, false);
    }
    if (cIGZWin* first = childWindow(0x8a909e00)) first->InvalidateSelfAndParents();   // vslot 0x2e0

    cIGZWin* parent = parentWin();
    if (!parent) return;

    if (view == 0) {
        cRZString title;
        g_host->LoadString(kWinMapStringGroup, kWinMapTitleInstance, title);
        if (Ref<gz::cIGZWinText> text = QueryText(parent, kWinMapIdLegendTitle)) text->SetCaption(title);
        setWindowVisible(kWinMapIdTrafficLegend, false);
        return;
    }

    std::vector<std::uint32_t> colors, labelStrings;
    std::uint32_t titleString = 0;
    const bool loaded = g_host->LoadLegendData(view, colors, labelStrings, titleString);
    if (loaded) {
        cRZString title;
        g_host->LoadString(kWinMapStringGroup, titleString, title);
        if (Ref<gz::cIGZWinText> text = QueryText(parent, kWinMapIdLegendTitle)) text->SetCaption(title);
    }

    cIGZFont* font = g_host->LegendFont();
    int lineY = 0;
    for (std::size_t i = 0; loaded && i < colors.size(); ++i) {
        if (colors[i] == 0) continue;
        if (Ref<gz::cIGZWinFlatRect> flat = QueryFlat(parent, 0x8a909e10 + static_cast<std::uint32_t>(i))) {
            cIGZWin* flatWin = flat->AsIGZWin();
            flatWin->SetFlag(kWinVisible, true);
            cRZRect swatch{0x173, 0x3d + lineY, 0x173 + flatWin->GetW(), 0x3d + lineY + flatWin->GetH()};
            flatWin->SetArea(swatch);
            const std::uint32_t rgb = colors[i];
            flat->SetFillColor(flatWin->MakeFillColor(static_cast<std::uint8_t>(rgb >> 16),
                                                      static_cast<std::uint8_t>(rgb >> 8),
                                                      static_cast<std::uint8_t>(rgb)));
        }
        // Quirk: the original compares `i <= labels.size()`; when i == size it reads one past the vector. The port
        // substitutes instance 0 there.
        if (i <= labelStrings.size()) {
            const std::uint32_t labelInstance = i < labelStrings.size() ? labelStrings[i] : 0;
            if (Ref<gz::cIGZWinText> text = QueryText(parent, 0x8a909e00 + static_cast<std::uint32_t>(i))) {
                std::int8_t gutterX = 0, gutterY = 0;
                text->GetGutters(gutterX, gutterY);   // vslot 0xc0
                cIGZWin* textWin = text->AsIGZWin();
                cRZRect area{0x116, 0x18 + lineY, 0x116 + textWin->GetW(), 0x18 + lineY + textWin->GetH()};
                cRZString label;
                g_host->LoadString(kWinMapStringGroup, labelInstance, label);
                textWin->SetFlag(kWinVisible, true);
                textWin->SetArea(area);
                text->SetCaption(label);
                area.right += gutterX * -2;
                area.bottom += gutterY * -2;
                label.Trim();
                if (font) {
                    font->CalculateTextArea(label.Data(), static_cast<std::uint32_t>(label.Strlen()), area, true,
                                            true);   // vslot 0x168
                }
                lineY += ((area.bottom - area.top + 0x11) / 0x12) * 0x12;
            }
        }
    }

    if (view == kWinMapViewLegendTraffic) {
        showTrafficLegend_ = true;
        if (cIGZWin* large = parent->GetChildWindowFromIDRecursive(kWinMapIdLargePanel)) {
            if (large->GetFlag(kWinVisible)) setWindowVisible(kWinMapIdTrafficLegend, true);
        }
    } else {
        showTrafficLegend_ = false;
        setWindowVisible(kWinMapIdTrafficLegend, false);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// HookUpdateMessages (0x10034e426): ask the animation tick manager for the view's notification flags, pick the
// message the view listens to, sort the list and register every entry.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::HookUpdateMessages() {
    g_host->SetAnimationNotifications(messageTarget(), dataView_ == kWinMapViewTraffic ? 0xc0 : 0);   // ATM vslot 0x40
    std::uint32_t message = kWinMapMsgAnimationTick;
    switch (dataView_) {
        case 1: message = kWinMapMsgPollutionAir; break;
        case 2: message = kWinMapMsgDesirability; break;
        case 4: message = kWinMapMsgCrime; break;
        case 7: message = kWinMapMsgPower; break;
        case 8: message = kWinMapMsgPollutionAirChange; break;
        case 9:
        case kWinMapViewTraffic: message = kWinMapMsgTraffic; break;
        case 10: message = kWinMapMsgPlumbing; break;
        case 0x18: message = kWinMapMsgPollutionWaterChange; break;
        case 0x19: message = kWinMapMsgPollutionGarbageChange; break;
        case 0x1a: message = kWinMapMsgPollutionRadiationChange; break;
        case 0x1d: message = kWinMapMsgLandValue; break;
        default: break;
    }
    watchedMessages_.push_back(message);
    std::sort(watchedMessages_.begin(), watchedMessages_.end());
    if (messages_) {
        for (std::uint32_t type : watchedMessages_) messages_->AddNotification(messageTarget(), type);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The grid-display converters. The three small ones build the byte grid the recovered (partial) Uint8 display
// takes; their scaling arithmetic is the original's.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::DisplayStandardFloat32GridAsUint8(const std::vector<std::vector<float>>& grid,
                                                       cIGZBuffer* buffer, float cellSizeX, float cellSizeZ) {
    std::vector<std::vector<std::uint8_t>> bytes;
    bytes.resize(grid.size());
    for (std::size_t row = 0; row < grid.size(); ++row) {
        bytes[row].resize(grid[row].size(), 0);
        for (std::size_t col = 0; col < grid[row].size(); ++col)
            bytes[row][col] = static_cast<std::uint8_t>(static_cast<int>(grid[row][col]));
    }
    DisplayStandardUint8Grid(bytes, buffer, cellSizeX, cellSizeZ);
}

void cSC4WinMapView::DisplayStandardSint16GridScaled(const std::vector<std::vector<std::int16_t>>& grid,
                                                     cIGZBuffer* buffer, float cellSizeX, float cellSizeZ) {
    std::vector<std::vector<std::uint8_t>> bytes;
    bytes.resize(grid.size());
    for (std::size_t row = 0; row < grid.size(); ++row) {
        bytes[row].resize(grid[row].size(), 0);
        for (std::size_t col = 0; col < grid[row].size(); ++col) {
            const int scaled = static_cast<int>((static_cast<std::int64_t>(grid[row][col]) * s16GridScale_) >> 16);
            const int shifted = scaled + 0x80;
            // The original clamps through (-0x81 - scaled) >> 31: 0xff above the range, 0 below it.
            std::uint8_t value = static_cast<std::uint8_t>(scaled > -0x81 ? 0xff : 0x00);
            if (static_cast<std::uint32_t>(shifted) < 0x100) value = static_cast<std::uint8_t>(shifted);
            bytes[row][col] = value;
        }
    }
    DisplayStandardUint8Grid(bytes, buffer, cellSizeX, cellSizeZ);
}

void cSC4WinMapView::DisplayStandardUint16GridScaled(const std::vector<std::vector<std::uint16_t>>& grid,
                                                     cIGZBuffer* buffer, float cellSizeX, float cellSizeZ) {
    std::vector<std::vector<std::uint8_t>> bytes;
    bytes.resize(grid.size());
    for (std::size_t row = 0; row < grid.size(); ++row) {
        bytes[row].resize(grid[row].size(), 0);
        for (std::size_t col = 0; col < grid[row].size(); ++col) {
            const std::int64_t scaled = static_cast<std::int64_t>(grid[row][col]) * u16GridScale_;
            std::uint64_t value = static_cast<std::uint64_t>(scaled) >> 16;
            if (value > 0xfe) value = 0xff;   // quirk: 0xfe stays, everything above becomes 0xff
            bytes[row][col] = static_cast<std::uint8_t>(value);
        }
    }
    DisplayStandardUint8Grid(bytes, buffer, cellSizeX, cellSizeZ);
}

// ---------------------------------------------------------------------------------------------------------------
// The grid helpers of the module's anonymous namespace (i386 0x006e0ea8 / 0x006e1366 SC4InterpolateGrid,
// 0x006e1130 / 0x006e15f2 MakeMapColorBuffer, 0x006e1cd0 / 0x006e1a74 BlockMapByLot). Grids are [x][z].
// ---------------------------------------------------------------------------------------------------------------
namespace {
using ByteGrid = std::vector<std::vector<std::uint8_t>>;

// SC4InterpolateGrid<T, Bias>: 16.16 bilinear resample of `source` onto `out` (already sized); signed grids land
// biased by 0x80. Positions start half a step in (step / 2 - 0x8000); past either edge the edge cell is used.
template <class T, int Bias>
void InterpolateGrid(ByteGrid& out, const std::vector<std::vector<T>>& source) {
    const int srcX = static_cast<int>(source.size());
    if (srcX == 0 || out.empty()) return;
    const int srcZ = static_cast<int>(source[0].size());
    const int dstX = static_cast<int>(out.size());
    const int dstZ = static_cast<int>(out[0].size());
    if (srcZ == 0 || dstZ == 0) return;
    const int stepX = (srcX << 16) / dstX;
    const int stepZ = (srcZ << 16) / dstZ;
    int posX = stepX / 2 - 0x8000;
    for (int x = 0; x < dstX; ++x, posX += stepX) {
        const std::vector<T>* a;
        const std::vector<T>* b;
        int fracX = 0;
        if (posX < 0) {
            a = b = &source[0];
        } else if (posX >= (srcX - 1) * 0x10000) {
            a = b = &source[srcX - 1];
        } else {
            a = &source[posX >> 16];
            b = &source[(posX >> 16) + 1];
            fracX = posX & 0xffff;
        }
        std::uint32_t posZ = static_cast<std::uint32_t>(stepZ / 2 - 0x8000);
        for (int z = 0; z < dstZ && z < static_cast<int>(out[x].size()); ++z, posZ += static_cast<std::uint32_t>(stepZ)) {
            const int iz = static_cast<int>(posZ) >> 16;
            int value;
            if (posZ < static_cast<std::uint32_t>((srcZ - 1) * 0x10000)) {
                const int fz = static_cast<int>(posZ & 0xffff);
                const int va = (*a)[iz] + ((((*a)[iz + 1] - (*a)[iz]) * fz + 0x8000) >> 16);
                const int vb = (*b)[iz] + ((((*b)[iz + 1] - (*b)[iz]) * fz + 0x8000) >> 16);
                value = va + (((vb - va) * fracX + 0x8000) >> 16);
            } else {
                const int edge = iz >= 0 ? srcZ - 1 : 0;
                value = (*a)[edge] + ((((*b)[edge] - (*a)[edge]) * fracX + 0x8000) >> 16);
            }
            out[x][z] = static_cast<std::uint8_t>(value + Bias);
        }
    }
}

// MakeMapColorBuffer (orientation 0): every cell becomes a block of the smallest power-of-two size that covers the
// buffer, in the palette colour with red and blue swapped. GUARD: the original writes whole blocks even past the
// buffer edge when the buffer is not a power-of-two multiple of the grid; the port clips them.
template <class T>
void MakeMapColorBuffer(const std::vector<std::vector<T>>& grid, int rows, int cols, cIGZBuffer* buffer, int width,
                        int height, const std::uint32_t* palette) {
    int blockX = 1, blockZ = 1;
    for (int w = width; rows < w; w >>= 1) blockX <<= 1;
    for (int h = height; cols < h; h >>= 1) blockZ <<= 1;
    const auto lock = static_cast<cIGZBuffer::eLockFlags>(0x8040);
    if (!buffer->Lock(lock)) return;
    auto* bits = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    const std::uint32_t stride = buffer->GetColorSurfaceStride();
    for (int x = 0; x < rows && bits; ++x) {
        for (int z = 0; z < cols && z < static_cast<int>(grid[x].size()); ++z) {
            const std::uint8_t index = std::is_signed_v<T> ? static_cast<std::uint8_t>(static_cast<std::uint8_t>(grid[x][z]) ^ 0x80)
                                                           : static_cast<std::uint8_t>(grid[x][z]);
            const std::uint32_t c = palette[index];
            const std::uint32_t color = (((c & 0xff0000) >> 8 | (c & 0xff00) << 8 | c << 24) >> 8) | (c & 0xff000000);
            const int px = x * blockX, py = z * blockZ;
            for (int row = py; row < py + blockZ && row < height; ++row) {
                auto* line = reinterpret_cast<std::uint32_t*>(bits + static_cast<std::size_t>(row) * stride);
                for (int col = px; col < px + blockX && col < width; ++col) line[col] = color;
            }
        }
    }
    buffer->Unlock(lock);
}

// BlockMapByLot: every lot whose zone type bit is in `mask` gets its cells' rounded average (never 0: +-1) plus
// `bias`; the cells of no such lot stay 0.
template <class T>
void BlockMapByLot(ByteGrid& out, const std::vector<std::vector<T>>& source, int bias, std::uint32_t mask,
                   const std::vector<WinMapLot>& lots) {
    for (const WinMapLot& lot : lots) {
        if ((mask & (1u << (lot.zoneType & 0x1f))) == 0) continue;
        const int count = (lot.z1 - lot.z0 + 1) * (lot.x1 - lot.x0 + 1);
        if (count <= 0) continue;
        int sum = 0;
        for (int x = lot.x0; x <= lot.x1; ++x)
            for (int z = lot.z0; z <= lot.z1; ++z)
                if (x >= 0 && x < static_cast<int>(source.size()) && z >= 0 && z < static_cast<int>(source[x].size())) sum += source[x][z];
        int average;
        if (sum < 0) {
            average = (sum - count / 2) / count;
            if (average == 0) average = -1;
        } else {
            average = (sum + count / 2) / count;
            if (average == 0) average = 1;
        }
        for (int x = lot.x0; x <= lot.x1; ++x)
            for (int z = lot.z0; z <= lot.z1; ++z)
                if (x >= 0 && x < static_cast<int>(out.size()) && z >= 0 && z < static_cast<int>(out[x].size()))
                    out[x][z] = static_cast<std::uint8_t>((average + bias) & 0xff);
    }
}

ByteGrid SizedGrid(int x, int z) { return ByteGrid(static_cast<std::size_t>(std::max(x, 0)), std::vector<std::uint8_t>(static_cast<std::size_t>(std::max(z, 0)), 0)); }
constexpr float kCityCellMeters = 16.0f;   // spCity vslots 0x16c / 0x170 (only the marked-cell size reads them)
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// DisplayStandardUint8Grid (0x10034e730; i386 0x0027ce5e) / DisplayStandardSint8Grid (0x10034f10a; i386 0x0027d686):
// a grid coarser than the city is first resampled to city size when +0x9fc asks for it; with +0x9fd each lot of the
// +0xa00 zone types shows its average; the cells are then drawn into the buffer through paletteB and, once the marked
// cell view is registered, every cell whose paletteA colour has alpha is marked in that colour.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::DisplayStandardUint8Grid(const std::vector<std::vector<std::uint8_t>>& grid, cIGZBuffer* buffer,
                                              float cellSizeX, float cellSizeZ) {
    if (!buffer || grid.empty()) return;
    const int rows = static_cast<int>(grid.size());
    const int cols = static_cast<int>(grid[0].size());
    const int cityX = g_host->CitySizeXInCells(), cityZ = g_host->CitySizeZInCells();
    if (clampGridToCity_ && (rows < cityX || cols < cityZ)) {
        ByteGrid city = SizedGrid(cityX, cityZ);
        InterpolateGrid<std::uint8_t, 0>(city, grid);
        DisplayStandardUint8Grid(city, buffer, kCityCellMeters, kCityCellMeters);
        return;
    }
    const ByteGrid* use = &grid;
    ByteGrid blocked;
    if (averageGridByLots_) {
        blocked = SizedGrid(rows, cityZ);
        std::vector<WinMapLot> lots;
        g_host->CityLots(lots);
        BlockMapByLot(blocked, grid, 0, lotTypeMask_, lots);
        use = &blocked;
    }
    cRZRect area;
    buffer->GetBufferArea(area);
    MakeMapColorBuffer(*use, rows, cols, buffer, area.right - area.left, area.bottom - area.top, paletteB_);
    if (markedCellsRegistered_ && markedCells_) {
        std::vector<WinMapCell> cells;
        std::vector<std::uint32_t> colors;
        for (int x = 0; x < rows; ++x) {
            for (int z = 0; z < cols && z < static_cast<int>((*use)[x].size()); ++z) {
                const std::uint32_t color = paletteA_[(*use)[x][z]];
                if (color & 0xff000000) {
                    cells.push_back({x, z});
                    colors.push_back(color);
                }
            }
        }
        markedCells_->AddMarkedCells(cells);
        markedCells_->SetCellColors(colors);
        markedCells_->SetCellSize(cellSizeX, cellSizeZ);
    }
}

void cSC4WinMapView::DisplayStandardSint8Grid(const std::vector<std::vector<std::int8_t>>& grid, cIGZBuffer* buffer,
                                              float cellSizeX, float cellSizeZ) {
    if (!buffer || grid.empty()) return;
    const int rows = static_cast<int>(grid.size());
    const int cols = static_cast<int>(grid[0].size());
    const int cityX = g_host->CitySizeXInCells(), cityZ = g_host->CitySizeZInCells();
    if (clampGridToCity_ && (rows < cityX || cols < cityZ)) {
        ByteGrid city = SizedGrid(cityX, cityZ);
        InterpolateGrid<std::int8_t, 0x80>(city, grid);
        DisplayStandardUint8Grid(city, buffer, kCityCellMeters, kCityCellMeters);
        return;
    }
    if (averageGridByLots_) {
        // Quirk kept: the lot averages are drawn and then the raw grid is drawn over them below.
        ByteGrid blocked = SizedGrid(rows, cityZ);
        std::vector<WinMapLot> lots;
        g_host->CityLots(lots);
        BlockMapByLot(blocked, grid, 0x80, lotTypeMask_, lots);
        DisplayStandardUint8Grid(blocked, buffer, static_cast<float>(rows), static_cast<float>(cols));
    }
    cRZRect area;
    buffer->GetBufferArea(area);
    MakeMapColorBuffer(grid, rows, cols, buffer, area.right - area.left, area.bottom - area.top, paletteB_);
    if (markedCellsRegistered_ && markedCells_) {
        std::vector<WinMapCell> cells;
        std::vector<std::uint32_t> colors;
        for (int x = 0; x < rows; ++x) {
            for (int z = 0; z < cols && z < static_cast<int>(grid[x].size()); ++z) {
                const std::uint32_t color = paletteA_[static_cast<std::uint8_t>(grid[x][z] + 0x80)];
                if (color & 0xff000000) {
                    cells.push_back({x, z});
                    colors.push_back(color);
                }
            }
        }
        markedCells_->AddMarkedCells(cells);
        markedCells_->SetCellColors(colors);
        markedCells_->SetCellSize(cellSizeX, cellSizeZ);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// DrawTransportationPotholeDecals (0x10034fd86): one rotated pothole decal at the centre of every cell the traffic
// simulator reports as a road.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::DrawTransportationPotholeDecals(cIGZBuffer* buffer) {
    if (!buffer) return;
    const int cityX = g_host->CitySizeXInCells();
    const int cityZ = g_host->CitySizeZInCells();
    if (cityX <= 0) return;
    const float scale = static_cast<float>(buffer->Width()) / static_cast<float>(cityX);   // vslot 0x58
    cIGZBuffer* decal = nullptr;
    for (int x = 0; x < cityX; ++x) {
        const int pixelX = static_cast<int>(static_cast<float>(x) * scale);
        for (int z = 0; z < cityZ; ++z) {
            if (!g_host->IsTransportationCell(x, z)) continue;   // traffic sim vslot 0x80
            if (!decal) {
                decal = g_host->CreatePotholeDecal();   // spGF vslot 0x50
                if (!decal) return;
            }
            // The renderer reports the map rotation (vslot 0x80 -> object +0x114); the blit wants it negated.
            SC4MapViewRotatedAlphaBlt(buffer, decal, pixelX, static_cast<int>(static_cast<float>(z) * scale),
                                      -g_host->MapRotation());
        }
    }
    if (decal) decal->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// UpdateOccupancyMap (0x1003503fa): keep a tract grid of the occupied cells. The destination is the view's own
// 1x1 grid (+0xa98); the source is the developer's population map, and the developer's capacity map is then divided
// into it. Equal tract sizes copy, a finer destination duplicates each source tract, a coarser destination averages.
// ---------------------------------------------------------------------------------------------------------------
void cSC4WinMapView::UpdateOccupancyMap(const cISC4GrowthDeveloper* developer) {
    if (!occupancyGrid_) {
        occupancyGrid_ = new cSC4SimGrid<std::int16_t>();   // class id 0x49b9e605
        occupancyGrid_->Init();
        occupancyGrid_->SetTractSize(1);
        occupancyGrid_->SetInstanceID(0x89d5c471);
    }
    cSC4SimGrid<std::uint16_t>* capacity = developer->GetCapacityMap();   // vslot 0x70
    cSC4SimGrid<std::int16_t>* population = developer->GetPopulationMap();   // vslot 0x78
    if (!capacity || !population) return;

    auto& destination = occupancyGrid_->GetGridData();
    const int destinationShift = occupancyGrid_->GetTractShift();
    const int destinationX = occupancyGrid_->GetTractCountX();
    const int destinationZ = occupancyGrid_->GetTractCountZ();
    const auto& source = population->GetGridData();
    const int sourceShift = population->GetTractShift();
    const int sourceX = population->GetTractCountX();
    const int sourceZ = population->GetTractCountZ();

    if (destinationShift == sourceShift) {
        for (int x = 0; x < destinationX; ++x)
            for (int z = 0; z < destinationZ; ++z) destination[x][z] = source[x][z];
    } else if (destinationShift < sourceShift) {
        const int shift = sourceShift - destinationShift;
        for (int x = 0; x < sourceX; ++x) {
            for (int z = 0; z < sourceZ; ++z) {
                const std::int16_t value = source[x][z];
                const int firstX = x << shift, lastX = (x + 1) << shift;
                const int firstZ = z << shift, lastZ = (z + 1) << shift;
                for (int dx = firstX; dx < lastX; ++dx)
                    for (int dz = firstZ; dz < lastZ; ++dz) destination[dx][dz] = value;
            }
        }
    } else {
        const int shift = destinationShift - sourceShift;
        for (int x = 0; x < destinationX; ++x) {
            for (int z = 0; z < destinationZ; ++z) {
                int sum = 0;
                for (int sx = x << shift; sx < ((x + 1) << shift); ++sx)
                    for (int sz = z << shift; sz < ((z + 1) << shift); ++sz) sum += source[sx][sz];
                destination[x][z] = static_cast<std::int16_t>((sum >> shift) >> shift);
            }
        }
    }
    ProcessTwoGridsWithCallback<std::int16_t, std::uint16_t>(*occupancyGrid_, *capacity, Divide, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// SetDataView (0x100351858; i386 0x0027fc98): load the view's exemplar {0x6534284a, 0x690f693f, view + 0x4a0b6800}
// and configure the map from it (data view, palettes, grid handling, highlights), then draw it.
//   PARTIAL: the 3D side is not applied: the cSC4ComplexViewOccupantFilter (0x4a0b47e0/e1/e2, the network list
//   0x4a0b47f1 and the pipe/subway inclusion), spView3D vslot 0x5c (underground mode 0x4a0b47e3) and 0x4c (filter),
//   spTerrain 0x158/0x150, the zone manager's display (0x4a0b47e6), the building development view (0x4a0b47f0) and the
//   custom view (GZCOM class 0x4a0b47eb, iid 0x8a175d6b).
// ---------------------------------------------------------------------------------------------------------------
bool cSC4WinMapView::SetDataView(std::uint32_t view) {
    if (!viewActive_) return false;
    const std::uint32_t key = view == 0 ? 0 : view + kWinMapLegendInstanceBase;
    if (key == cachedViewKey_) return true;
    if (viewExemplar_) {
        viewExemplar_->Release();
        viewExemplar_ = nullptr;
    }
    removeWatchedNotifications();   // UnhookUpdateMessages
    if (key == 0) {
        if (markedCells_) markedCells_->ClearMarkedCells();
        dataView_ = 0;
        clampGridToCity_ = false;
        averageGridByLots_ = false;
        highlightView_ = 0;
        effectName_ = cRZString();
        UpdateHighlights();
        Update();
        if (IWinMapViewColorSink* map = g_host->ViewColorSink(parentWin())) {
            map->SetBaseColor(1, 0);   // SetFlags(1, 0)
            map->Refresh();
            map->Invalidate();
        }
        cachedViewKey_ = 0;
        return true;
    }
    WinMapViewExemplar exemplar;
    if (!g_host->LoadDataViewExemplar(key, exemplar)) return false;

    if (exemplar.hasNetworkTypes) {   // 0x6b7c5cdd, at most 8
        networkTypeCount_ = static_cast<std::uint8_t>(std::min<std::size_t>(exemplar.networkTypes.size(), 8));
        for (int i = 0; i < networkTypeCount_; ++i) networkTypes_[i] = static_cast<std::uint8_t>(exemplar.networkTypes[i]);
    }
    if (exemplar.hasRamp && !exemplar.ramp.empty()) {   // 0x4a0b47e4: the palettes
        g_host->InterpolateRamp(paletteA_, 0x100, exemplar.ramp.data(), static_cast<int>(exemplar.ramp.size()));
        for (int i = 0; i < 0x100; ++i) {   // paletteB = paletteA premultiplied by its alpha
            const std::uint32_t c = paletteA_[i], alpha = c >> 24;
            paletteB_[i] = (c & 0xff000000) + ((((c & 0xff0000) * alpha + 0x800000) >> 8) & 0xff0000) +
                           ((((c & 0xff00) * alpha + 0x8000) >> 8) & 0xff00) + (((alpha * (c & 0xff) + 0x80) >> 8) & 0xff);
        }
        for (int i = 0; i < 0x100; ++i) {   // both swapped to ABGR
            for (std::uint32_t* palette : {paletteA_, paletteB_}) {
                const std::uint32_t c = palette[i];
                palette[i] = (c & 0xffffff) >> 16 | (((c << 8) & 0xff0000) >> 8) | (((c << 8) & 0xff00) << 8) | (c & 0xff000000);
            }
        }
    }
    if (markedCells_) markedCells_->ClearMarkedCells();
    dataView_ = exemplar.hasDataView ? exemplar.dataView : 0;
    if (markedCells_) markedCells_->SetShouldUseNetworkAltitude(dataView_ == 9 || dataView_ == kWinMapViewTraffic);
    layerStyle_ = exemplar.hasLayerStyle ? exemplar.layerStyle : 1;
    averageGridByLots_ = exemplar.hasLotTypeMask;
    if (exemplar.hasLotTypeMask) lotTypeMask_ = exemplar.lotTypeMask;
    clampGridToCity_ = (exemplar.hasClampToCity && exemplar.clampToCity) || averageGridByLots_;
    u16GridScale_ = 0x100;
    s16GridScale_ = 0x100;
    paletteSteps_ = 0x100;
    if (exemplar.hasSteps) {
        const std::uint32_t steps = exemplar.steps;
        if (steps - 2u < 0xfffffeu) {
            u16GridScale_ = static_cast<std::int64_t>(0xffff00ull / steps);
            s16GridScale_ = static_cast<std::int64_t>(0x7fff00ll / static_cast<std::int32_t>(steps));
        }
        paletteSteps_ = steps;
    }
    highlightView_ = exemplar.hasHighlightView ? exemplar.highlightView : 0;
    effectName_ = exemplar.hasEffectName ? exemplar.effectName : cRZString();
    lotShadeColor_ = exemplar.hasLotShadeColor ? exemplar.lotShadeColor : 0;
    circleColor_ = exemplar.hasCircleColor ? exemplar.circleColor : 0;
    UpdateHighlights();
    Update();
    if (IWinMapViewColorSink* map = g_host->ViewColorSink(parentWin())) {
        map->SetBaseColor(1, 0);                                         // SetFlags(1, 0)
        if (exemplar.hasShowZones && exemplar.showZones) map->SetBaseColor(1, 1);   // 0x4a0b47f5
        map->Refresh();
        map->Invalidate();
    }
    cachedViewKey_ = key;
    HookUpdateMessages();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// DoMessage(cGZMessage&) (0x10034d7ee): the command router of the parent's buttons.
// ---------------------------------------------------------------------------------------------------------------
namespace {
struct ViewModeCommand {
    std::uint32_t command;
    int view;
};
// 0x5001..0x5011 with the data views the original pairs them with (0x5101..0x5111 use the same entries).
constexpr ViewModeCommand kViewModeCommands[] = {
    {0x5001, 0x00}, {0x5002, 0x05}, {0x5003, 0x03}, {0x5004, 0x1b}, {0x5006, 0x0a}, {0x5007, 0x07},
    {0x5009, 0x20}, {0x500a, 0x02}, {0x500b, 0x01}, {0x500c, 0x1e}, {0x500d, 0x1c}, {0x500e, 0x08},
    {0x500f, 0x18}, {0x5010, 0x19}, {0x5011, 0x1a},
};
struct DesirabilityCommand {
    std::uint32_t command;
    int view;
};
constexpr DesirabilityCommand kDesirabilityCommands[] = {
    {0x6001, 0x28}, {0x6002, 0x29}, {0x6003, 0x2a}, {0x6004, 0x32}, {0x6005, 0x33}, {0x6006, 0x34},
    {0x6007, 0x35}, {0x6008, 0x36}, {0x6009, 0x3c}, {0x600a, 0x3d}, {0x600b, 0x3e}, {0x600c, 0x3f},
};
constexpr DesirabilityCommand kTrafficCommands[] = {
    {0x8003, 0x4c}, {0x8004, 0x4d}, {0x8005, 0x4e}, {0x8006, 0x4f}, {0x8007, 0x50}, {0x8008, 0x51},
    {0x8009, 0x52}, {0x800a, 0x53}, {0x800b, 0x54}, {0x800c, 0x55},
};
constexpr std::uint32_t kCommandFamily = 0x287259f6;   // DoMessage's command type word
}  // namespace

bool cSC4WinMapView::DoMessage(cGZMessage& message) {
    if (message.type != kGZMsgCommand) return gz::cGZWinGen::DoMessage(message);
    if (!g_host->FilterUICommand(message)) return true;   // veto: emitted as handled
    if (message.param1 != kCommandFamily) return gz::cGZWinGen::DoMessage(message);
    const std::uint32_t command = static_cast<std::uint32_t>(message.param2);
    cIGZWin* parent = parentWin();

    // The 0x5001..0x5011 family and its 0x5101..0x5111 aliases (the alias keeps the button id 0x50xx).
    if (command >= 0x5001 && command <= 0x5111) {
        const std::uint32_t base = command >= 0x5101 ? command - 0x100 : command;
        if (base == 0x5005) {
            if (!parent) return true;
            SC4DoButtonExclusion(parent, 0x5001, 0x11, 0x5005);
            ShowDesirableWindow();
            SetBaseView(desirabilityView_);
            DisplayLegend(desirabilityView_);
            viewView_ = desirabilityView_;
            return gz::cGZWinGen::DoMessage(message);
        }
        if (base == 0x5008) {
            if (!parent) return true;
            SC4DoButtonExclusion(parent, 0x5001, 0x11, 0x5008);
            ShowTrafficWindow();
            Ref<gz::cIGZWinBtn> small = QueryButton(parent, 0x8001);
            if (!small || !small->IsOn()) {   // vslot 0x40
                SetBaseView(trafficView_);
                DisplayLegend(trafficView_);
                viewView_ = trafficView_;
            } else {
                SetBaseView(9);
                DisplayLegend(9);
                viewView_ = 9;
            }
            return gz::cGZWinGen::DoMessage(message);
        }
        for (const ViewModeCommand& entry : kViewModeCommands) {
            if (entry.command == base) {
                OnViewModeButtonSelected(entry.command, entry.view);
                return gz::cGZWinGen::DoMessage(message);
            }
        }
        return gz::cGZWinGen::DoMessage(message);
    }

    // 0x6001..0x600c and their 0x6101..0x610c aliases.
    const std::uint32_t desirabilityCommand = (command >= 0x6101 && command <= 0x610c) ? command - 0x100 : command;
    for (const DesirabilityCommand& entry : kDesirabilityCommands) {
        if (entry.command == desirabilityCommand) {
            OnDesirabilityModeButtonSelected(entry.command, entry.view);
            return gz::cGZWinGen::DoMessage(message);
        }
    }

    // 0x8001 / 0x8002 / 0x8003..0x800c.
    if (command == 0x8001) {
        if (parent) {
            SC4DoButtonExclusion(parent, 0x8001, 2, 0x8001);
            EnableChildWinSequence(parent, 0, 0, true);
        }
        SetBaseView(9);
        DisplayLegend(9);
        return gz::cGZWinGen::DoMessage(message);
    }
    if (command == 0x8002) {
        if (parent) {
            SC4DoButtonExclusion(parent, 0x8001, 2, 0x8002);
            EnableChildWinSequence(parent, 1, 0, true);
        }
        SetBaseView(trafficView_);
        DisplayLegend(trafficView_);
        return gz::cGZWinGen::DoMessage(message);
    }
    for (const DesirabilityCommand& entry : kTrafficCommands) {
        if (entry.command == command) {
            OnTrafficModeButtonSelected(entry.command, entry.view);
            return gz::cGZWinGen::DoMessage(message);
        }
    }

    // 0x8100 / 0x8101: the traffic-route overlay buttons.
    if (command == 0x8100 || command == 0x8101) {
        if (parent) SC4DoButtonExclusion(parent, 0x8100, 2, command);
        trafficRoutes_ = command == 0x8101;
        ResetDataView();
        return gz::cGZWinGen::DoMessage(message);
    }

    switch (command) {
        case kWinMapIdViewSmall2:      // 0x0a32cac3
        case kWinMapIdViewSmall3:      // 0xa32cacd4
        case kWinMapIdViewContext2:    // 0xea8b80a7
        case 0xea8b80a8:
            ShowSmallWindow();
            break;
        case kWinMapIdViewSmall1:      // 0x4a32ca92 and 0xea8b80a6: toggle the data-view panel
        case kWinMapIdViewContext1: {
            Ref<gz::cIGZWinBtn> desirable = parent ? QueryButton(parent, 0x5005) : Ref<gz::cIGZWinBtn>{};
            if (!desirable || !desirable->IsOn()) {
                if (dataView_ == 9) {
                    ShowTrafficWindow();
                    SetBaseView(9);
                    DisplayLegend(9);
                } else if (dataView_ == kWinMapViewTraffic) {
                    ShowTrafficWindow();
                    SetBaseView(trafficView_);
                    DisplayLegend(trafficView_);
                } else {
                    ShowLargeWindow();
                    SetBaseView(viewView_);
                    DisplayLegend(viewView_);
                }
            } else {
                ShowDesirableWindow();
                SetBaseView(desirabilityView_);
                DisplayLegend(desirabilityView_);
            }
            break;
        }
        case kWinMapIdShowTraffic:     // 0xcb72f247
            ShowTrafficWindow();
            break;
        case kWinMapIdRefresh:         // 0x8a2871d5
            g_host->SendCommand(0x6a935cf1, 0, 0);   // command server vslot 0x98
            break;
        case kWinMapIdHide1:
        case kWinMapIdHide2:
            HideWindow();
            return gz::cGZWinGen::DoMessage(message);
        case kWinMapIdRefresh2:
        case kWinMapIdRefresh3:
            UpdateHighlights();
            Update();
            ShowLargeWindow();
            return true;
        default:
            break;
    }
    return gz::cGZWinGen::DoMessage(message);
}

// ---------------------------------------------------------------------------------------------------------------
// IsVisible (0x10035261c) / InitWindow (0x10035266e) / ShowWindow (0x1003527b0) / HideWindow (0x100352a6e).
// ---------------------------------------------------------------------------------------------------------------
bool cSC4WinMapView::IsVisible() const {
    cIGZWin* window = childWindow(kWinMapIdMap2);
    return window && window->GetFlag(kWinVisible);   // vslot 0x218 with flag 1
}

void cSC4WinMapView::InitWindow() {
    IWinMapViewColorSink* sink = g_host->ViewColorSink(parentWin());
    if (!sink) return;
    sink->SetBaseColor(0xffffffff, 0);   // vslot 0x40
    sink->ResetColors();                 // vslot 0x30
    sink->SetColor(1, 0x00ff00);
    sink->SetColor(2, 0x44cc22);
    sink->SetColor(3, 0x1e911e);
    sink->SetColor(4, 0x5b81d0);
    sink->SetColor(5, 0x3d6eb8);
    sink->SetColor(6, 0x3c42ad);
    sink->SetColor(7, 0xd0d030);
    sink->SetColor(8, 0xc6b733);
    sink->SetColor(9, 0xbc9f37);
}

void cSC4WinMapView::ShowWindow() {
    cIGZWin* parent = parentWin();
    if (!parent) return;
    if (!windowVisible(kWinMapIdLegendPanel)) setWindowVisible(kWinMapIdMap1, true);
    setWindowVisible(kWinMapIdMap2, true);
    for (std::uint32_t id : kWinMapButtonIds) {
        if (cIGZWin* child = parent->GetChildWindowFromIDRecursive(id)) child->SetNotificationTarget(this);   // vslot 0x2b0
    }
    DisplayLegend(0);
    if (desirabilityView_ == 99) {
        for (std::uint32_t id : {0x5001u, 0x6001u, 0x8001u, 0x8100u, 0x8003u}) {
            void* out = nullptr;
            if (parent->GetChildAsRecursive(id, gz::kGZIID_cIGZWinBtn, &out)) {
                static_cast<gz::cIGZWinBtn*>(out)->ToggleOn();   // vslot 0x30
                static_cast<gz::cIGZWinBtn*>(out)->Release();
            }
        }
        EnableChildWinSequence(parent, 0, 0, true);
        desirabilityView_ = 0x28;
    }
    shown_ = true;
    ResetDataView();
}

void cSC4WinMapView::HideWindow() {
    cIGZWin* parent = parentWin();
    if (!parent) return;
    if (!parent->GetChildWindowFromIDRecursive(kWinMapIdMap1)) return;
    setWindowVisible(0x4200, false);
    setWindowVisible(kWinMapIdMap1, false);
    setWindowVisible(kWinMapIdMap2, false);
    setWindowVisible(kWinMapIdLargePanel, false);
    setWindowVisible(kWinMapIdDesirablePanel, false);
    setWindowVisible(kWinMapIdLegendPanel, false);
    setWindowVisible(kWinMapIdTrafficLegend, false);
    shown_ = false;
    ResetDataView();
}

// ---------------------------------------------------------------------------------------------------------------
// The free functions of the module.
// ---------------------------------------------------------------------------------------------------------------
void SC4DoButtonExclusion(cIGZWin* parent, std::uint32_t firstId, std::uint32_t count, std::uint32_t selectedId) {
    if (!parent) return;
    for (std::uint32_t i = 0; i < count; ++i) {
        void* out = nullptr;
        if (!parent->GetChildAsRecursive(firstId + i, gz::kGZIID_cIGZWinBtn, &out)) continue;
        auto* button = static_cast<gz::cIGZWinBtn*>(out);
        if (selectedId == firstId + i) button->ToggleOn();    // vslot 0x30
        else button->ToggleOff();                             // vslot 0x38
        button->Release();
    }
}

void EnableChildWinSequence(cIGZWin* parent, std::uint32_t enabled, std::uint32_t unused, bool unusedBool) {
    (void)unused;
    (void)unusedBool;
    if (!parent) return;
    for (std::uint32_t i = 0; i < 10; ++i) {
        void* out = nullptr;
        if (!parent->GetChildAsRecursive(0x8003 + i, kGZIID_cIGZWin, &out)) continue;
        auto* window = static_cast<cIGZWin*>(out);
        window->SetFlag(kWinEnabled, enabled != 0);   // vslot 0x220 with flag 2
        window->Release();
    }
}

void SC4MapViewRotatedAlphaBlt(cIGZBuffer* destination, cIGZBuffer* source, int x, int y, int rotation) {
    if (!destination || !source) return;
    const cRZRect area = *source->GetBufferArea();   // vslot 0x68
    const int left = area.left, top = area.top;
    // The original swaps the span ends for the odd rotations.
    int spanX = area.bottom, spanY = area.right;
    if ((rotation & 1) == 0) {
        spanX = area.right;
        spanY = area.bottom;
    }
    const int extentX = spanX - left;
    const int extentY = spanY - top;

    // The step vectors (the original's uVar3 / uVar4 / uVar6 at 0x1003500xx). ax is the byte-offset x step and
    // the stride y step, bx the stride x step, ay the byte-offset y step.
    const std::uint32_t quarter = static_cast<std::uint32_t>(rotation) & 3;
    int ax = 0, bx = 0, ay = 0;
    switch (quarter) {
        case 0: ax = 1; bx = 0; ay = 0; break;
        case 1: ax = 0; bx = 1; ay = -1; break;
        case 2: ax = -1; bx = 0; ay = 0; break;
        default: ax = 0; bx = -1; ay = 1; break;
    }

    const int destLeft = x - extentX / 2;
    const int destTop = y - extentY / 2;
    const int imageLeft = left + destLeft;   // the original's iVar19 after it adds the destination offset
    const int imageTop = top + destTop;      // the original's iVar16
    const cRZRect destinationArea = *destination->GetBufferArea();
    const int fromX = std::max(destinationArea.left, imageLeft);
    const int fromY = std::max(destinationArea.top, imageTop);
    const int toX = std::min(destinationArea.right, imageLeft + extentX);
    const int toY = std::min(destinationArea.bottom, imageTop + extentY);
    if (toX <= fromX || toY <= fromY) return;

    // The pixel offsets of the rotation (iVar20 / local_40 in the original).
    int columnOffset = 0, rowOffset = 0;
    if (quarter == 1) {
        columnOffset = extentX - 1;
    } else if (quarter == 2) {
        columnOffset = extentX - 1;
        rowOffset = extentY - 1;
    } else if (quarter == 3) {
        rowOffset = extentY - 1;
    }

    if (!source->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) return;
    const int sourceStride = static_cast<int>(source->GetColorSurfaceStride());   // vslot 0x128
    auto* sourceBits = static_cast<std::uint8_t*>(source->GetColorSurfaceBits()); // vslot 0x120
    if (!destination->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) {
        source->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
        return;
    }
    const int destinationStride = static_cast<int>(destination->GetColorSurfaceStride());
    auto* destinationBits = static_cast<std::uint8_t*>(destination->GetColorSurfaceBits());

    const std::int64_t dx = fromX - imageLeft, dy = fromY - imageTop;
    auto* sourcePixel = reinterpret_cast<std::uint32_t*>(
        sourceBits + (dx * ax + dy * ay + static_cast<std::int64_t>(columnOffset)) * 4 +
        (static_cast<std::int64_t>(bx) * dx + dy * ax + static_cast<std::int64_t>(rowOffset)) * sourceStride);
    auto* destinationRow = reinterpret_cast<std::uint32_t*>(destinationBits + fromX * 4 + fromY * destinationStride);

    const std::ptrdiff_t columnStep = static_cast<std::ptrdiff_t>(bx) * sourceStride + ax * 4;
    const std::ptrdiff_t rowStep = static_cast<std::ptrdiff_t>(ax) * sourceStride + ay * 4;
    for (int row = fromY; row < toY; ++row) {
        std::uint32_t* destinationPixel = destinationRow;
        auto* sample = sourcePixel;
        for (int column = fromX; column < toX; ++column) {
            const std::uint32_t src = *sample;
            const std::uint32_t dst = *destinationPixel;
            const std::uint32_t alpha = src >> 24;
            std::uint32_t alphaOut = ((dst >> 24) + alpha) - (((dst >> 24) * alpha + 0x80) >> 8);
            if (alphaOut > 0xfe) alphaOut = 0xff;
            const std::uint32_t green = ((src & 0xff00) * alpha + 0x8000 + (dst & 0xff00) * (0x100 - alpha)) & 0xff0000;
            const std::uint32_t redBlue =
                ((src & 0xff00ff) * alpha + 0x800080 + (dst & 0xff00ff) * (0x100 - alpha)) & 0xff00ff00;
            *destinationPixel = ((green | redBlue) >> 8) | (alphaOut << 24);
            sample = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(sample) + columnStep);
            ++destinationPixel;
        }
        destinationRow = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(destinationRow) +
                                                          destinationStride);
        sourcePixel = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(sourcePixel) + rowStep);
    }
    destination->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
    source->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
}

}  // namespace sc4

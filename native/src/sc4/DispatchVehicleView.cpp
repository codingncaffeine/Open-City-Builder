#include "sc4/DispatchVehicleView.h"

#include <bit>
#include <cmath>
#include <iterator>

#include "gz/Buffer.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "s3d/ModelProcessor.h"
#include "sc4/DrawContext.h"

namespace sc4 {

namespace {

IDispatchVehicleViewHost* g_host = nullptr;

// _nCurrentHandle: one counter for every view, pre-incremented by TrackOccupant (0x0019e5fd).
std::uint32_t g_currentHandle = 0;

constexpr std::uint32_t kGZIID_cIGZUnknown = 1;
constexpr std::uint32_t kGZIID_MessageTarget2 = 0x452294aa;

// the float constants of the class (i386 addresses of the literal pool)
constexpr float kOne = 1.0f;                                   // 0x00a706d8 (named kfMaxStoplightDistance)
constexpr float kHalf = 0.5f;                                  // 0x00a706b4
constexpr float kMsToSeconds = 0.001f;                         // 0x00a70798 (named kfMinTravelDistance)
constexpr float kOneOverFadeTimeSeconds = 2.0f;                // _kfOneOverFadeTimeSeconds
constexpr float kRemoveAlpha = 0.01f;                          // 0x00a706d0 (named sfBarrelVelocityScalingFactor)
constexpr float kSelectableAlpha = 0.9f;                       // 0x00a707d0
constexpr float kHalfPi = std::bit_cast<float>(0x3fc90fdbu);   // 0x00a70884
constexpr float kDegreesToRadians = std::bit_cast<float>(0x3c8efa35u);   // _kDegreesToRadians / 0x00a70840
constexpr float kDegRotPerSec = 7.0f;                          // Update3DModelIndicator::DegRotPerSec (0x00ab9340)
constexpr float kHalf2DArrowSize = 21.0f;                      // _kfHalf2DArrowSize / 0x00a70910
constexpr float k2DArrowSize = 42.0f;                          // 0x00a70914
constexpr float kNudgeStepDistanceScale = 1.75f;               // 0x00a70918
constexpr float kNudgeStep = 2.0f;                             // 0x00a7072c (named _kfMaxStoplightRatio)
constexpr float kNudgeAround = 43.0f;                          // 0x00a7091c
constexpr float kClusterRadius = 50.0f;                        // 0x00a7079c (named cSC4Helicopter ... kDeltaAltitude)
constexpr std::int32_t kViewEdgeInset = 0x20;
constexpr float kModelLift = 10.0f;                            // 0x00a707d4
constexpr float kArrowAutoFrameRate = std::bit_cast<float>(0x3c449ba6u);   // TrackOccupant case 6
// the player vehicle colour by damage ratio: the healthy colour, then the wrecked one (r, g, b)
constexpr float kDamageColorHealthy[3] = {0.12890625f, 0.89453125f, 0.09765625f};   // 0x00a70908 / 0x00a70900 / 0x00a708f8
constexpr float kDamageColorWrecked[3] = {0.921875f, 0.0546875f, 0.05859375f};     // 0x00a7090c / 0x00a70904 / 0x00a708fc
constexpr std::uint32_t kAutomataFlagRemoved = 0x1;            // CheckAutomataFlag(1) in Update
constexpr std::uint32_t kAutomataFlagWrecked = 0x8000000;      // CheckAutomataFlag(0x8000000) in Update3DModelIndicator

// the cIGZWin ids sEnumChildWindows never treats as a clip window
constexpr std::uint32_t kUnclippedWindowIds[3] = {0xaa32bce6, 0xea8cad14, 0x4c30e4fa};

// nsTexUtil::GetNearestPowerOfTwo: the smallest power of two >= value (1 for value <= 1).
int NearestPowerOfTwo(int value) {
    int candidate = 1;
    while (candidate < value) candidate += candidate;
    return candidate;
}

// cS3DMatrix3::RotateY (0x0062d1f6): column 0 becomes c * col0 - s * col2, column 2 becomes c * col2 + s * col0.
void RotateY(float m[3][3], float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    for (int row = 0; row < 3; ++row) {
        const float x = m[row][0], z = m[row][2];
        m[row][0] = c * x - s * z;
        m[row][2] = z * c + x * s;
    }
}

// cS3DMatrix3::From(0, 0, angle) (0x0065efdc) with the first two angles zero: a rotation about z.
void FromZ(float m[3][3], float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    m[0][0] = c;
    m[0][1] = -s;
    m[0][2] = 0.0f;
    m[1][0] = s;
    m[1][1] = c;
    m[1][2] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 1.0f;
}

// cS3DTransform::TransformVector (0x0062d0c2) / TransformPoint (0x0065fdcc): rotation rows times the vector, times the scale
// (plus the translation for a point).
void TransformVector(const cS3DTransform& t, float& x, float& y, float& z) {
    const float rx = t.rotation[0][0] * x + t.rotation[0][1] * y + t.rotation[0][2] * z;
    const float ry = t.rotation[1][0] * x + t.rotation[1][1] * y + t.rotation[1][2] * z;
    const float rz = t.rotation[2][0] * x + t.rotation[2][1] * y + t.rotation[2][2] * z;
    x = t.scale * rx;
    y = t.scale * ry;
    z = t.scale * rz;
}
void TransformPoint(const cS3DTransform& t, float& x, float& y, float& z) {
    TransformVector(t, x, y, z);
    x += t.translation[0];
    y += t.translation[1];
    z += t.translation[2];
}

std::uint32_t BindingTexture(const IPylonTextureBinding* binding) {
    // the original reads the texture id through the binding without a null check
    return binding ? binding->TextureId() : 0;
}

}  // namespace

IDispatchVehicleViewHost* DispatchVehicleViewHost() { return g_host; }
void SetDispatchVehicleViewHost(IDispatchVehicleViewHost* host) { g_host = host; }

// ---- TrackInfo ------------------------------------------------------------------------------------------------------------

// 0x006a5f1c: a field copy; the model gains a reference, the processor pointer is shared (the map's record owns it).
cSC4DispatchVehicleView::TrackInfo::TrackInfo(const TrackInfo& other)
    : handle(other.handle), type(other.type), occupant(other.occupant), texture(other.texture), callNumber(other.callNumber),
      pictureId(other.pictureId), label(other.label), binding(other.binding), width(other.width), height(other.height),
      lastRect(other.lastRect), lastCenter(other.lastCenter), buttonState(other.buttonState), alpha(other.alpha),
      flags(other.flags), model(other.model), flashTime(other.flashTime), flashPeriod(other.flashPeriod),
      processor(other.processor) {
    for (int i = 0; i < 4; ++i) {
        numberQuad[i] = other.numberQuad[i];
        iconQuad[i] = other.iconQuad[i];
    }
    if (model) model->AddRef();
}

// 0x006a4f7a: drops the model reference (the occupant and the processor are released by RemoveTrackInfo / Shutdown).
cSC4DispatchVehicleView::TrackInfo::~TrackInfo() {
    if (model) model->Release();
}

// ---- construction ---------------------------------------------------------------------------------------------------------

// 0x0019b53e: everything zero, the per-type counts cleared (the maxima are set by Init).
cSC4DispatchVehicleView::cSC4DispatchVehicleView() = default;

// 0x0019b324: the containers only (Shutdown releases what the records hold).
cSC4DispatchVehicleView::~cSC4DispatchVehicleView() = default;

// 0x0019e68e: 0x452294aa answers the message target view; everything else goes to cRZUnknown (iid 1).
bool cSC4DispatchVehicleView::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_MessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == kGZIID_cIGZUnknown) *out = static_cast<IDispatchVehicleView*>(this);
    if (!*out) return false;
    AddRef();
    return true;
}

std::uint32_t cSC4DispatchVehicleView::AddRef() { return ++refs_; }

std::uint32_t cSC4DispatchVehicleView::Release() {
    if (refs_ > 1) return --refs_;
    delete this;
    return 0;
}

// 0x0019af2a
bool cSC4DispatchVehicleView::Init() {
    if (initialized_) return true;
    IDispatchVehicleViewHost* host = g_host;
    if (host && host->RendererPresent()) {
        host->RendererGetDrawContext();
        states_[0].PackRenderState(true, true, true, true, true, 1, true, 3, 4, 5, 4, 0.0f, 1, -1);
        states_[1].PackRenderState(true, false, false, true, true, 1, true, 3, 4, 5, 4, 0.0f, 1, -1);
        states_[2].PackRenderState(true, false, false, false, false, 1, true, -1, -1, -1, -1, -1.0f, 1, -1);
    }
    for (IPylonTextureBinding*& binding : bindings_) binding = nullptr;
    for (int slot = 0; slot < 7; ++slot)
        bindings_[slot] = host ? host->CreateTextureBinding(kDispatchViewIconInstance[slot]) : nullptr;
    for (std::uint32_t& max : max_) max = 0xffffffffu;
    if (host && host->Messages())
        host->Messages()->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDispatchViewMessageRebuild);
    initialized_ = true;
    return true;
}

// 0x0019c486
bool cSC4DispatchVehicleView::Shutdown() {
    if (!initialized_) return true;
    IDispatchVehicleViewHost* host = g_host;
    if (host && host->Messages())
        host->Messages()->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDispatchViewMessageRebuild);
    for (auto& [type, info] : tracks_) {
        if (info.texture != 0) {
            if (host) host->ReleaseTexture(info.texture);
            info.texture = 0;
        }
        if (info.model) {
            s3d::cIS3DModelInstanceAnimated* model = info.model;
            info.model = nullptr;
            model->Release();
        }
        if (info.processor) {
            delete info.processor;
            info.processor = nullptr;
        }
        if (info.occupant) info.occupant->Release();
    }
    tracks_.clear();
    for (IPylonTextureBinding*& binding : bindings_) {
        if (binding) {
            binding->Release();
            binding = nullptr;
        }
    }
    initialized_ = false;
    return true;
}

// 0x0019c634
void cSC4DispatchVehicleView::HandleViewChanged() {
    for (auto& [type, info] : tracks_) BuildCallNumberDisplay(info);
}

// 0x0019c048
bool cSC4DispatchVehicleView::DoMessage(cIGZMessage2* message) {
    if (message && message->GetType() == kDispatchViewMessageRebuild)
        for (auto& [type, info] : tracks_) BuildCallNumberDisplay(info);
    return true;
}

// ---- small queries --------------------------------------------------------------------------------------------------------

// 0x0019ae08
void cSC4DispatchVehicleView::SetIndicatorMax(int trackerType, std::uint32_t max) { max_[trackerType] = max; }

// 0x0019ae1a
bool cSC4DispatchVehicleView::IsSelectableIndicator(const TrackInfo& info) const {
    const std::uint32_t flags = info.flags;
    return (flags & 0x4) != 0 && (flags & 0x20) != 0 && (flags & 0x10) == 0 && kSelectableAlpha <= info.alpha &&
           hidden_.count(info.type) == 0;
}

// 0x0019c6e4
cSC4DispatchVehicleView::TrackMap::iterator cSC4DispatchVehicleView::FindTrackInfoByHandle(std::uint32_t handle) {
    auto it = tracks_.begin();
    while (it != tracks_.end() && it->second.handle != handle) ++it;
    return it;
}

const cSC4DispatchVehicleView::TrackInfo* cSC4DispatchVehicleView::FindTrackInfo(std::uint32_t handle) const {
    for (const auto& [type, info] : tracks_)
        if (info.handle == handle) return &info;
    return nullptr;
}

// 0x0019c66e: the last selectable indicator (in map order) whose last screen rect holds the point.
std::uint32_t cSC4DispatchVehicleView::FindIndicatorHandle(std::int32_t x, std::int32_t y) {
    std::uint32_t found = 0;
    for (const auto& [type, info] : tracks_) {
        if (IsSelectableIndicator(info) && info.lastRect.left <= x && info.lastRect.top <= y && x <= info.lastRect.right &&
            y <= info.lastRect.bottom)
            found = info.handle;
    }
    return found;
}

// 0x0019c712: null for an unknown handle and for an empty rect.
const DispatchViewRect* cSC4DispatchVehicleView::GetLastScreenIconRect(std::uint32_t handle) {
    auto it = FindTrackInfoByHandle(handle);
    if (it == tracks_.end()) return nullptr;
    const DispatchViewRect& rect = it->second.lastRect;
    if (rect.right == rect.left || rect.bottom == rect.top) return nullptr;
    return &rect;
}

// 0x0019c758
cISC4Occupant* cSC4DispatchVehicleView::GetActiveOccupant() {
    auto it = FindTrackInfoByHandle(active_);
    return it == tracks_.end() ? nullptr : it->second.occupant;
}

// 0x0019c788: turning the flash on sets the period and pulls the running flash time back under it.
void cSC4DispatchVehicleView::FlashIndicator(std::uint32_t handle, bool on, float period) {
    auto it = FindTrackInfoByHandle(handle);
    if (it == tracks_.end()) return;
    TrackInfo& info = it->second;
    if (on) {
        info.flags |= 0x100;
        info.flashPeriod = period;
        if (period < info.flashTime) info.flashTime = period;
    } else {
        info.flags &= ~0x100u;
    }
}

// 0x0019c7fc
void cSC4DispatchVehicleView::LockIndicator(std::uint32_t handle, bool lock) {
    auto it = FindTrackInfoByHandle(handle);
    if (it == tracks_.end()) return;
    if (lock) it->second.flags |= 0x80;
    else it->second.flags &= ~0x80u;
}

// 0x0019c848: the previous active indicator goes back to button state 1; the new one must be selectable.
void cSC4DispatchVehicleView::SetActiveIndicator(std::uint32_t handle, std::uint32_t buttonState) {
    if (active_ == handle) return;
    if (active_ != 0) {
        auto it = FindTrackInfoByHandle(active_);
        if (it != tracks_.end()) {
            it->second.buttonState = 1;
            BuildCallNumberDisplay(it->second);
        }
        active_ = 0;
    }
    if (handle == 0) return;
    auto it = FindTrackInfoByHandle(handle);
    if (it == tracks_.end() || !IsSelectableIndicator(it->second)) return;
    active_ = handle;
    it->second.buttonState = buttonState;
    BuildCallNumberDisplay(it->second);
}

// 0x0019c8fc: of an occupant's indicators (not fading out, of a shown type) the highest tracker type wins flag 0x800;
// every indicator of the occupant is then flagged against that type. A null occupant re-sorts every tracked occupant.
void cSC4DispatchVehicleView::SortIndicatorPriority(cISC4Occupant* occupant) {
    if (!occupant) {
        std::set<cISC4Occupant*> occupants;
        for (const auto& [type, info] : tracks_)
            if (info.occupant && occupants.count(info.occupant) == 0) occupants.insert(info.occupant);
        for (cISC4Occupant* each : occupants) SortIndicatorPriority(each);
        return;
    }
    int top = 0;
    for (const auto& [type, info] : tracks_) {
        if (info.occupant == occupant && (info.flags & 0x10) == 0 && hidden_.count(info.type) == 0)
            top = info.type <= top ? top : info.type;
    }
    for (auto& [type, info] : tracks_) {
        if (info.occupant != occupant) continue;
        if (info.type == top) info.flags |= 0x800;
        else info.flags &= ~0x800u;
    }
}

// 0x0019ca5e
void cSC4DispatchVehicleView::ShowAllIndicators(int trackerType, bool show) {
    if (!show) {
        if (hidden_.count(trackerType) == 0) hidden_.insert(trackerType);
    } else if (hidden_.count(trackerType) != 0) {
        hidden_.erase(trackerType);
    }
    SortIndicatorPriority(nullptr);
}

// 0x00801bfc: the multimap is searched with the handle's low byte as the tracker-type key; the flag argument is unused.
bool cSC4DispatchVehicleView::UseAlternateModel(std::uint32_t handle, bool alternate) {
    (void)alternate;
    auto it = tracks_.find(static_cast<std::uint8_t>(handle));
    if (it == tracks_.end()) return false;
    it->second.flags |= 0x400;
    return true;
}

// ---- tracking -------------------------------------------------------------------------------------------------------------

// 0x0019cace
void cSC4DispatchVehicleView::RemoveTrackInfo(TrackMap::iterator it, TrackInfo& info) {
    IDispatchVehicleViewHost* host = g_host;
    if (count_[info.type] != 0) --count_[info.type];
    if (info.texture != 0 && host) host->ReleaseTexture(info.texture);
    IDispatchViewAutomaton* automaton = host ? host->QueryAutomaton(info.occupant) : nullptr;
    if (automaton) automaton->SetAutomataFlag(kDispatchViewTrackedAutomataFlag, false);
    if (info.model) {
        s3d::cIS3DModelInstanceAnimated* model = info.model;
        info.model = nullptr;
        model->Release();
    }
    if (info.processor) {
        delete info.processor;
        info.processor = nullptr;
    }
    info.occupant->Release();
    tracks_.erase(it);
    if (automaton) automaton->Release();
}

// 0x0019cbd8: a fading indicator is only marked (Update removes it once transparent) unless forced.
bool cSC4DispatchVehicleView::Untrack(std::uint32_t handle, bool force) {
    if (handle == 0) return false;
    auto it = FindTrackInfoByHandle(handle);
    if (it == tracks_.end()) return false;
    if (force || (it->second.flags & 0x8) == 0) {
        cISC4Occupant* occupant = it->second.occupant;
        if (occupant) occupant->AddRef();
        RemoveTrackInfo(it, it->second);
        SortIndicatorPriority(occupant);
        if (occupant) occupant->Release();
    } else {
        it->second.flags |= 0x10;
    }
    return true;
}

// 0x0019e1ea
int cSC4DispatchVehicleView::TrackOccupant(cISC4Occupant* occupant, int trackerType, std::uint32_t value, const cRZString* label) {
    // the original indexes its seven-entry tables with the type unchecked
    if (!occupant || trackerType < 0 || trackerType >= kDispatchViewTrackerTypes) return 0;
    if (max_[trackerType] != 0 && max_[trackerType] <= count_[trackerType]) return 0;
    IDispatchVehicleViewHost* host = g_host;

    TrackInfo info;
    info.type = trackerType;
    info.occupant = occupant;
    info.flashPeriod = kOne;
    info.flags = 0x28;
    switch (trackerType) {
    case kTrackerFire:
        info.binding = bindings_[0];
        info.callNumber = value;
        info.flags = 0x2a;
        break;
    case kTrackerPolice:
        info.binding = bindings_[1];
        info.callNumber = value;
        info.flags = 0x2a;
        break;
    case kTrackerDispatchOther:
        info.binding = bindings_[6];
        info.callNumber = value;
        info.flags = 0x2a;
        break;
    case kTrackerMySim:
        if (label) {
            info.label.Copy(*label);
            info.pictureId = 0;
        } else {
            info.pictureId = value;
        }
        info.binding = bindings_[2];
        info.alpha = kOne;
        info.flags = (info.flags & ~0x8u) | 0x2;
        break;
    case kTrackerSituation:
        if (label) {
            info.label.Copy(*label);
            info.pictureId = 0;
        } else {
            info.pictureId = value;
        }
        info.binding = bindings_[4];
        info.flags |= 0x6;
        info.buttonState = 1;
        break;
    case kTrackerSituationTarget:
        info.binding = bindings_[5];
        info.flags = 0x28;
        break;
    case kTrackerPlayerDrive: {
        info.flags = 0x68;
        const cGZPersistResourceKey key{kDispatchViewArrowModelKey[0], kDispatchViewArrowModelKey[1], kDispatchViewArrowModelKey[2]};
        s3d::cIS3DModelInstanceAnimated* animated = nullptr;
        if (host && host->GetAnimatedModelFromResourceKey(key, &animated) && animated) {
            s3d::cIS3DModelInstance* model = animated->AsModelInstance();
            model->SetOwner(occupant);
            model->SetProperties(1);
            int zoom = 0, rotation = 0;
            host->GetZoomAndRotation(zoom, rotation);
            animated->SetZoomAndRotation(zoom, rotation);
            animated->AddRef();
            info.model = animated;
            auto* processor = new s3d::cS3DModelProcessor();
            info.processor = processor;
            processor->SetModel(static_cast<s3d::cIS3DModelInstance*>(animated));
            info.flags |= 0x200;
            s3d::SC4DrawAttributes attributes = model->GetDrawAttributes();
            attributes.flags |= 0x10;
            model->SetDrawAttributes(attributes);
            cS3DBoundingBox box;
            model->GetWorldBounds(box);
            info.height = std::fabs(box.max[1] - box.min[1]);
            animated->SetAutoFrameRate(kArrowAutoFrameRate);
            animated->Release();
        }
        break;
    }
    default:
        break;
    }

    const std::uint32_t occupantType = occupant->GetType();
    if (occupantType == kDispatchViewClusteredOccupantType) {
        info.flags |= 0x1;
    } else {
        info.flags &= ~0x1u;
        if (occupantType == kDispatchViewAutomatonTypeA || occupant->GetType() == kDispatchViewAutomatonTypeB) {
            if (IDispatchViewAutomaton* automaton = host ? host->QueryAutomaton(occupant) : nullptr) {
                automaton->SetAutomataFlag(kDispatchViewTrackedAutomataFlag, true);
                automaton->Release();
            }
        }
    }
    occupant->AddRef();
    BuildCallNumberDisplay(info);
    ++count_[trackerType];
    info.handle = ++g_currentHandle;
    tracks_.emplace(static_cast<std::uint8_t>(trackerType), info);
    SortIndicatorPriority(occupant);
    return static_cast<int>(info.handle);
}

// ---- the call number / picture quad ---------------------------------------------------------------------------------------

// 0x0019b622: (re)builds the number or picture texture and the two camera-facing quads: the icon (64 viewport units square,
// kCallNumberYOffset above the occupant) and, for an indicator with a number / picture, the number quad centred on the icon.
void cSC4DispatchVehicleView::BuildCallNumberDisplay(TrackInfo& info) {
    IDispatchVehicleViewHost* host = g_host;
    if ((info.flags & 0x40) != 0 || !host) return;
    if (info.texture != 0) {
        host->ReleaseTexture(info.texture);
        info.texture = 0;
    }

    float numberZ = 0.0f;        // the number quad's z
    float iconZ = 0.0f;          // the icon quad's z
    int u0 = 0, u1 = 0;          // the number quad's texel columns
    int widthPixels = 0;
    float heightPixels = 0.0f;
    float textureSize = 0.0f;
    if (static_cast<std::uint32_t>(info.type - kTrackerMySim) < 2) {
        cIGZBuffer* buffer = nullptr;
        if (info.pictureId == 0) {
            if (!host->LoadBitmapFromFile(info.label, &buffer) || !buffer) return;
        } else if (!host->LoadPictureBitmap(info.pictureId, &buffer) || !buffer) {
            return;
        }
        host->UploadTexture(buffer, info.texture);
        const int width = static_cast<int>(buffer->Width());
        const int height = static_cast<int>(buffer->Height());
        buffer->Release();
        if (info.type == kTrackerSituation) {
            // the CSI bitmap holds four button states side by side
            info.width = 35.0f;
            info.height = 35.0f;
            const int size = NearestPowerOfTwo(width);
            const float quarter = static_cast<float>(static_cast<int>(static_cast<float>(width) * 0.25f));
            const float start = static_cast<float>(static_cast<std::uint8_t>(info.buttonState)) * quarter;
            u0 = static_cast<int>(start + 0.0f);
            u1 = static_cast<int>(quarter + start);
            heightPixels = static_cast<float>(height);
            widthPixels = u1 - u0;
            textureSize = static_cast<float>(size);
            numberZ = 0.3f;
            iconZ = 0.0f;
        } else {
            info.width = 32.0f;
            info.height = 32.0f;
            heightPixels = static_cast<float>(height);
            numberZ = 0.0f;
            iconZ = 0.3f;
            u0 = 0;
            textureSize = 64.0f;
            u1 = width;
            widthPixels = width;
        }
    } else if ((info.flags & 0x2) == 0) {
        info.width = 0.0f;
        info.height = 0.0f;
        iconZ = 0.0f;
        numberZ = 0.0f;
        u1 = 1;
        u0 = 0;
        heightPixels = kOne;
        widthPixels = 1;
        textureSize = 32.0f;
    } else {
        // the call number: "%d" at font size 3 into an ARGB8888 power-of-two texture
        int width = 0, height = 0, size = 0x20;
        if (IPylonFont* font = host->CallNumberFont()) {
            font->AddRef();
            const int previousSize = font->GetFontSize();
            font->SetFontSize(kPylonCallNumberFontSize);
            cRZString text;
            text.Sprintf("%d", static_cast<int>(info.callNumber));
            width = font->TextWidth(text.ToChar(), text.Strlen());
            height = font->TextHeight(text.ToChar(), text.Strlen());
            if (width >= 0x21 || height >= 0x21) size = NearestPowerOfTwo(height < width ? width : height);
            cIGZBuffer* buffer = nullptr;
            if (host->CreateBuffer(&buffer) && buffer) {
                const cGZBufferColorType colorType{kBufferColorARGB8888, 0x20};
                if (buffer->Init(static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size), colorType)) {
                    if (buffer->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8001))) {
                        buffer->Fill(0, nullptr);
                        buffer->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8001));
                    }
                    font->DrawText(buffer, 0, 0, text.ToChar(), text.Strlen(), 0xff000000u, 0x08000000u, nullptr, 0);
                    host->UploadTexture(buffer, info.texture);
                }
                buffer->Release();
            }
            font->SetFontSize(previousSize);
            font->Release();
        }
        info.width = static_cast<float>(width);
        info.height = 14.0f;
        heightPixels = static_cast<float>(height);
        textureSize = static_cast<float>(size);
        numberZ = 0.3f;
        iconZ = 0.0f;
        u0 = 0;
        u1 = width;
        widthPixels = width;
    }

    int zoom = 0, rotation = 0;
    host->GetZoomAndRotation(zoom, rotation);
    const float yOffset = host->ViewportUnitsToWorldUnits(kDispatchViewCallNumberYOffset[zoom]) / std::cos(host->ViewPitchAngle());
    const float iconWidth = host->ViewportUnitsToWorldUnits(64.0f);
    const float iconHeight = iconWidth / std::cos(host->ViewPitchAngle());
    const float iconHalfWidth = kHalf * iconWidth;
    float iconHalfHeight = kHalf * iconHeight;
    const float numberHeight = host->ViewportUnitsToWorldUnits(heightPixels);
    const float numberHalfWidth = host->ViewportUnitsToWorldUnits(static_cast<float>(widthPixels)) * kHalf;
    const float numberHalfHeight = (numberHeight / std::cos(host->ViewPitchAngle())) * kHalf;
    const float texel = kOne / textureSize;
    if ((info.flags & 0x2) != 0) {
        iconHalfHeight = yOffset + iconHalfHeight;   // the icon's centre
        const float top = numberHalfHeight + iconHalfHeight;
        const float bottom = iconHalfHeight - numberHalfHeight;
        const float uLeft = static_cast<float>(u0) * texel, uRight = static_cast<float>(u1) * texel;
        const float vTop = texel * 0.0f, vBottom = heightPixels * texel;
        info.numberQuad[0] = DispatchViewVertex{-numberHalfWidth, top, numberZ, uLeft, vTop};
        info.numberQuad[1] = DispatchViewVertex{-numberHalfWidth, bottom, numberZ, uLeft, vBottom};
        info.numberQuad[2] = DispatchViewVertex{numberHalfWidth, bottom, numberZ, uRight, vBottom};
        info.numberQuad[3] = DispatchViewVertex{numberHalfWidth, top, numberZ, uRight, vTop};
    }
    const float iconTop = yOffset + iconHeight;
    info.iconQuad[0] = DispatchViewVertex{-iconHalfWidth, iconTop, iconZ, 0.0f, 0.0f};
    info.iconQuad[1] = DispatchViewVertex{-iconHalfWidth, yOffset, iconZ, 0.0f, 1.0f};
    info.iconQuad[2] = DispatchViewVertex{iconHalfWidth, yOffset, iconZ, 1.0f, 1.0f};
    info.iconQuad[3] = DispatchViewVertex{iconHalfWidth, iconTop, iconZ, 1.0f, 0.0f};

    // face the camera: a yaw of atan2(-dir.x, -dir.z) about y
    float dirX = 0.0f, dirZ = 0.0f;
    host->ViewDirection(dirX, dirZ);
    cS3DTransform facing;
    RotateY(facing.rotation, std::atan2(-dirX, -dirZ));
    facing.flags |= 4;
    ++facing.changeCounter;
    for (int i = 0; i < 4; ++i) {
        TransformVector(facing, info.iconQuad[i].x, info.iconQuad[i].y, info.iconQuad[i].z);
        if ((info.flags & 0x2) != 0) TransformVector(facing, info.numberQuad[i].x, info.numberQuad[i].y, info.numberQuad[i].z);
    }
}

// ---- the window clip list -------------------------------------------------------------------------------------------------

// sEnumChildWindows (0x0019b398): a visible child smaller than the 3-D view in either direction, not one of the three
// unclipped windows and not fading, blocks every priority.
bool cSC4DispatchVehicleView::EnumChildWindow(const cRZRect& parentArea, const DispatchViewChildWindow& child,
                                              std::list<DispatchViewRectInfo>& clipList) {
    if (!child.visible) return true;
    const DispatchViewRectInfo rect{child.area.left, child.area.top, child.area.right, child.area.bottom, 0xff};
    const bool smaller = rect.right - rect.left < parentArea.right - parentArea.left ||
                         rect.bottom - rect.top < parentArea.bottom - parentArea.top;
    if (smaller && child.id != kUnclippedWindowIds[0] && child.id != kUnclippedWindowIds[1] && child.id != kUnclippedWindowIds[2] &&
        !child.fading)
        clipList.push_back(rect);
    return true;
}

// 0x0019b4ba: the 3-D view's own rect first (Update takes the view edge from it), then the child windows.
void cSC4DispatchVehicleView::BuildWindowClipList() {
    IDispatchVehicleViewHost* host = g_host;
    cRZRect area;
    if (!host || !host->View3DWinArea(area)) return;
    clipList_.clear();
    clipList_.push_back(DispatchViewRectInfo{area.left, area.top, area.right, area.bottom, 0xff});
    std::vector<DispatchViewChildWindow> children;
    host->View3DChildWindows(children);
    for (const DispatchViewChildWindow& child : children) EnumChildWindow(area, child, clipList_);
}

// 0x0019ae6a
bool cSC4DispatchVehicleView::TestOverlap(const DispatchViewRectInfo& rect, const std::list<DispatchViewRectInfo>& list,
                                          const DispatchViewRect& bounds, DispatchViewRectInfo& hit) {
    if (rect.left < bounds.left || rect.top < bounds.top || bounds.right < rect.right || bounds.bottom < rect.bottom) {
        hit = DispatchViewRectInfo{bounds.left, bounds.top, bounds.right, bounds.bottom, 0xff};
        return true;
    }
    for (const DispatchViewRectInfo& entry : list) {
        if ((entry.priority == 0xff || entry.priority == rect.priority) && entry.left <= rect.right && rect.left <= entry.right &&
            entry.top <= rect.bottom && rect.top <= entry.bottom) {
            hit = entry;
            return true;
        }
    }
    return false;
}

// ---- drawing --------------------------------------------------------------------------------------------------------------

// 0x0019c098: the player-drive arrow: coloured by the vehicle's damage, lit, lifted 10 units over the occupant plus half its
// height, turned 7 degrees further and drawn with the indicator's alpha.
void cSC4DispatchVehicleView::Update3DModelIndicator(SC4DrawContext* context, TrackInfo& info, float elapsed) {
    IDispatchVehicleViewHost* host = g_host;
    if (!info.occupant || !info.model || !host) return;
    s3d::cIS3DModelInstance* model = info.model->AsModelInstance();
    float ratio = 0.0f;
    IDispatchViewAutomaton* automaton = host->QueryAutomaton(info.occupant);
    if (automaton) {
        std::uint32_t stateValue = 0;
        if (automaton->StateAndGroup(stateValue)) {
            float limit = 0.0f;
            const bool hasParams = automaton->PlayerDriveParam5c(limit);
            if (automaton->CheckAutomataFlag(kAutomataFlagWrecked)) ratio = kOne;
            else if (hasParams && limit != 0.0f) ratio = static_cast<float>(stateValue) / limit;
        }
    }
    if ((info.flags & 0x200) != 0) {
        const float healthy = kOne - ratio;
        const float blue = ratio * kDamageColorWrecked[2] + healthy * kDamageColorHealthy[2];
        const float green = ratio * kDamageColorWrecked[1] + healthy * kDamageColorHealthy[1];
        const float red = ratio * kDamageColorWrecked[0] + healthy * kDamageColorHealthy[0];
        s3d::SC4DrawAttributes attributes = model->GetDrawAttributes();
        attributes.shade[0] = red;
        attributes.shade[1] = green;
        attributes.shade[2] = blue;
        attributes.shade[3] = 1.0f;
        model->SetDrawAttributes(attributes);
    }
    // the original lights unconditionally and so assumes SetModel found the model's meshes; the port skips a processor
    // without them instead of dereferencing the missing mesh
    if (info.processor && info.processor->MeshAndMaterial()) info.processor->LightModel(host->LightingManager(), false);
    float position[3] = {0.0f, 0.0f, 0.0f};
    info.occupant->GetPosition(position);
    int zoom = 0, rotation = 0;
    host->GetZoomAndRotation(zoom, rotation);
    info.model->SetZoomAndRotation(zoom, rotation);
    position[1] = kModelLift + position[1] + kHalf * info.height;

    cS3DTransform transform;
    model->GetTransform(transform);
    // cS3DTransform::ClearTranslation (0x006a50f4)
    transform.translation[0] = transform.translation[1] = transform.translation[2] = 0.0f;
    transform.flags &= 0xfe;
    ++transform.changeCounter;
    transform.translation[0] += position[0];
    transform.translation[1] += position[1];
    transform.translation[2] += position[2];
    transform.flags |= 1;
    ++transform.changeCounter;
    RotateY(transform.rotation, kDegreesToRadians * kDegRotPerSec);
    transform.flags |= 4;
    ++transform.changeCounter;
    model->SetTransform(transform);
    info.model->SetOpacity(info.alpha < kOne, info.alpha);
    info.model->DoAutoFrameUpdate(elapsed);
    info.model->SetSelfIlluminated(true);
    model->Draw(context, false);
    if (automaton) automaton->Release();
}

// 0x0019cc8e: the draw pass (view-object list 5): fades and flashes every indicator, drops the faded-out ones, and draws the
// top-priority indicator of each occupant (up to the type's maximum) either as a billboard over an on-screen automaton or
// as a 2-D arrow at the view edge pointing at the occupant.
bool cSC4DispatchVehicleView::Update(SC4DrawContext* context) {
    IDispatchVehicleViewHost* host = g_host;
    if (!host || !context) return true;
    const float elapsed = host->StopTickTimer();
    const float fade = kMsToSeconds * elapsed * kOneOverFadeTimeSeconds;
    if (host->RenderSnapshot(*context)) return true;

    std::map<std::uintptr_t, std::uint8_t> clusterCounts;   // the clustered occupants seen this frame
    std::list<DispatchViewRectInfo> placed;                  // the windows plus the arrows drawn this frame
    if (host->CurrentViewLevel() != 1) return true;
    const bool emergencyPaused = host->SimulatorEmergencyPaused();
    std::uint32_t drawn[kDispatchViewTrackerTypes] = {};
    BuildWindowClipList();
    // i386 0x0019ce03: the copy starts at the list's second entry: the 3-D view's own rect (the first) is the edge, not a
    // blocker
    if (!clipList_.empty())
        for (auto rect = std::next(clipList_.begin()); rect != clipList_.end(); ++rect) placed.push_back(*rect);

    for (auto it = tracks_.begin(); it != tracks_.end();) {
        auto current = it++;
        TrackInfo& info = current->second;
        float position[3] = {0.0f, 0.0f, 0.0f};   // the occupant, later its viewport point
        float minimum[3] = {0.0f, 0.0f, 0.0f};    // the indicator's screen rect corners
        float maximum[3] = {0.0f, 0.0f, 0.0f};

        std::uint32_t flags = info.flags;
        if ((flags & 0x8) != 0) {
            if ((flags & 0x10) == 0) {
                float alpha = fade + info.alpha;
                if (kOne <= alpha) alpha = kOne;
                info.alpha = alpha;
            } else {
                float alpha = info.alpha - fade;
                if (alpha <= 0.0f) alpha = 0.0f;
                info.alpha = alpha;
                if (alpha < kRemoveAlpha) {
                    cISC4Occupant* occupant = info.occupant;
                    if (occupant) occupant->AddRef();
                    RemoveTrackInfo(current, info);
                    SortIndicatorPriority(occupant);
                    if (occupant) occupant->Release();
                    continue;
                }
            }
        }
        if ((flags & 0x100) != 0) {
            const float time = kMsToSeconds * elapsed + info.flashTime;
            info.flashTime = time;
            if (info.flashPeriod < time) info.flashTime = 0.0f;
            info.alpha = std::cos((info.flashTime / info.flashPeriod) * kHalfPi);
            flags = info.flags;
        }
        if ((flags & 0x20) == 0 || (flags & 0x800) == 0) continue;
        if (hidden_.count(info.type) != 0) continue;
        const int type = info.type;
        if (type == kTrackerSituation && emergencyPaused) continue;
        if (!(drawn[type] < max_[type])) continue;
        ++drawn[type];

        flags = info.flags;
        if ((flags & 0x10) == 0) {
            if (IDispatchViewAutomaton* automaton = host->QueryAutomaton(info.occupant)) {
                if (automaton->CheckAutomataFlag(kAutomataFlagRemoved)) info.flags |= 0x10;
                automaton->Release();
            }
            flags = info.flags;
        }
        if ((flags & 0x40) != 0) {
            Update3DModelIndicator(context, info, elapsed);
            continue;
        }

        info.occupant->GetPosition(position);
        const float ground = host->TerrainLocationInBounds(position[0], position[2]) ? host->TerrainAltitude(position[0], position[2])
                                                                                     : host->TerrainSeaLevel();
        if (position[1] <= ground) position[1] = ground;
        cS3DTransform world;
        world.translation[0] += position[0];
        world.translation[1] += position[1];
        world.translation[2] += position[2];
        world.flags |= 1;
        ++world.changeCounter;

        if ((info.flags & 0x80) != 0) {
            minimum[0] = static_cast<float>(info.lastRect.left);
            minimum[1] = static_cast<float>(info.lastRect.top);
            maximum[0] = static_cast<float>(info.lastRect.right);
            maximum[1] = static_cast<float>(info.lastRect.bottom);
        } else if ((info.flags & 0x1) == 0) {
            // the billboard's top-left and bottom-right corners, projected
            float a[3] = {info.iconQuad[0].x, info.iconQuad[0].y, info.iconQuad[0].z};
            float b[3] = {info.iconQuad[2].x, info.iconQuad[2].y, info.iconQuad[2].z};
            TransformPoint(world, a[0], a[1], a[2]);
            TransformPoint(world, b[0], b[1], b[2]);
            minimum[0] = a[0];
            minimum[1] = a[1];
            minimum[2] = a[2];
            maximum[0] = b[0];
            maximum[1] = b[1];
            maximum[2] = b[2];
            host->ProjectPointToViewportCoordinates2D(minimum);
            host->ProjectPointToViewportCoordinates2D(maximum);
        } else {
            // a clustered occupant (a building): its indicators sit on a 50-unit circle round it, 72 degrees apart
            const auto key = reinterpret_cast<std::uintptr_t>(info.occupant);
            std::uint8_t index;
            auto found = clusterCounts.find(key);
            if (found == clusterCounts.end()) {
                clusterCounts.emplace(key, 1);
                index = 1;
            } else {
                index = static_cast<std::uint8_t>(found->second + 1);
                found->second = index;
            }
            host->ProjectPointToViewportCoordinates2D(position);
            const float angle = static_cast<float>(static_cast<int>(index) * 0x48 - 0x5a) * kDegreesToRadians;
            const float dx = std::cos(angle) * kClusterRadius;
            const float dy = std::sin(angle);
            minimum[0] = (dx + position[0]) - kHalf2DArrowSize;
            minimum[1] = (dy * kClusterRadius + position[1]) - kHalf2DArrowSize;
            maximum[0] = minimum[0] + k2DArrowSize;
            maximum[1] = minimum[1] + k2DArrowSize;
        }

        const float rectWidth = maximum[0] - minimum[0];
        const float rectHeight = maximum[1] - minimum[1];
        // the view edge: the 3-D view's rect (the clip list's first entry) inset by 32
        const DispatchViewRectInfo view = clipList_.empty() ? DispatchViewRectInfo{} : clipList_.front();
        const DispatchViewRect bounds{view.left + kViewEdgeInset, view.top + kViewEdgeInset, view.right - kViewEdgeInset,
                                      view.bottom - kViewEdgeInset};
        const float boundsLeft = static_cast<float>(bounds.left);
        bool billboard = false;
        if ((info.flags & 0x80) != 0) {
            billboard = true;
        } else {
            bool inside;
            if (boundsLeft <= minimum[0]) {
                const float boundsRight = static_cast<float>(bounds.right);
                if (boundsRight < maximum[0]) {
                    minimum[0] = boundsRight - rectWidth;
                    maximum[0] = boundsRight;
                    inside = false;
                } else {
                    inside = true;
                }
            } else {
                maximum[0] = rectWidth + boundsLeft;
                minimum[0] = boundsLeft;
                inside = false;
            }
            if (static_cast<float>(bounds.top) <= minimum[1]) {
                if (maximum[1] <= static_cast<float>(bounds.bottom)) {
                    if (inside) billboard = true;
                } else {
                    minimum[1] = static_cast<float>(bounds.bottom) - rectHeight;
                }
            } else {
                minimum[1] = static_cast<float>(bounds.top);
            }
        }

        if (billboard) {
            const std::uint32_t current_flags = info.flags;
            if ((current_flags & 0x1) == 0 && info.type != kTrackerSituation && (current_flags & 0x2) == 0) {
                // on screen: the icon as a billboard over the occupant, a faint pass then the full one
                context->ClearTexTransform(0);
                context->SetModelTransform(world);
                context->SetTransparency(kHalf * info.alpha);
                context->SetRenderState(states_[1]);
                context->SetTexture(static_cast<int>(BindingTexture(info.binding)), 0);
                context->DrawPrims(6, 2, 4, info.iconQuad);
                if ((info.flags & 0x2) != 0) {
                    context->SetTexture(static_cast<int>(info.texture), 0);
                    context->DrawPrims(6, 2, 4, info.numberQuad);
                }
                context->SetTransparency(info.alpha);
                context->SetRenderState(states_[0]);
                context->SetTexture(static_cast<int>(BindingTexture(info.binding)), 0);
                context->DrawPrims(6, 2, 4, info.iconQuad);
                if ((info.flags & 0x2) != 0) {
                    context->SetTexture(static_cast<int>(info.texture), 0);
                    context->DrawPrims(6, 2, 4, info.numberQuad);
                }
                context->ResetTransparency();
                context->ResetModelViewTransform();
                info.lastRect = DispatchViewRect{static_cast<std::int32_t>(minimum[0]), static_cast<std::int32_t>(minimum[1]),
                                                 static_cast<std::int32_t>(maximum[0]), static_cast<std::int32_t>(maximum[1])};
                info.lastCenter = cS3DVector3{world.translation[0], world.translation[1], world.translation[2]};
                continue;
            }
        }

        // the 2-D arrow: a 42-unit square at the clamped corner, walked towards the view centre until it overlaps
        // nothing of its priority (or, when it hits another arrow, until one of the four 43-unit steps round it is free)
        maximum[0] = minimum[0] + k2DArrowSize;
        maximum[1] = k2DArrowSize + minimum[1];
        const std::int32_t originalBottom = static_cast<std::int32_t>(maximum[1]);
        const std::int32_t originalRight = static_cast<std::int32_t>(maximum[0]);
        const std::int32_t originalTop = static_cast<std::int32_t>(minimum[1]);
        const std::int32_t originalLeft = static_cast<std::int32_t>(minimum[0]);
        DispatchViewRectInfo rect{originalLeft, originalTop, originalRight, originalBottom, kDispatchViewTrackerClipPriority[type]};
        float centerY = static_cast<float>(originalTop) + kHalf2DArrowSize;
        float centerX = static_cast<float>(originalLeft) + kHalf2DArrowSize;
        float stepY = (static_cast<float>(bounds.top) + static_cast<float>(bounds.bottom - bounds.top) * kHalf) - centerY;
        float stepX = (static_cast<float>(bounds.right - bounds.left) * kHalf + boundsLeft) - centerX;
        const float distance = std::sqrt(stepX * stepX + stepY * stepY) * kNudgeStepDistanceScale;
        {
            // cS3DVector2::Normalize (0x0062dd74)
            const float scale = kOne / std::sqrt(stepX * stepX + stepY * stepY);
            stepX = scale * stepX;
            stepY = scale * stepY;
        }
        DispatchViewRectInfo hit;
        DispatchViewRectInfo scratch;
        for (float walked = 0.0f; walked < distance; walked += kNudgeStep) {
            rect.left = static_cast<std::int32_t>(centerX - kHalf2DArrowSize);
            rect.top = static_cast<std::int32_t>(centerY - kHalf2DArrowSize);
            rect.right = static_cast<std::int32_t>(kHalf2DArrowSize + centerX);
            rect.bottom = static_cast<std::int32_t>(kHalf2DArrowSize + centerY);
            if (!TestOverlap(rect, placed, bounds, hit)) break;
            if (hit.priority != 0xff) {
                rect.left = hit.left;
                rect.right = hit.right;
                rect.top = static_cast<std::int32_t>(static_cast<float>(hit.top) + kNudgeAround);
                rect.bottom = static_cast<std::int32_t>(static_cast<float>(hit.bottom) + kNudgeAround);
                if (!TestOverlap(rect, placed, bounds, scratch)) break;
                rect.left = hit.left;
                rect.right = hit.right;
                rect.top = static_cast<std::int32_t>(static_cast<float>(hit.top) - kNudgeAround);
                rect.bottom = static_cast<std::int32_t>(static_cast<float>(hit.bottom) - kNudgeAround);
                if (!TestOverlap(rect, placed, bounds, scratch)) break;
                rect.top = hit.top;
                rect.bottom = hit.bottom;
                rect.left = static_cast<std::int32_t>(static_cast<float>(hit.left) + kNudgeAround);
                rect.right = static_cast<std::int32_t>(static_cast<float>(hit.right) + kNudgeAround);
                if (!TestOverlap(rect, placed, bounds, scratch)) break;
                rect.top = hit.top;
                rect.bottom = hit.bottom;
                rect.left = static_cast<std::int32_t>(static_cast<float>(hit.left) - kNudgeAround);
                rect.right = static_cast<std::int32_t>(static_cast<float>(hit.right) - kNudgeAround);
                if (!TestOverlap(rect, placed, bounds, scratch)) break;
            }
            centerX = stepX + stepX + centerX;
            centerY = stepY + stepY + centerY;
        }
        if (TestOverlap(rect, placed, bounds, hit)) {
            rect.left = originalLeft;
            rect.top = originalTop;
            rect.right = originalRight;
            rect.bottom = originalBottom;
        }
        placed.push_back(rect);

        if ((info.flags & 0x1) == 0) host->ProjectPointToViewportCoordinates2D(position);
        cS3DVector3 center;
        if ((info.flags & 0x80) != 0) {
            center = info.lastCenter;
        } else {
            center.x = static_cast<float>(rect.left) + static_cast<float>(rect.right - rect.left) * kHalf;
            center.y = static_cast<float>(rect.top) + static_cast<float>(rect.bottom - rect.top) * kHalf;
            center.z = 0.0f;
        }
        info.lastRect = DispatchViewRect{rect.left, rect.top, rect.right, rect.bottom};
        info.lastCenter = center;

        // the arrow points from the centre at the occupant's viewport point
        float pointX = position[0] - center.x;
        float pointY = position[1] - center.y;
        float pointZ = 0.0f;
        {
            // cS3DVector3::Normalize (0x0062c712)
            const float scale = kOne / std::sqrt(pointX * pointX + pointY * pointY + pointZ * pointZ);
            pointX = scale * pointX;
            pointY = scale * pointY;
            pointZ = scale * pointZ;
        }
        const float angle = std::atan2(-pointX, pointY);
        cS3DTransform arrow;
        FromZ(arrow.rotation, angle);
        arrow.translation[0] += center.x;
        arrow.translation[1] += center.y;
        arrow.translation[2] += center.z;
        arrow.flags |= 5;
        arrow.changeCounter = static_cast<std::uint8_t>(arrow.changeCounter + 2);

        DispatchViewVertex quads[8] = {
            {-32.0f, -32.0f, 0.0f, 0.0f, 0.0f},
            {-32.0f, 32.0f, 0.0f, 0.0f, 1.0f},
            {32.0f, 32.0f, 0.0f, 1.0f, 1.0f},
            {32.0f, -32.0f, 0.0f, 1.0f, 0.0f},
        };
        const float halfWidth = info.width * kHalf;
        const float halfHeight = kHalf * info.height;
        quads[4] = DispatchViewVertex{-halfWidth, -halfHeight, 0.0f, info.numberQuad[0].u, info.numberQuad[0].v};
        quads[5] = DispatchViewVertex{-halfWidth, halfHeight, 0.0f, info.numberQuad[1].u, info.numberQuad[1].v};
        quads[6] = DispatchViewVertex{halfWidth, halfHeight, 0.0f, info.numberQuad[2].u, info.numberQuad[2].v};
        quads[7] = DispatchViewVertex{halfWidth, -halfHeight, 0.0f, info.numberQuad[3].u, info.numberQuad[3].v};
        for (int i = 0; i < 4; ++i) {
            float x = quads[i].x, y = quads[i].y, z = quads[i].z;
            TransformPoint(arrow, x, y, z);
            quads[i].x = x;   // only x / y are written back
            quads[i].y = y;
            quads[4 + i].x = quads[4 + i].x + center.x;
            quads[4 + i].y = quads[4 + i].y + center.y;
        }

        context->ResetModelViewTransform();
        context->ClearTexTransform(0);
        host->Set2DCameraView(*context);
        context->SetTransparency(info.alpha);
        context->SetRenderState(states_[0]);
        if (info.binding == bindings_[2]) {
            // the MySim: the picture under the frame
            if ((info.flags & 0x2) != 0) {
                context->SetTexture(static_cast<int>(info.texture), 0);
                context->DrawPrims(6, 2, 4, &quads[4]);
            }
            context->SetTexture(static_cast<int>(BindingTexture(info.binding)), 0);
            context->DrawPrims(6, 2, 4, &quads[0]);
        } else {
            context->SetTexture(static_cast<int>(BindingTexture(info.binding)), 0);
            context->DrawPrims(6, 2, 4, &quads[0]);
            if ((info.flags & 0x2) != 0) {
                context->SetTexture(static_cast<int>(info.texture), 0);
                context->DrawPrims(6, 2, 4, &quads[4]);
            }
        }
        context->ResetTransparency();
        host->Set3DCameraView(*context);
    }
    host->RestartTickTimer();
    return true;
}

}  // namespace sc4

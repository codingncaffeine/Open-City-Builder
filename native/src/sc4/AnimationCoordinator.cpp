#include "sc4/AnimationCoordinator.h"

#include <cmath>

#include "gz/cIGZMessage2.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kIidMessageTarget2 = 0x452294aa;
constexpr std::uint32_t kMessageCellBlockChanged = 0x8a07ae00;
constexpr std::uint32_t kMessageLocalFundingChanged = 0x6a5fd3fd;
constexpr std::uint32_t kMessagePreferencesChanged = 0x231bbf91;
constexpr std::uint32_t kMessageCameraTargetDone = 0xa6b79612;
constexpr std::uint32_t kMessageZoomRotationDone = 0xa6b79602;
constexpr std::uint32_t kMessages[] = {kMessageCellBlockChanged, kMessageLocalFundingChanged, kMessagePreferencesChanged,
                                       kMessageCameraTargetDone, kMessageZoomRotationDone};
constexpr std::uint32_t kFundedObjectIid = 0x26bb767e;
constexpr std::uint32_t kPropRequiresPower = 0xea5393ed;
constexpr std::uint32_t kPropFundingToAnimate = 0x8a5f88e3;   // {budget purpose, least percent}
constexpr std::uint32_t kPropNotAnimatable = 0x4a70d491;
constexpr std::uint32_t kOccupantFlagAnimating = 0x20;
constexpr std::uint32_t kTickPriority = 0xc0;
constexpr float kWorldToSet = 0.00390625f;   // 1 / 256 (0x00a70924)
constexpr float kCellToSet = 0.0625f;        // 1 / 16 (0x00a70700)
}  // namespace

bool cSC4AnimationCoordinator::IsAnimatableClass(int modelClass) {
    return modelClass == static_cast<int>(0xea123be1u) || modelClass == static_cast<int>(0xaa123bf9u);
}

cSC4AnimationCoordinator::~cSC4AnimationCoordinator() { Shutdown(); }

bool cSC4AnimationCoordinator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kIidMessageTarget2 || iid == 1) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4AnimationCoordinator::Release() {
    const std::uint32_t left = refCount_ > 0 ? --refCount_ : 0;
    if (left == 0) delete this;
    return left;
}

// 0x00285946
bool cSC4AnimationCoordinator::Init() {
    if (initialized_ || !host_) return true;
    if (cIGZMessageServer2* server = host_->Messages())
        for (std::uint32_t type : kMessages) server->AddNotification(this, type);
    zoom_ = host_->ViewZoom();
    const int cells = host_->CityCellsX();
    sets_ = (cells < 0 ? cells + 15 : cells) >> 4;
    grid_.assign(static_cast<std::size_t>(sets_ > 0 ? sets_ : 0), std::vector<OccupantSet>(static_cast<std::size_t>(sets_ > 0 ? sets_ : 0)));
    ReadVisibleRect(visible_);
    EvaluatePreferences();
    initialized_ = true;
    return true;
}

// 0x0028559c
void cSC4AnimationCoordinator::Shutdown() {
    if (!initialized_) return;
    if (host_)
        if (cIGZMessageServer2* server = host_->Messages())
            for (std::uint32_t type : kMessages) server->RemoveNotification(this, type);
    // the sets' references go with the grid (the original clears the vector of hash sets of auto references)
    for (auto& column : grid_)
        for (OccupantSet& set : column) {
            for (cISC4Occupant* occupant : set) occupant->Release();
            set.clear();
        }
    grid_.clear();
    initialized_ = false;
}

// cSC4AnimatableOccupantFilter::IsOccupantIncluded: an animated model class and no 0x4a70d491 on the exemplar.
bool cSC4AnimationCoordinator::IsIncluded(cISC4Occupant* occupant) {
    if (!occupant || !host_ || !IsAnimatableClass(host_->OccupantModelClass(occupant))) return false;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    return !(holder && holder->HasProperty(kPropNotAnimatable));
}

// 0x00284d44 with RequiresFundingToAnimate 0x00284c60 and GetActualFundingForOccupant 0x00284cc4.
bool cSC4AnimationCoordinator::ShouldAnimate(cISC4Occupant* occupant) {
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    if (!holder) return true;
    if (holder->HasProperty(kPropRequiresPower) && !host_->IsOccupantPowered(occupant)) return false;
    std::vector<std::uint32_t> funding;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPropFundingToAnimate, funding) && funding.size() > 1) {
        float actual = 0.0f;   // kSC4PercentageZero when there is no lot / building / item
        if (!host_->OccupantFunding(occupant, funding[0], actual)) actual = 0.0f;
        if (actual * 100.0f < static_cast<float>(funding[1])) return false;
    }
    return true;
}

// 0x00284eb4
void cSC4AnimationCoordinator::AddToTickManager(cISC4Occupant* occupant, bool add) {
    void* raw = nullptr;
    if (!occupant->QueryInterface(kIidMessageTarget2, &raw) || !raw) return;
    auto* target = static_cast<cIGZMessageTarget2*>(raw);
    if (add) {
        host_->AddTickable(target, kTickPriority);
        occupant->SetFlag(kOccupantFlagAnimating);
    } else {
        host_->RemoveTickable(target, kTickPriority);
        occupant->ClearFlag(kOccupantFlagAnimating);
    }
    target->Release();
}

// 0x002851c6: the occupant's position / 256, floored, clamped to the grid.
void cSC4AnimationCoordinator::GetOccupantSetCell(cISC4Occupant* occupant, int& x, int& z) const {
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(position);
    x = static_cast<int>(std::floor(position[0] * kWorldToSet));
    z = static_cast<int>(std::floor(position[2] * kWorldToSet));
    if (x < 0) x = 0; else if (x >= sets_) x = sets_ - 1;
    if (z < 0) z = 0; else if (z >= sets_) z = sets_ - 1;
}

bool cSC4AnimationCoordinator::InVisible(int x, int z) const {
    return detailAllows_ && zoom_ >= leastZoom_ && x >= visible_[0] && z >= visible_[1] && x <= visible_[2] && z <= visible_[3];
}

// 0x00284f76
void cSC4AnimationCoordinator::UpdateSet(OccupantSet& set, int x, int z) {
    for (cISC4Occupant* occupant : set) {
        const bool animate = InVisible(x, z) && ShouldAnimate(occupant);
        if (animate != occupant->IsFlagSet(kOccupantFlagAnimating)) AddToTickManager(occupant, animate);
    }
}

// 0x00285034
void cSC4AnimationCoordinator::UpdateRect(const int rect[4]) {
    if (held_ == 0) return;
    for (int x = rect[0]; x <= rect[2]; ++x)
        for (int z = rect[1]; z <= rect[3]; ++z) {
            OccupantSet& set = grid_[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
            if (!set.empty()) UpdateSet(set, x, z);
        }
}

// 0x002850b8 with CityCellRectToOccupantSetRect 0x00284bd4 (truncating; only the low ends clamp to 0, the high ends to n - 1).
void cSC4AnimationCoordinator::UpdateCellRect(const int cells[4]) {
    if (held_ == 0) return;
    int rect[4];
    for (int i = 0; i < 4; ++i) rect[i] = static_cast<int>(static_cast<float>(cells[i]) * kCellToSet);
    if (rect[0] < 0) rect[0] = 0;
    if (rect[1] < 0) rect[1] = 0;
    if (rect[2] >= sets_) rect[2] = sets_ - 1;
    if (rect[3] >= sets_) rect[3] = sets_ - 1;
    UpdateRect(rect);
}

// 0x0028513a
void cSC4AnimationCoordinator::UpdateAll() {
    if (held_ == 0) return;
    const int rect[4] = {0, 0, sets_ - 1, sets_ - 1};
    UpdateRect(rect);
}

void cSC4AnimationCoordinator::ReadVisibleRect(int out[4]) const {
    float area[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (host_) host_->VisibleTerrainRect(area);
    for (int i = 0; i < 4; ++i) out[i] = static_cast<int>(area[i] * kWorldToSet);
    if (out[0] < 0) out[0] = 0;
    if (out[1] < 0) out[1] = 0;
    if (out[2] >= sets_) out[2] = sets_ - 1;
    if (out[3] >= sets_) out[3] = sets_ - 1;
}

// 0x00285176
void cSC4AnimationCoordinator::EvaluatePreferences() {
    if (held_ == 0) {
        // Quirk kept: with nothing held the original returns before reading the preference (the ctor's 1 stays until
        // the first occupant arrives and a later 0x231bbf91 re-reads it).
        return;
    }
    const bool allows = host_->CityDetail() >= 2;
    if (allows != detailAllows_) {
        detailAllows_ = allows;
        if (initialized_) UpdateAll();
    }
}

// 0x00285bf6 with GetCellRectDifferences 0x002852ac.
void cSC4AnimationCoordinator::UpdateVisibleSetRect() {
    int next[4];
    ReadVisibleRect(next);
    if (next[0] == visible_[0] && next[1] == visible_[1] && next[2] == visible_[2] && next[3] == visible_[3]) return;
    if (held_ == 0 || zoom_ < leastZoom_) {
        for (int i = 0; i < 4; ++i) visible_[i] = next[i];
        return;
    }
    std::vector<std::pair<int, int>> changed;
    const auto collect = [&changed](const int* from, const int* other) {
        for (int x = from[0]; x <= from[2]; ++x)
            for (int z = from[1]; z <= from[3]; ++z)
                if (x < other[0] || z < other[1] || other[2] < x || other[3] < z) changed.emplace_back(x, z);
    };
    collect(next, visible_);
    collect(visible_, next);
    for (int i = 0; i < 4; ++i) visible_[i] = next[i];
    for (const auto& [x, z] : changed) {
        OccupantSet& set = grid_[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
        if (!set.empty()) UpdateSet(set, x, z);
    }
}

// 0x0028535e
void cSC4AnimationCoordinator::SetOccupantAnimation(cISC4Occupant* occupant, bool on) {
    if (!initialized_ || sets_ <= 0 || !IsIncluded(occupant)) return;
    int x = 0, z = 0;
    GetOccupantSetCell(occupant, x, z);
    OccupantSet& set = grid_[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
    const int before = static_cast<int>(set.size());
    if (on) {
        if (set.insert(occupant).second) occupant->AddRef();
    } else {
        if (set.erase(occupant) != 0) occupant->Release();
    }
    held_ += static_cast<int>(set.size()) - before;
    const bool animate = on && InVisible(x, z) && ShouldAnimate(occupant);
    if (animate != occupant->IsFlagSet(kOccupantFlagAnimating)) AddToTickManager(occupant, animate);
}

// 0x00285dda
bool cSC4AnimationCoordinator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    switch (message->GetType()) {
    case kMessageCellBlockChanged: {
        const int cells[4] = {static_cast<int>(standard->GetData1()), static_cast<int>(standard->GetData2()),
                              static_cast<int>(standard->GetData3()), static_cast<int>(standard->GetData4())};
        UpdateCellRect(cells);
        break;
    }
    case kMessagePreferencesChanged:
        EvaluatePreferences();
        break;
    case kMessageLocalFundingChanged: {
        cIGZUnknown* payload = standard->GetIGZUnknown();
        void* raw = nullptr;
        if (payload && payload->QueryInterface(kFundedObjectIid, &raw) && raw) {
            auto* occupant = static_cast<cISC4Occupant*>(static_cast<cIGZUnknown*>(raw));
            // DoOccupantBudgetChanged 0x002850f4: the occupant's lot's cells
            int cells[4];
            if (host_->OccupantLotCells(occupant, cells)) UpdateCellRect(cells);
            occupant->Release();
        }
        break;
    }
    case kMessageZoomRotationDone: {
        const auto* before = static_cast<const int*>(standard->GetVoid1());
        const auto* after = static_cast<const int*>(standard->GetVoid2());
        if (!before || !after) break;   // guard: the original reads both without a check
        zoom_ = after[0];
        if (held_ != 0) {
            const bool wasAbove = leastZoom_ <= before[0];
            if ((before[1] != after[1] && wasAbove) || wasAbove != (leastZoom_ <= after[0])) {
                ReadVisibleRect(visible_);
                UpdateAll();
            }
        }
        break;
    }
    case kMessageCameraTargetDone:
        UpdateVisibleSetRect();
        break;
    default:
        break;
    }
    return true;
}

}  // namespace sc4

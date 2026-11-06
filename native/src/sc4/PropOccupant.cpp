#include "sc4/PropOccupant.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include <bit>

#include "gz/StreamOps.h"
#include "rz/cRZMessage2Standard.h"
#include "s3d/ModelInstanceAnimatedSprite.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kGZIID_cIGZMessageTarget2Prop = 0x452294aa;
constexpr std::uint32_t kMessagePropAnimationTick = 0x86ad10ee;   // the animation tick manager's message
std::vector<std::uint32_t>& TypeList() {
    static std::vector<std::uint32_t> list;
    return list;
}
IPropHost* g_host = nullptr;
}  // namespace

IPropHost* PropHost() { return g_host; }
void SetPropHost(IPropHost* host) { g_host = host; }

std::uint16_t GetPropTypeIndex(std::uint32_t propType) {
    auto& list = TypeList();
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i] == propType) return static_cast<std::uint16_t>(i);
    list.push_back(propType);
    return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint32_t GetPropTypeFromIndex(std::uint16_t index) {
    const auto& list = TypeList();
    return index < list.size() ? list[index] : 0;
}

bool cSC4PropOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZMessageTarget2Prop) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return cSC4Occupant::QueryInterface(iid, out);   // (the original also answers 0x2977aa47 / 0xe9793a65)
}

bool cSC4PropOccupant::DoMessage(cIGZMessage2* message) {
    if (!message || message->GetType() != kMessagePropAnimationTick) return true;
    cISC43DPlaceableObject* placeable = GetPlaceableObject();
    if (!placeable) return true;
    placeable->AddRef();
    std::uint32_t count = static_cast<std::uint32_t>(placeable->GetModelCount());
    if (count != 0) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        const float milliseconds = std::bit_cast<float>(static_cast<std::uint32_t>(standard->GetData3()));
        const float dilation = g_host ? g_host->AnimationTimeDilation() : 1.0f;
        std::vector<cIS3DModelInstance*> models(count, nullptr);
        placeable->GetModels(models.data(), count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!models[i]) continue;
            void* raw = nullptr;
            if (models[i]->QueryInterface(s3d::kGZIID_cIS3DModelInstanceAnimated, &raw) && raw) {
                auto* animated = static_cast<s3d::cIS3DModelInstanceAnimated*>(raw);
                animated->DoAutoFrameUpdate(milliseconds * dilation);
                animated->Release();
            }
        }
    }
    placeable->Release();
    return true;
}

bool cSC4PropOccupant::Init() {
    if (!IsInitialized()) cSC4Occupant::Init();
    return true;
}

bool cSC4PropOccupant::Shutdown() {
    dateRange_.reset();
    return cSC4Occupant::Shutdown();
}

bool cSC4PropOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(6);
    if (cSC4Occupant::Write(stream)) {
        stream.SetUint32(GetPropTypeFromIndex(propTypeIndex_));
        for (float v : min_) stream.SetFloat32(v);
        for (float v : max_) stream.SetFloat32(v);
        stream.SetSint8(orientation_);
        stream.SetUint8(state_);
        stream.SetUint8(timeStart_);
        stream.SetUint8(timeEnd_);
        stream << (dateRange_ != nullptr);
        if (dateRange_) {
            stream.SetUint32(dateRange_->period);
            stream.SetUint32(dateRange_->span);
            stream << cRZDate::FromJulianDay(dateRange_->showDate.DayNumber());
            stream << cRZDate::FromJulianDay(dateRange_->hideDate.DayNumber());
        }
        stream.SetUint8(randomChance_);
        stream.SetUint8(usage_);
        stream.SetUint32(lotTag_);
        stream.SetUint8(timerBits_);
    }
    return !stream.GetError();
}

bool cSC4PropOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (cSC4Occupant::Read(stream)) {
        std::uint32_t type = 0;
        stream.GetUint32(type);
        propTypeIndex_ = GetPropTypeIndex(type);
        for (float& v : min_) stream.GetFloat32(v);
        for (float& v : max_) stream.GetFloat32(v);
        std::int8_t orientation = 0;
        stream.GetSint8(orientation);
        orientation_ = orientation;
        stream.GetUint8(state_);
        stream.GetUint8(timeStart_);
        stream.GetUint8(timeEnd_);
        bool hasRange = false;
        stream >> hasRange;
        if (hasRange) {
            dateRange_ = std::make_unique<DateRange>();
            stream.GetUint32(dateRange_->period);
            stream.GetUint32(dateRange_->span);
            cRZDate a, b;
            stream >> a >> b;
            dateRange_->showDate.Set(a.JulianDay());
            dateRange_->hideDate.Set(b.JulianDay());
        }
        stream.GetUint8(randomChance_);
        stream.GetUint8(usage_);
        stream.GetUint32(lotTag_);
        if (version < 6) timerBits_ = 0xf;
        else stream.GetUint8(timerBits_);
    }
    return !stream.GetError();
}

void cSC4PropOccupant::GetPosition(float* xyz) {
    xyz[0] = (min_[0] + max_[0]) * 0.5f;
    xyz[1] = min_[1];
    xyz[2] = (min_[2] + max_[2]) * 0.5f;
}

void cSC4PropOccupant::SetPosition(const float* xyz) {
    const float halfX = (max_[0] - min_[0]) * 0.5f, oldY = min_[1], halfZ = (max_[2] - min_[2]) * 0.5f;
    min_[0] = xyz[0] - halfX;
    max_[0] = halfX + xyz[0];
    min_[1] = xyz[1];
    max_[1] = (max_[1] - oldY) + xyz[1];
    min_[2] = xyz[2] - halfZ;
    max_[2] = halfZ + xyz[2];
}

void cSC4PropOccupant::GetBoundingBox(float* minXyz, float* maxXyz) {
    std::copy(min_, min_ + 3, minXyz);
    std::copy(max_, max_ + 3, maxXyz);
}

void cSC4PropOccupant::SetBoundingBox(const float* min, const float* max) {
    std::copy(min, min + 3, min_);
    std::copy(max, max + 3, max_);
}

bool cSC4PropOccupant::GetLotTag(std::uint32_t& tag) const {
    if (lotTag_ == 0xffffffffu) return false;
    tag = lotTag_;
    return true;
}

bool cSC4PropOccupant::SetOrientation(std::int32_t orientation) {
    orientation_ = static_cast<std::int8_t>(std::clamp(orientation, 0, 3));
    return true;
}

void cSC4PropOccupant::SetState(std::uint32_t state) {
    if (state_ == static_cast<std::uint8_t>(state)) return;
    state_ = static_cast<std::uint8_t>(state);
    if (IsInitialized()) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMessagePropStateChanged);
        message->SetVoid1(static_cast<cISC4Occupant*>(this));
        message->SetIGZUnknown(static_cast<cISC4Occupant*>(this));
        message->SetData2(state);
        if (cIGZMessageServer2* server = DBSegmentMessageServer()) server->MessagePost(message, false);
        message->Release();
    }
    // (the original also forwards the state to the placeable object's effect children, iid 0xa818edd8)
}

void cSC4PropOccupant::SetTimeOfDayRange(float start, float end) {
    timeStart_ = static_cast<std::uint8_t>(static_cast<int>(start * 10.0f));
    timeEnd_ = static_cast<std::uint8_t>(static_cast<int>(end * 10.0f));
    IPropHost* host = g_host;
    if (host && !(end < 0.0f) && !(start < 0.0f) && start != end) {
        const float now = static_cast<float>(host->ClockHour()) + static_cast<float>(host->ClockMinute()) / 60.0f;
        SetPropTimerState(kPropTimerTimeOfDay, now <= end && start <= now);
    }
}

void cSC4PropOccupant::GetTimeOfDayRange(float& start, float& end) const {
    start = static_cast<float>(timeStart_) * 0.1f;
    end = static_cast<float>(timeEnd_) * 0.1f;
}

void cSC4PropOccupant::SetSimulatorDateRange(std::uint32_t period, std::uint32_t span, std::uint8_t month, std::uint8_t day) {
    if (period == 0) {
        dateRange_.reset();
        return;
    }
    if (!dateRange_) dateRange_ = std::make_unique<DateRange>();
    dateRange_->period = period;
    dateRange_->span = span != 0 ? span : 1;
    cGZDate today;
    if (g_host) g_host->SimulatorDate(today);
    dateRange_->showDate.Set(month, day, today.Year());
    if (dateRange_->showDate < today) dateRange_->showDate.Set(month, day, today.Year() + 1);
    dateRange_->hideDate.Set(static_cast<std::uint32_t>(dateRange_->showDate + static_cast<std::int32_t>(dateRange_->span)));
    SetPropTimerState(kPropTimerDate, today >= dateRange_->showDate && today <= dateRange_->hideDate);
}

void cSC4PropOccupant::TickSimulatorDate(const cIGZDate& today) {
    if (!dateRange_ || dateRange_->period == 0) return;
    if (!GetPropTimerState(kPropTimerDate) && today >= dateRange_->showDate) {
        SetPropTimerState(kPropTimerDate, true);
        if (dateRange_->period != 365) {
            dateRange_->showDate += static_cast<std::int32_t>(dateRange_->period);
        } else {
            dateRange_->showDate.Set(dateRange_->showDate.Month(), dateRange_->showDate.DayOfMonth(), dateRange_->showDate.Year() + 1);
        }
        return;
    }
    if (GetPropTimerState(kPropTimerDate) && today >= dateRange_->hideDate) {
        SetPropTimerState(kPropTimerDate, false);
        dateRange_->hideDate.Set(static_cast<std::uint32_t>(dateRange_->showDate + static_cast<std::int32_t>(dateRange_->span)));
    }
}

void cSC4PropOccupant::SetPropTimerState(std::uint8_t mask, bool on) {
    const std::uint8_t before = timerBits_;
    timerBits_ = on ? (before | mask) : (before & ~mask);
    const bool wasAll = (before & 0xf) == 0xf, isAll = (timerBits_ & 0xf) == 0xf;
    if (wasAll == isAll) return;
    if (isAll) {
        // all timers allow it: show the prop, subject to its random chance
        if (g_host && g_host->RandomPercent() < randomChance_) PlaceInRenderer(true);
    } else {
        PlaceInRenderer(false);
    }
}

bool cSC4PropOccupant::PlaceInOccupantManager(bool place) {
    const bool placed = (flags_ & 1) != 0;
    if (placed == place) return true;
    cISC4OccupantManager* manager = g_host ? g_host->OccupantManager() : nullptr;
    if (!manager) return false;
    if (!placed) {
        if (!manager->InsertOccupant(static_cast<cISC4Occupant*>(this), 0)) return false;
        flags_ |= 1;
    } else {
        if (flags_ & 2) PlaceInRenderer(false);
        manager->RemoveOccupant(static_cast<cISC4Occupant*>(this), false, 0);
        flags_ &= ~1;
    }
    return true;
}

bool cSC4PropOccupant::PlaceInRenderer(bool place) {
    if ((((flags_ & 2) != 0) == place)) return true;
    cISC43DPlaceableObject* object = GetPlaceableObject();
    if (!object) return false;
    object->AddRef();
    if (place) {
        if (!(flags_ & 1)) PlaceInOccupantManager(true);
        flags_ |= 2;
    } else {
        flags_ &= ~2;
    }
    object->SetVisibility(place);
    object->Release();
    return true;
}

}  // namespace sc4

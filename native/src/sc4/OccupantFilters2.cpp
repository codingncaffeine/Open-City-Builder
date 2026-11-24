#include "sc4/OccupantFilters2.h"
#include "gz/StreamOps.h"

#include <algorithm>

namespace sc4 {

static IOccupantFilterWorld* g_world = nullptr;
IOccupantFilterWorld* OccupantFilterWorld() { return g_world; }
void SetOccupantFilterWorld(IOccupantFilterWorld* world) { g_world = world; }

bool cSC4OccupantTypesFilter::IsOccupantTypeIncluded(std::uint32_t type) {
    return std::find(types_.begin(), types_.end(), type) != types_.end();
}

// Version 3, base version 3, then (unless the stream has already failed) the count and the types.
bool cSC4OccupantTypesFilter::Write(cIGZOStream& s) const {
    s.SetUint16(3);
    s.SetUint16(3);
    if (!s.GetError()) {
        s.SetUint32(static_cast<std::uint32_t>(types_.size()));
        for (const std::uint32_t type : types_) s.SetUint32(type);
    }
    return !s.GetError();
}

bool cSC4OccupantTypesFilter::Read(cIGZIStream& s) {
    std::uint16_t version = 0;
    s.GetUint16(version);
    if (version != 3) return false;
    s.GetUint16(version);
    if (version == 3 && !s.GetError()) {
        std::uint32_t count = 0;
        s.GetUint32(count);
        types_.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) s.GetUint32(types_[i]);
    }
    return !s.GetError();
}

bool cSC4OccupantTypesFilterHashed::Write(cIGZOStream& s) const {
    s.SetUint16(3);
    s.SetUint16(3);
    if (!s.GetError()) {
        s.SetUint32(static_cast<std::uint32_t>(types_.size()));
        for (const std::uint32_t type : types_) s.SetUint32(type);
    }
    return !s.GetError();
}

bool cSC4OccupantTypesFilterHashed::Read(cIGZIStream& s) {
    std::uint16_t version = 0;
    s.GetUint16(version);
    if (version != 3) return false;
    s.GetUint16(version);
    if (version == 3 && !s.GetError()) {
        std::uint32_t count = 0;
        s.GetUint32(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t type = 0;
            s.GetUint32(type);
            types_.insert(type);
        }
    }
    return !s.GetError();
}

bool cSC4ViewLevelOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    return static_cast<std::uint32_t>(g_world->OccupantViewLevel(occupant)) == level_;
}

bool cSC4ViewLevelOccupantFilter::Write(cIGZOStream& s) const {
    s.SetUint16(1);
    s.SetUint8(static_cast<std::uint8_t>(level_));
    return !s.GetError();
}

bool cSC4ViewLevelOccupantFilter::Read(cIGZIStream& s) {
    std::uint16_t version = 0;
    s.GetUint16(version);
    if (version != 1) return false;
    std::uint8_t level = 0;
    s.GetUint8(level);
    level_ = level;
    return !s.GetError();
}

bool cSC4NetworkOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    std::uint32_t flags = 0;
    return g_world->NetworkFlags(occupant, flags) && (mask_ & flags) != 0;
}

bool cSC4NetworkOccupantFilter::Write(cIGZOStream& s) const {
    s.SetUint16(1);
    s.SetUint32(mask_);
    return !s.GetError();
}

bool cSC4NetworkOccupantFilter::Read(cIGZIStream& s) {
    std::uint16_t version = 0;
    s.GetUint16(version);
    if (version != 1) return false;
    s.GetUint32(mask_);
    return !s.GetError();
}

bool cSC4SurfaceNetworkOccupantFilter::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZIID_cISC4OccupantFilter && iid != 1) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4SurfaceNetworkOccupantFilter::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4SurfaceNetworkOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    std::uint32_t flags = 0;
    return g_world->NetworkFlags(occupant, flags) && (flags & 0x1f4f) != 0;
}

cSC4PlacedNetworkOccupantFilter::cSC4PlacedNetworkOccupantFilter(cISC4OccupantFilter* inner) : inner_(inner) {
    if (inner_) inner_->AddRef();
}

cSC4PlacedNetworkOccupantFilter::~cSC4PlacedNetworkOccupantFilter() {
    if (inner_) inner_->Release();
}

bool cSC4PlacedNetworkOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    return inner_->IsOccupantIncluded(occupant) && IsOccPlaced(occupant);
}

bool cSC4PlacedNetworkOccupantFilter::IsOccPlaced(cISC4Occupant* occupant) {
    return occupant != nullptr && g_world->NetworkIsPlaced(occupant);
}

// ---- cSC4RealisticOccupantFilter ---------------------------------------------------------------------------------------

bool cSC4RealisticOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {   // i386 0x000795c8
    if (!cSC4DefaultOccupantFilter::IsOccupantIncluded(occupant)) return false;
    if (!showZots && occupant->IsOccupantGroup(0x5001)) return false;
    auto answers = [occupant](std::uint32_t iid) {
        void* raw = nullptr;
        if (!occupant->QueryInterface(iid, &raw) || !raw) return false;
        static_cast<cIGZUnknown*>(raw)->Release();
        return true;
    };
    const std::uint32_t type = occupant->GetType();
    if (type == 0x497f6d9d) {   // a lot base texture: hidden on an empty lot
        if (showEmptyLotTextures) return true;
        if (answers(0x097f6d98)) {
            float position[3] = {0.0f, 0.0f, 0.0f};
            occupant->GetPosition(position);
            IOccupantFilterWorld* world = OccupantFilterWorld();
            if (world && world->LotStateAtPosition(position[0], position[2]) == 1) return false;
        }
        return true;
    }
    if (type != 0xa823821e || showSigns) return true;   // a prop: signposts and text decals are hidden
    return !answers(0x4b44fbe2) && !answers(0x2b3b7d86);
}

bool cSC4RealisticOccupantFilter::Write(cIGZOStream& s) const {   // i386 0x0007a192: version 4, the three flags, the default filter
    s.SetUint16(4);
    s << showEmptyLotTextures << showZots << showSigns;
    return cSC4DefaultOccupantFilter::Write(s);
}

bool cSC4RealisticOccupantFilter::Read(cIGZIStream& s) {   // i386 0x00079eaa: version 3 has no sign flag (it follows the zots)
    std::uint16_t version = 0;
    s.GetUint16(version);
    if (version < 3) return false;
    s >> showEmptyLotTextures >> showZots;
    if (version < 4) showSigns = showZots;
    else s >> showSigns;
    return cSC4DefaultOccupantFilter::Read(s);
}

}  // namespace sc4

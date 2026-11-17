#include "sc4/CommercialSimulator.h"

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/DBSegment.h"

#include <cstdio>
#include <cstdlib>

namespace sc4 {

bool cSC4CommercialSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4CommercialSimulator) *out = static_cast<cISC4CommercialSimulator*>(this);
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4CommercialSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

cSC4SimGrid<std::uint8_t>* cSC4CommercialSimulator::MakeGrid(std::uint32_t instanceId) {
    auto* grid = new cSC4SimGrid<std::uint8_t>();
    grid->AddRef();
    grid->Init();
    grid->SetTractSize(tractSize_);
    grid->SetInstanceID(instanceId);
    return grid;
}

bool cSC4CommercialSimulator::Init() {
    if (initialised_) return true;
    initialised_ = true;
    static const char* const kName = "cSC4CommercialSimulator";
    for (int kind : {0, 10, 4, 6}) host_->AddIdleClient(static_cast<cIGZMessageTarget2*>(this), kind, kName, 2);
    host_->AddNotification(this, kMessageCommercialChangeA);
    host_->AddNotification(this, kMessageCommercialChangeB);
    if (!host_->AcquireServices()) return false;
    if (!highGrid_) highGrid_ = MakeGrid(kCommercialProximityGridHigh);
    if (!lowGrid_) lowGrid_ = MakeGrid(kCommercialProximityGridLow);
    return true;
}

bool cSC4CommercialSimulator::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    for (auto** grid : {&highGrid_, &lowGrid_}) {
        if (*grid) {
            (*grid)->Shutdown();
            (*grid)->Release();
            *grid = nullptr;
        }
    }
    host_->RemoveNotification(this, kMessageCommercialChangeA);
    host_->RemoveNotification(this, kMessageCommercialChangeB);
    host_->ReleaseServices();
    host_->RemoveIdleClient(static_cast<cIGZMessageTarget2*>(this));
    return true;
}

bool cSC4CommercialSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    bool flag = true;
    if (!dirty_) flag = state_ != 3;
    stream.SetUint8(flag);
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(lowGrid_));
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(highGrid_));
    return !stream.GetError();
}

bool cSC4CommercialSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    std::uint8_t flag = 0;
    stream.GetUint8(flag);
    dirty_ = flag != 0;
    SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, reinterpret_cast<void**>(&lowGrid_), false);
    SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, reinterpret_cast<void**>(&highGrid_), false);
    if (std::getenv("SC4_COMMERCIALDEBUG")) {   // what the saved proximity maps hold: zeros, the far value, column 0
        for (const auto* grid : {lowGrid_, highGrid_}) {
            if (!grid) continue;
            const auto& data = grid->GetGridData();
            int zeros = 0, far = 0, column0 = 0, cells = 0, maxValue = 0;
            for (const auto& column : data)
                for (std::size_t z = 0; z < column.size(); ++z) {
                    ++cells;
                    if (column[z] == 0) { ++zeros; if (z == 0) ++column0; }
                    if (column[z] == 0xff) ++far;
                    if (column[z] > maxValue) maxValue = column[z];
                }
            std::fprintf(stderr, "commercial: read grid %08x tract %d %dx%d: %d cells, %d zero (%d in row 0), %d at 255, max %d\n",
                         grid->GetInstanceID(), grid->GetTractSize(), grid->GetTractCountX(), grid->GetTractCountZ(), cells, zeros,
                         column0, far, maxValue);
            if (std::getenv("SC4_COMMERCIALDEBUG")[0] == '2') {
                for (std::size_t x = 0; x < data.size(); x += 4) {
                    std::fprintf(stderr, "commercial:  x %3zu:", x);
                    for (std::size_t z = 0; z < data[x].size(); z += 4) std::fprintf(stderr, " %3d", data[x][z]);
                    std::fprintf(stderr, "\n");
                }
            }
        }
        std::fprintf(stderr, "commercial: read dirty %d\n", dirty_ ? 1 : 0);
    }
    return !stream.GetError();
}

bool cSC4CommercialSimulator::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    auto* payload = static_cast<cIGZMessage2Standard*>(message);
    if (type - kMessageCommercialChangeA < 2) {
        if (payload->GetData3() == kCommercialNetworkChangeTag) {
            auto* object = static_cast<cIGZUnknown*>(payload->GetVoid1());
            void* raw = nullptr;
            if (object->QueryInterface(kIIDNetworkChange, &raw)) {
                auto* change = static_cast<ICommercialChange*>(raw);
                if (*change->GetKind() == 3) dirty_ = true;
                change->Release();
            }
        }
    } else if (type == kMessageIdleTick || type == kMessageIdleTickWithData) {
        if (type == kMessageIdleTickWithData) (void)payload->GetData3();
        DoIdle();
    }
    return true;
}

namespace {
void TraceGrid(const char* what, const cSC4SimGrid<std::uint8_t>* grid) {
    if (!grid || !std::getenv("SC4_COMMERCIALDEBUG")) return;
    int zeros = 0, ones = 0, other = 0;
    for (const auto& column : grid->GetGridData())
        for (std::uint8_t v : column) (v == 0 ? zeros : v == 1 ? ones : other)++;
    std::fprintf(stderr, "commercial: %s %08x: %d zero, %d one, %d other\n", what, grid->GetInstanceID(), zeros, ones, other);
}
}  // namespace

void cSC4CommercialSimulator::DoIdle() {
    const int before = state_;
    switch (state_) {
    case 0:
        state_ = 1;
        [[fallthrough]];
    case 1:
        host_->FloodFillDistance(*lowGrid_, 2);
        state_ = 2;
        break;
    case 2:
        host_->FloodFillDistance(*highGrid_, 3);
        state_ = 3;
        break;
    case 3:
        if (dirty_) {
            dirty_ = false;
            state_ = 0;
        }
        break;
    }
    if (before != state_ && state_ == 2) TraceGrid("flooded", lowGrid_);
    if (before != state_ && state_ == 3) TraceGrid("flooded", highGrid_);
}

cSC4SimGrid<std::uint8_t>* cSC4CommercialSimulator::GetProximityMap(std::uint32_t developerType) const {
    if (developerType == kDeveloperTypeCommercialHigh) return highGrid_;
    if (developerType == kDeveloperTypeCommercialLow) return lowGrid_;
    return nullptr;
}

}  // namespace sc4

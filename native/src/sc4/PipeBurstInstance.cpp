#include "sc4/PipeBurstInstance.h"

#include "rz/cRZMessage2Standard.h"
#include "sc4/Occupant.h"

namespace sc4 {

namespace {
IPipeBurstHost* g_host = nullptr;
cRZRandom& Random() {
    static cRZRandom random(0xffffffff);
    return random;
}
constexpr int kOffsetX[4] = {-1, 0, 1, 0};
constexpr int kOffsetZ[4] = {0, -1, 0, 1};

void ReleaseEffect(cISC4VisualEffect*& effect) {
    if (!effect) return;
    effect->Stop(1);
    effect->Release();
    effect = nullptr;
}
}  // namespace

IPipeBurstHost* PipeBurstHost() { return g_host; }
void SetPipeBurstHost(IPipeBurstHost* host) { g_host = host; }

cSC4DisasterInstancePipeBurst::~cSC4DisasterInstancePipeBurst() {
    if (initialised_) Shutdown();
}

bool cSC4DisasterInstancePipeBurst::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DisasterInstance) {
        *out = static_cast<cISC4DisasterInstance*>(this);
        AddRef();
        return true;
    }
    if (iid == 1) {
        *out = this;
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4DisasterInstancePipeBurst::Release() {
    if (refCount_ > 1) return --refCount_;
    refCount_ = 0;
    delete this;
    return 0;
}

bool cSC4DisasterInstancePipeBurst::StaticInit() {
    if (!sbInited) {
        sbInited = true;
        if (g_host) g_host->RegisterShutdownCallback(&StaticShutdown);
        ReadTuningParameters();
    }
    return true;
}

bool cSC4DisasterInstancePipeBurst::StaticShutdown(void*) {
    sbInited = false;
    return true;
}

void cSC4DisasterInstancePipeBurst::SeedRandom(std::uint32_t seed) { Random().Seed(seed); }

void cSC4DisasterInstancePipeBurst::ReadTuningParameters() {
    float minSeconds = 0, maxSeconds = 0;
    int zoom = kGoToZoomLevel;
    if (g_host && g_host->ReadTuning(minSeconds, maxSeconds, zoom)) {
        kMinLeakTimeBeforeBurst = static_cast<int>(static_cast<long>(minSeconds * 1e6f));
        kMaxLeakTimeBeforeBurst = static_cast<int>(static_cast<long>(maxSeconds * 1e6f));
        kGoToZoomLevel = zoom;
    }
}

bool cSC4DisasterInstancePipeBurst::Init(cIGZUnknown* pipeOccupant) {
    if (!sbInited) StaticInit();
    initialised_ = true;
    IDisasterHost* host = DisasterHost();
    cellsX_ = host->CityCellCountX();
    cellsZ_ = host->CityCellCountZ();

    filter_ = new cSC4WateredPipeFilter(nullptr);
    filter_->AddRef();
    if (IDisasterInstanceFactory* factory = DisasterInstanceFactory()) {
        IPlumbingGrid* grid = factory->PlumbingGrid();     // a counted reference, kept by the filter
        filter_->SetGrid(grid);
        if (grid) grid->Release();
    }

    cISC4DisasterLayer* layer = host->DisasterLayer();
    manager_ = layer->GetDisasterManager(kDisasterManagerIdPipeBurst);
    if (manager_) manager_->AddRef();
    manager_->AddInstanceToList(this);

    if (cIGZMessageServer2* server = host->MessageServer()) {
        server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kPipeBurstMessageViewLevelChanged);
        server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageOccupantInserted);
        server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessagePipeOccupantRemoved);
    }
    if (cIGZMessageServer2* notifications = host->NotificationServer())
        notifications->AddNotification(static_cast<cIGZMessageTarget2*>(this), kNotificationCityTick);
    viewLevel_ = g_host ? g_host->ViewLevel() : 1;

    auto* pipe = static_cast<cISC4PipeOccupantView*>(pipeOccupant);
    if (!StartLeakAtPipe(pipe)) {
        Shutdown();
        return false;
    }
    layer->RegisterDisasterLocation(this, 0, 0, pipe ? pipe->AsOccupant() : nullptr, kGoToZoomLevel, true);
    layer->DisasterHasBegun(this);
    return true;
}

bool cSC4DisasterInstancePipeBurst::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    for (tLeakyPipe& leak : leaks_) ReleaseEffect(leak.effect);
    leaks_.clear();
    IDisasterHost* host = DisasterHost();
    if (host) {
        if (cIGZMessageServer2* notifications = host->NotificationServer())
            notifications->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kNotificationCityTick);
        if (cIGZMessageServer2* server = host->MessageServer()) {
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kPipeBurstMessageViewLevelChanged);
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageOccupantInserted);
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessagePipeOccupantRemoved);
        }
    }
    if (filter_) {
        filter_->SetGrid(nullptr);
        filter_->Release();
        filter_ = nullptr;
    }
    if (manager_) {
        manager_->RemoveInstanceFromList(this);
        manager_->Release();
        manager_ = nullptr;
    }
    return true;
}

bool cSC4DisasterInstancePipeBurst::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    switch (message->GetType()) {
        case kPipeBurstMessageAnimationTick:
            DoAnimationTick(static_cast<std::uint32_t>(standard->GetData2()));
            break;
        case kMessageOccupantInserted: {
            auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
            cISCPropertyHolder* holder = occupant->AsPropertyHolder();
            if (!holder->HasProperty(kPropertyLeaveAloneOccupant)) DoOccupantInserted(occupant);
            break;
        }
        case kMessagePipeOccupantRemoved:
            DoOccupantRemoved(static_cast<cISC4Occupant*>(standard->GetVoid1()));
            break;
        case kPipeBurstMessageViewLevelChanged:
            DoViewLevelChange(*static_cast<int*>(standard->GetVoid2()));
            break;
        default: break;
    }
    return true;
}

void cSC4DisasterInstancePipeBurst::DoViewLevelChange(int level) {
    if (level == viewLevel_) return;
    viewLevel_ = level;
    for (tLeakyPipe& leak : leaks_) CreateLeakEffectForPipe(leak);
}

void cSC4DisasterInstancePipeBurst::DoOccupantInserted(cISC4Occupant* occupant) {
    if (occupant->GetType() != kOccupantTypeNetworkOccupant) return;
    cISC4PipeOccupantView* pipe = nullptr;
    if (!occupant->QueryInterface(kGZIID_PortPipeOccupantView, reinterpret_cast<void**>(&pipe))) return;
    std::uint32_t x = 0, z = 0;
    pipe->GetCell(x, z);
    StopLeakAtCell(static_cast<int>(x), static_cast<int>(z));
    pipe->Release();
}

void cSC4DisasterInstancePipeBurst::DoOccupantRemoved(cISC4Occupant* occupant) { DoOccupantInserted(occupant); }

void cSC4DisasterInstancePipeBurst::StopLeakAtCell(int x, int z) {
    for (auto it = leaks_.begin(); it != leaks_.end(); ++it) {
        if (it->cellX == x && it->cellZ == z) {
            ReleaseEffect(it->effect);
            leaks_.erase(it);
            return;
        }
    }
}

void cSC4DisasterInstancePipeBurst::DoAnimationTick(std::uint32_t elapsedMs) {
    TickLeaksInWait();
    for (auto it = leaks_.begin(); it != leaks_.end();) {
        it->elapsed += elapsedMs;
        if (it->effect && it->elapsed >= it->burstTime) {
            BurstPipe(it->cellX, it->cellZ);
            it->effect->Stop(0);               // (not released: the original leaves the effect to finish)
            it = leaks_.erase(it);
        } else {
            ++it;
        }
    }
    if (leaks_.empty() && waiting_.empty()) {
        if (cISC4DisasterLayer* layer = DisasterHost()->DisasterLayer()) {
            layer->UnregisterDisasterLocations(this);
            layer->DisasterHasEnded();
        }
        Shutdown();
    }
}

void cSC4DisasterInstancePipeBurst::TickLeaksInWait() {
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        const int before = it->delay++;
        if (before < 3) {
            ++it;
        } else {
            StartLeakAtCells(it->cells);
            it = waiting_.erase(it);
        }
    }
}

void cSC4DisasterInstancePipeBurst::StartLeakAtCells(const std::vector<std::pair<int, int>>& cells) {
    for (const auto& [x, z] : cells) {
        if (cISC4PipeOccupantView* pipe = GetPipeAtLocation(x, z)) {
            StartLeakAtPipe(pipe);
            pipe->Release();
        }
    }
}

bool cSC4DisasterInstancePipeBurst::StartLeakAtPipe(cISC4PipeOccupantView* pipe) {
    std::uint32_t cx = 0, cz = 0;
    pipe->GetCell(cx, cz);
    const int x = static_cast<int>(cx), z = static_cast<int>(cz);
    for (const tLeakyPipe& leak : leaks_)
        if (leak.cellX == x && leak.cellZ == z) return false;
    leaks_.emplace_back();
    tLeakyPipe& leak = leaks_.back();
    pipe->GetCenter(leak.position);
    leak.cellX = x;
    leak.cellZ = z;
    leak.effect = nullptr;
    CreateLeakEffectForPipe(leak);
    return true;
}

void cSC4DisasterInstancePipeBurst::CreateLeakEffectForPipe(tLeakyPipe& pipe) {
    ReleaseEffect(pipe.effect);
    const char* name = nullptr;
    if (viewLevel_ == 3) name = "burst_pipe_underground";
    else if (viewLevel_ == 1) name = "burst_pipe_aboveground";
    if (name) CreateParticleEffect(name, pipe.position, true, &pipe.effect);
    pipe.elapsed = 0;
    pipe.burstTime = static_cast<std::uint32_t>(Random().RandomSint32RangeUniform(kMinLeakTimeBeforeBurst, kMaxLeakTimeBeforeBurst));
    PostMessageA(kMessagePipeBurstCell, static_cast<std::uint64_t>(pipe.cellX), static_cast<std::uint64_t>(pipe.cellZ), 0);
}

void cSC4DisasterInstancePipeBurst::CreateParticleEffect(const char* name, const float* position, bool keep, cISC4VisualEffect** out) {
    *out = nullptr;
    if (!g_host || !g_host->CreateEffect(name, out)) return;
    float matrix[13] = {};
    matrix[0] = matrix[4] = matrix[8] = 1.0f;     // identity rotation
    matrix[9] = position[0];
    matrix[10] = position[1];
    matrix[11] = position[2];
    matrix[12] = 1.0f;                            // scale
    (*out)->SetTransform(matrix);
    (*out)->Start(0);
    if (!keep) {
        (*out)->Release();
        *out = nullptr;
    }
}

void cSC4DisasterInstancePipeBurst::PostMessageA(std::uint32_t type, std::uint64_t d1, std::uint64_t d2, std::uint64_t d3) {
    cIGZMessageServer2* server = DisasterHost() ? DisasterHost()->MessageServer() : nullptr;
    if (!server) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(d1);
    message->SetData2(d2);
    message->SetData3(d3);
    server->MessagePost(message, false);
    message->Release();
}

cISC4PipeOccupantView* cSC4DisasterInstancePipeBurst::GetPipeAtLocation(int x, int z) {
    cISC4Occupant* occupant = nullptr;
    if (!DisasterHost()->Occupants()->GetFirstOccupantByStandardCityCell(occupant, x, z, filter_)) return nullptr;
    cISC4PipeOccupantView* pipe = nullptr;
    occupant->QueryInterface(kGZIID_PortPipeOccupantView, reinterpret_cast<void**>(&pipe));
    occupant->Release();
    return pipe;
}

void cSC4DisasterInstancePipeBurst::BurstPipe(int x, int z) {
    cISC4PipeOccupantView* pipe = GetPipeAtLocation(x, z);
    if (!pipe) return;
    float center[3] = {0, 0, 0};
    pipe->GetCenter(center);
    cISC4VisualEffect* explosion = nullptr;
    CreateParticleEffect("pipe_explodes", center, false, &explosion);
    std::vector<std::pair<int, int>> neighbours;
    for (int i = 0; i < 4; ++i) {
        if (!pipe->IsConnected(i, 4)) continue;
        const int nx = kOffsetX[i] + x, nz = kOffsetZ[i] + z;
        if (nx > -1 && nz > -1 && nx < cellsX_ && nz < cellsZ_) neighbours.emplace_back(nx, nz);
    }
    if (!neighbours.empty()) waiting_.push_back(tLeaksInWait{0, std::move(neighbours)});
    if (g_host && g_host->HasDemolition()) {
        g_host->DemolishPipe(pipe->AsOccupant());
        PostMessageA(kMessagePipeBurstCell, static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(z), 0);
    }
    pipe->Release();
    CreateSinkHole(x, z);
}

void cSC4DisasterInstancePipeBurst::CreateSinkHole(int x, int z) {
    if (!g_host) return;
    const float cell = g_host->TerrainCellSize();
    SinkHoleDesc desc;
    desc.x = static_cast<float>(x) * cell;
    desc.z = static_cast<float>(z) * cell;
    g_host->AddSinkHole(0x14, desc);
}

}  // namespace sc4

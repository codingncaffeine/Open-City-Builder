#include "sc4/DisasterManagers.h"

#include <algorithm>

namespace sc4 {

namespace {
IDisasterInstanceFactory* g_factory = nullptr;
}
IDisasterInstanceFactory* DisasterInstanceFactory() { return g_factory; }
void SetDisasterInstanceFactory(IDisasterInstanceFactory* factory) { g_factory = factory; }

// ----------------------------------------------------------------------------------------- watered pipe filter

cSC4WateredPipeFilter::~cSC4WateredPipeFilter() {
    if (grid_) grid_->Release();
}

void cSC4WateredPipeFilter::SetGrid(IPlumbingGrid* grid) {
    if (grid_) grid_->Release();
    grid_ = grid;
    if (grid_) grid_->AddRef();
}

bool cSC4WateredPipeFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (occupant->GetType() != kOccupantTypeNetworkOccupant) return false;
    cISC4PipeOccupantView* view = nullptr;
    occupant->QueryInterface(kGZIID_cISC4PipeOccupant, reinterpret_cast<void**>(&view));
    if (!view) return false;
    std::uint32_t x = 0, z = 0;
    view->GetCell(x, z);
    const bool watered = grid_ && grid_->IsWatered(static_cast<int>(x), static_cast<int>(z));
    view->Release();
    return watered;
}

// ------------------------------------------------------------------------------------------------------ fire

bool cSC4DisasterManagerFire::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DisasterManagerFire) {
        *out = static_cast<cISC4DisasterManagerFire*>(this);
        AddRef();
        return true;
    }
    return cSC4DisasterManager::QueryInterface(iid, out);
}

cISC4DisasterInstanceFire* cSC4DisasterManagerFire::GetFireInstance() {
    if (instances_.empty()) return nullptr;
    cISC4DisasterInstanceFire* fire = nullptr;
    instances_.front()->QueryInterface(kGZIID_cISC4DisasterInstanceFire, reinterpret_cast<void**>(&fire));
    return fire;
}

cISC4DisasterInstanceFire* cSC4DisasterManagerFire::FireInstance() {
    if (!instances_.empty()) return GetFireInstance();
    IDisasterInstanceFactory* factory = DisasterInstanceFactory();
    if (!factory) return nullptr;
    cISC4DisasterInstanceFire* fire = factory->CreateFire();
    fire->Init();
    fire->AddRef();
    return fire;
}

bool cSC4DisasterManagerFire::Init() {
    cSC4DisasterManager::Init();
    if (cISC4DisasterInstanceFire* fire = FireInstance()) {
        fire->Update();
        fire->Release();
    }
    return true;
}

bool cSC4DisasterManagerFire::AttemptDisasterStart(int, int) {
    AttemptDisasterStart();
    return false;
}

std::uint32_t cSC4DisasterManagerFire::SetCause(std::uint32_t cause) {
    cISC4DisasterInstanceFire* fire = FireInstance();
    if (!fire) return 0;
    const std::uint32_t result = fire->SetCause(cause);
    fire->Release();
    return result;
}

std::uint32_t cSC4DisasterManagerFire::GetCause() {
    cISC4DisasterInstanceFire* fire = FireInstance();
    if (!fire) return 0;
    const std::uint32_t result = fire->GetCause();
    fire->Release();
    return result;
}

bool cSC4DisasterManagerFire::IgniteOccupant(cISC4Occupant* occupant) {
    cISC4DisasterInstanceFire* fire = FireInstance();
    if (!fire) return false;
    const bool result = fire->Ignite(occupant, false, 0, true);
    fire->Release();
    return result;
}

bool cSC4DisasterManagerFire::IgniteCell(std::uint32_t x, std::uint32_t z, bool a, bool b, std::uint32_t c) {
    cISC4DisasterInstanceFire* fire = FireInstance();
    if (!fire) return false;
    const bool result = fire->IgniteCell(x, z, a, b, c);
    fire->Release();
    return result;
}

std::uint32_t cSC4DisasterManagerFire::ExtinguishCell(std::uint32_t x, std::uint32_t z, int amount) {
    cISC4DisasterInstanceFire* fire = GetFireInstance();   // never creates one
    if (!fire) return 0;
    const std::uint32_t result = fire->ExtinguishCell(x, z, amount);
    fire->Release();
    return result;
}

bool cSC4DisasterManagerFire::QuickBurnCell(std::uint32_t x, std::uint32_t z) {
    cISC4DisasterInstanceFire* fire = FireInstance();
    if (!fire) return false;
    const bool result = fire->QuickBurnCell(x, z);
    fire->Release();
    return result;
}

bool cSC4DisasterManagerFire::IgniteCellRadius(std::uint32_t x, std::uint32_t z, std::uint32_t radius, std::uint32_t c) {
    cISC4DisasterInstanceFire* fire = FireInstance();
    if (!fire) return false;
    const bool result = fire->IgniteCellRadius(x, z, radius, c);
    fire->Release();
    return result;
}

// --------------------------------------------------------------------------------------------------- volcano

bool cSC4DisasterManagerVolcano::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DisasterManagerVolcano) {
        *out = static_cast<cISC4DisasterManagerVolcano*>(this);
        AddRef();
        return true;
    }
    return cSC4DisasterManager::QueryInterface(iid, out);
}

bool cSC4DisasterManagerVolcano::OnStandardMessage(cIGZMessage2Standard* message) {
    message->GetType();
    return true;
}

bool cSC4DisasterManagerVolcano::AttemptDisasterStart() {
    IDisasterHost* host = DisasterHost();
    cRZRandom random(0xffffffff);
    const int x = random.RandomSint32RangeUniform(0, host ? host->CityCellCountX() : 0);
    const int z = random.RandomSint32RangeUniform(0, host ? host->CityCellCountZ() : 0);
    return AttemptDisasterStart(x, z);
}

bool cSC4DisasterManagerVolcano::AttemptDisasterStart(int, int) {
    if (!instances_.empty()) return false;
    cISC4DisasterInstanceVolcano* instance = nullptr;
    if (!CreateVolcanoInstance(&instance)) return false;
    instance->Erupt();
    instance->Prepare();
    instance->SetPhase(3);
    return true;
}

bool cSC4DisasterManagerVolcano::CreateVolcanoInstance(cISC4DisasterInstanceVolcano** out) {
    IDisasterInstanceFactory* factory = DisasterInstanceFactory();
    if (!factory) return false;
    cISC4DisasterInstanceVolcano* instance = factory->CreateVolcano();
    AddInstanceToList(instance);
    *out = instance;
    return true;
}

float cSC4DisasterManagerVolcano::GetLavaHeightByCityCell(int x, int z) {
    if (instances_.empty()) return 0.0f;
    cISC4DisasterInstanceVolcano* volcano = nullptr;
    instances_.front()->QueryInterface(kGZIID_cISC4DisasterInstanceVolcano, reinterpret_cast<void**>(&volcano));
    if (!volcano) return 0.0f;
    const float height = volcano->GetLavaHeightByCityCell(x, z);
    volcano->Release();
    return height;
}

// ------------------------------------------------------------------------------------------------------ riots

bool cSC4DisasterManagerRiot::Init() {
    const bool ok = cSC4DisasterManager::Init();
    if (ok) ReadTuningParameters();
    return ok;
}

bool cSC4DisasterManagerRiot::Shutdown() {
    if (initialised_) ClearRiotNetworkOccupants();
    return cSC4DisasterManager::Shutdown();
}

void cSC4DisasterManagerRiot::ReadTuningParameters() {
    if (IDisasterInstanceFactory* factory = DisasterInstanceFactory()) {
        int maxInstances = snRiotMaxInstances, maxLeaders = snRiotMaxLeaders;
        if (factory->ReadRiotTuning(maxInstances, maxLeaders)) {
            snRiotMaxInstances = maxInstances;
            snRiotMaxLeaders = maxLeaders;
        }
    }
}

void cSC4DisasterManagerRiot::ClearRiotNetworkOccupants() {
    for (cISC4RiotNetworkOccupant* occupant : riotOccupants_) {
        occupant->ClearState(2);
        occupant->Release();
    }
    riotOccupants_.clear();
}

void cSC4DisasterManagerRiot::AddRiotNetworkOccupant(cISC4RiotNetworkOccupant* occupant) {
    if (!riotOccupants_.insert(occupant).second) return;
    occupant->SetState(2);
    occupant->AddRef();
}

bool cSC4DisasterManagerRiot::RemoveInstanceFromList(cISC4DisasterInstance* instance) {
    const bool result = cSC4DisasterManager::RemoveInstanceFromList(instance);
    if (instances_.empty()) ClearRiotNetworkOccupants();
    return result;
}

bool cSC4DisasterManagerRiot::AttemptDisasterStart(int x, int z) {
    IDisasterInstanceFactory* factory = DisasterInstanceFactory();
    if (factory && GetActiveInstanceCount() < snRiotMaxInstances) {
        cISC4DisasterInstanceRiot* instance = factory->CreateRiot();
        instance->AddRef();
        AddInstanceToList(instance);
        instance->Init();
        instance->AddRiotLeader(x, z, false);
        instance->Release();
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ pipe bursts

bool cSC4DisasterManagerPipeBurst::Init() {
    cSC4DisasterManager::Init();
    if (!filter_) {
        filter_ = new cSC4WateredPipeFilter(nullptr);
        filter_->AddRef();
        if (IDisasterInstanceFactory* factory = DisasterInstanceFactory()) {
            IPlumbingGrid* grid = factory->PlumbingGrid();   // already a counted reference, kept by the filter
            filter_->SetGrid(grid);
            if (grid) grid->Release();
        }
    }
    return true;
}

bool cSC4DisasterManagerPipeBurst::Shutdown() {
    if (filter_) {
        filter_->SetGrid(nullptr);
        filter_->Release();
        filter_ = nullptr;
    }
    cSC4DisasterManager::Shutdown();
    return true;
}

cISC4DisasterInstance* cSC4DisasterManagerPipeBurst::CreateDisasterInstance(cISC4Occupant* occupant) {
    IDisasterInstanceFactory* factory = DisasterInstanceFactory();
    if (!factory) return nullptr;
    cISC4DisasterInstancePipeBurst* instance = factory->CreatePipeBurst();
    instance->AddRef();
    cIGZUnknown* pipe = nullptr;
    occupant->QueryInterface(kGZIID_cISC4PipeOccupant, reinterpret_cast<void**>(&pipe));
    const bool ok = instance->Init(pipe);
    cISC4DisasterInstance* result = ok ? instance : nullptr;
    instance->Release();
    if (pipe) pipe->Release();
    return result;
}

bool cSC4DisasterManagerPipeBurst::AttemptDisasterStart() {
    IDisasterHost* host = DisasterHost();
    if (!host) return false;
    const int width = host->CityCellCountX(), height = host->CityCellCountZ();
    cRZRandom random(0xffffffff);
    const int startX = static_cast<int>(random.RandomUint32Uniform(static_cast<std::uint32_t>(width)));
    const int startZ = static_cast<int>(random.RandomUint32Uniform(static_cast<std::uint32_t>(height)));
    SpiralIterator spiral = SpiralIterator::Over(startX, startZ, width, height);
    int x = 0, z = 0;
    while (spiral.GetNext(x, z)) {
        cISC4Occupant* occupant = nullptr;
        if (host->Occupants()->GetFirstOccupantByStandardCityCell(occupant, x, z, filter_)) {
            cISC4DisasterInstance* instance = CreateDisasterInstance(occupant);
            occupant->Release();
            return instance != nullptr;
        }
    }
    return false;
}

bool cSC4DisasterManagerPipeBurst::AttemptDisasterStart(int x, int z) {
    IDisasterHost* host = DisasterHost();
    cISC4Occupant* occupant = nullptr;
    if (!host || !host->Occupants()->GetFirstOccupantByStandardCityCell(occupant, x, z, filter_)) return false;
    cISC4DisasterInstance* instance = CreateDisasterInstance(occupant);
    occupant->Release();
    return instance != nullptr;
}

// ------------------------------------------------------------------------------------------------ earthquakes

bool cSC4DisasterManagerEarthquake::AttemptDisasterStart(int x, int z) {
    IDisasterInstanceFactory* factory = DisasterInstanceFactory();
    if (!instances_.empty() || !factory) return false;
    cISC4DisasterInstanceEarthquake* instance = factory->CreateEarthquake();
    AddInstanceToList(instance);
    instance->Init(x, z);
    if (instance->GetPhase() != 2) instance->BeginDeformation();
    return true;
}

}  // namespace sc4

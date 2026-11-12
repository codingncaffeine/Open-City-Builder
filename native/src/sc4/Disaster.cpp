#include "sc4/Disaster.h"

#include <algorithm>

#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IDisasterHost* g_host = nullptr;

// cSC4DisasterLayer::Init()::dlist — the seven disaster managers, (manager id, GZCOM class id)
struct ManagerEntry { std::uint32_t id, classId; };
constexpr ManagerEntry kManagerList[7] = {
    {kDisasterManagerIdVolcano, kGZCLSID_cSC4DisasterManagerVolcano},
    {kDisasterManagerIdFire, kGZCLSID_cSC4DisasterManagerFire},
    {kDisasterManagerIdEarthquake, kGZCLSID_cSC4DisasterManagerEarthquake},
    {kDisasterManagerIdPipeBurst, kGZCLSID_cSC4DisasterManagerPipeBurst},
    {kDisasterManagerIdRiot, kGZCLSID_cSC4DisasterManagerRiot},
    {kDisasterManagerIdEffect, kGZCLSID_cSC4DisasterManagerEffect},
    {kDisasterManagerIdToxic, kGZCLSID_cSC4DisasterManagerToxic},
};

void Retain(cIGZUnknown* object) { if (object) object->AddRef(); }
void Drop(cIGZUnknown* object) { if (object) object->Release(); }
}  // namespace

IDisasterHost* DisasterHost() { return g_host; }
void SetDisasterHost(IDisasterHost* host) { g_host = host; }

// ------------------------------------------------------------------------------------------ the layer

cSC4DisasterLayer::cSC4DisasterLayer() = default;

cSC4DisasterLayer::~cSC4DisasterLayer() {
    ClearDispatchTimers();
    for (const Location& location : locations_) Drop(location.occupant);
}

std::uint32_t cSC4DisasterLayer::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4DisasterLayer::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == kGZIID_cISC4DisasterLayer) *out = static_cast<cISC4DisasterLayer*>(this);
    else if (iid == 1) *out = static_cast<cISC4DisasterLayer*>(this);
    if (!*out) return false;
    AddRef();
    return true;
}

// Creates (or finds) the seven managers and initialises them.
bool cSC4DisasterLayer::Init() {
    if (initialised_) return true;
    initialised_ = true;
    disableCount_ = 0;
    for (const ManagerEntry& entry : kManagerList) {
        cISC4DisasterManager* manager = GetDisasterManager(entry.id);
        if (!manager) {
            manager = g_host ? g_host->CreateManager(entry.classId) : nullptr;
            if (!manager) continue;
            managers_[manager->GetID()] = manager;
        }
        manager->Init();
    }
    ResetDisasterCost();
    return true;
}

bool cSC4DisasterLayer::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    EndDamageAssessment();
    for (auto& entry : managers_) {
        entry.second->Shutdown();
        entry.second->Release();
    }
    managers_.clear();
    return true;
}

void cSC4DisasterLayer::EnableDisasters(bool enable) {
    disableCount_ = std::max(0, disableCount_ + (enable ? -1 : 1));
}

cISC4DisasterManager* cSC4DisasterLayer::GetDisasterManager(std::uint32_t id) const {
    auto it = managers_.find(id);
    return it == managers_.end() ? nullptr : it->second;
}

void cSC4DisasterLayer::PostMessageToServer(std::uint32_t type, std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
    if (!server) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(a);
    message->SetData2(b);
    message->SetData3(c);
    server->MessagePost(message, false);
    message->Release();
}

// The first running disaster pauses the simulator, starts the damage assessment and disables saving; every
// disaster posts "disaster began" with the running count and the instance.
void cSC4DisasterLayer::DisasterHasBegun(cISC4DisasterInstance* instance) {
    if (ongoing_++ == 0) {
        if (g_host) g_host->EmergencyPause();
        StartDamageAssessment();
        if (g_host) g_host->DisableSave();
    }
    GetMostRecentDisasterLocation(instance, nullptr, nullptr);       // vslot 0x80 (result unused)
    cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
    if (!server) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMessageDisasterBegan);
    message->SetData1(static_cast<std::uint64_t>(ongoing_));
    message->SetData2(0);
    message->SetData3(0);
    message->SetData4(instance->GetID());
    message->SetIGZUnknown(instance);
    server->MessagePost(message, false);
    message->Release();
}

void cSC4DisasterLayer::StartDamageAssessment() {
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        for (std::uint32_t type : {0x99ef1143u, kMessageDispatchBegin, kMessageDispatchStop1, kMessageDispatchStop3,
                                   kMessageDispatchStop2})
            server->AddNotification(target, type);
    }
    ResetDisasterCost();
    StartDispatchTimers();
}

void cSC4DisasterLayer::DisasterHasEnded() {
    if (initialised_ && ongoing_ != 0 && --ongoing_ == 0) {
        if (g_host) {
            g_host->EnableSave();
            g_host->EmergencyResume();
        }
        EndDamageAssessment();
        PostMessageToServer(kMessageDisasterEnded, 0, 0, 0);
    }
}

void cSC4DisasterLayer::EndDamageAssessment() {
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        for (std::uint32_t type : {0x99ef1143u, kMessageDispatchBegin, kMessageDispatchStop1, kMessageDispatchStop3,
                                   kMessageDispatchStop2})
            server->RemoveNotification(target, type);
    }
    EndDispatchTimers();
}

void cSC4DisasterLayer::ResetDisasterCost() {
    cost_ = DisasterCost{};
    specialBuildings_.clear();
}

bool cSC4DisasterLayer::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    void* standard = nullptr;
    if (type == kMessageDispatchBegin || type == kMessageDispatchStop1 || type == kMessageDispatchStop2 ||
        type == kMessageDispatchStop3) {
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard) && standard)
            DoDispatchMessage(static_cast<cIGZMessage2Standard*>(standard)), message->Release();
    } else if (type == 0x99ef1143) {                       // occupant removed from the occupant manager
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard) && standard)
            DoOnOccupantRemoval(static_cast<cIGZMessage2Standard*>(standard)), message->Release();
    }
    return true;
}

// While a disaster runs every occupant removed from the city adds its damage cost (exemplar property
// kPropertyDamageCost, or a fixed cost for network tiles) and, for buildings, is counted by category bit; the
// buildings in categories 10 / 23 / 24 are remembered by name ("special buildings").
void cSC4DisasterLayer::DoOnOccupantRemoval(cIGZMessage2Standard* message) {
    if (!g_host || !g_host->CityEstablished()) return;
    auto* occupant = static_cast<cISC4Occupant*>(message->GetVoid1());
    if (!occupant) return;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    std::int64_t cost = 0;
    const bool found = cSCPropertyHelp::GetPropertyValue(holder, kPropertyDamageCost, cost);
    const std::uint32_t type = occupant->GetType();
    if (type == kDisasterOccupantTypeNetwork) {
        if (!found) cost = kDamageCostPerNetworkTile;
    } else if (type == kOccupantTypeBuilding) {
        void* building = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, &building) && building) {
            auto* b = static_cast<cISC4BuildingOccupant*>(building);
            const BuildingProfile& profile = b->GetBuildingProfile();
            ++cost_.buildingsDestroyed;
            std::uint32_t flags = profile.categoryFlags;
            if (flags & 1) { cost_.category0Cost += cost; ++cost_.category0Count; }
            if (flags & 2) { cost_.category1Cost += cost; ++cost_.category1Count; }
            if (flags & 4) { cost_.category2Cost += cost; ++cost_.category2Count; }
            if (flags & 0x20) { cost_.category5Cost += cost; ++cost_.category5Count; }
            if (flags & 0x1800400) {
                SpecialBuildingDamage entry;
                entry.name = b->GetBuildingName();
                entry.cost = cost;
                specialBuildings_.push_back(entry);
            }
            b->Release();
        }
    }
    cost_.totalCost += cost;
}

void cSC4DisasterLayer::DoDispatchMessage(cIGZMessage2Standard* message) {
    const std::uint32_t kind = static_cast<std::uint32_t>(message->GetData1());
    const std::uint32_t key = static_cast<std::uint32_t>(message->GetData2());
    std::map<std::uint32_t, cRZTimer*>* timers;
    if ((kind | 4) == 5) timers = &fireTimers_;
    else if (kind == 6 || kind == 0) timers = &policeTimers_;
    else return;
    cRZTimer*& timer = (*timers)[key];
    if (!timer) {
        timer = new cRZTimer(cRZTimer::Units(4));
        timer->SetUnits(cRZTimer::Units(5));
    }
    const std::uint32_t type = message->GetType();
    if (type == kMessageDispatchBegin) {
        if (!timer->IsTimerRunning()) timer->Start();
    } else if (type == kMessageDispatchStop1 || type == kMessageDispatchStop2 || type == kMessageDispatchStop3) {
        if (timer->IsTimerRunning()) timer->Stop();
    }
}

void cSC4DisasterLayer::ClearDispatchTimers() {
    for (auto& entry : fireTimers_) delete entry.second;
    fireTimers_.clear();
    for (auto& entry : policeTimers_) delete entry.second;
    policeTimers_.clear();
    totalTimer_.Reset();
}

void cSC4DisasterLayer::StartDispatchTimers() {
    ClearDispatchTimers();
    totalTimer_.SetUnits(cRZTimer::Units(5));
    totalTimer_.Reset();
    totalTimer_.Start();
}

// The share of the disaster during which dispatch vehicles were busy, per available dispatch (fire / police).
void cSC4DisasterLayer::EndDispatchTimers() {
    totalTimer_.Stop();
    float total = 0.0f;
    totalTimer_.GetElapsedTimeFloat(total);
    float part = 0.0f, fireBusy = 0.0f, policeBusy = 0.0f;
    for (auto& entry : fireTimers_) {
        entry.second->Stop();
        entry.second->GetElapsedTimeFloat(part);
        fireBusy += part;
    }
    for (auto& entry : policeTimers_) {
        entry.second->Stop();
        entry.second->GetElapsedTimeFloat(part);
        policeBusy += part;
    }
    if (!g_host) return;
    const std::uint32_t fire = g_host->FireDispatchCount();
    const std::uint32_t police = g_host->PoliceDispatchCount();
    fireDispatchRatio_ = fire != 0 ? fireBusy / (static_cast<float>(fire) * total) : 0.0f;
    policeDispatchRatio_ = police != 0 ? policeBusy / (static_cast<float>(police) * total) : 0.0f;
}

bool IDisasterHost::IsOccupantInCity(cISC4Occupant* occupant) {
    cISC4OccupantManager* manager = Occupants();
    return manager && manager->FindOccupant(occupant, false, nullptr, nullptr);
}

void cSC4DisasterLayer::ResolveDisasterLocation(Location& location) {
    if (!g_host) return;
    if (location.occupant) {
        if (g_host->IsOccupantInCity(location.occupant)) {
            float position[3] = {};
            location.occupant->GetPosition(position);
            int cellX = 0, cellZ = 0;
            g_host->PositionToCell(position[0], position[2], cellX, cellZ);
            location.x = cellX;
            location.z = cellZ;
        }
        return;                                            // an occupant that is gone keeps its last cell
    }
    if (location.instance) {
        int x = 0, z = 0;
        if (location.instance->GetLocation(x, z)) {
            location.x = x;
            location.z = z;
        }
    }
}

bool cSC4DisasterLayer::GetNextDisasterLocation(int* x, int* z, int* zoom) {
    const int count = static_cast<int>(locations_.size());
    if (count == 0) return false;
    if (currentLocation_ >= count) currentLocation_ = count - 1;
    Location& location = locations_[currentLocation_];
    ResolveDisasterLocation(location);
    if (x) *x = location.x;
    if (z) *z = location.z;
    if (zoom) *zoom = location.zoom;
    currentLocation_ = (currentLocation_ > 0 ? currentLocation_ : count) - 1;
    return true;
}

bool cSC4DisasterLayer::GetMostRecentDisasterLocation(cISC4DisasterInstance* instance, int* x, int* z) {
    const int count = static_cast<int>(locations_.size());
    for (int i = count - 1; i >= 0; --i) {
        if (instance && locations_[i].instance != instance) continue;
        ResolveDisasterLocation(locations_[i]);
        if (x) *x = locations_[i].x;
        if (z) *z = locations_[i].z;
        return true;
    }
    return false;
}

// Cycles through the registered locations most recent first and centres the camera on the next one.
bool cSC4DisasterLayer::GoToNextDisaster() {
    int cellX = 0, cellZ = 0, zoom = 0;
    if (!GetNextDisasterLocation(&cellX, &cellZ, &zoom)) return false;
    if (g_host) {
        float x = 0.0f, z = 0.0f;
        g_host->CellCenterToPosition(cellX, cellZ, x, z);
        g_host->MoveCamera(x, g_host->TerrainHeight(x, z), z, zoom);
    }
    return true;
}

bool cSC4DisasterLayer::RegisterDisasterLocation(cISC4DisasterInstance* instance, int x, int z,
                                                 cISC4Occupant* occupant, int zoom, bool jump) {
    Location location;
    location.instance = instance;
    location.x = x;
    location.z = z;
    location.zoom = zoom;
    location.occupant = occupant;
    Retain(occupant);
    locations_.push_back(location);
    currentLocation_ = static_cast<int>(locations_.size()) - 1;
    if (jump && g_host && g_host->AutoJumpToDisasters()) GoToNextDisaster();
    return true;
}

bool cSC4DisasterLayer::UnregisterDisasterLocations(cISC4DisasterInstance* instance) {
    bool removed = false;
    for (int i = static_cast<int>(locations_.size()) - 1; i >= 0; --i) {
        if (instance && locations_[i].instance != instance) continue;
        Drop(locations_[i].occupant);
        locations_.erase(locations_.begin() + i);
        removed = true;
    }
    return removed;
}

bool cSC4DisasterLayer::UnregisterDisasterLocationByOccupant(cISC4DisasterInstance* instance, cISC4Occupant* occupant) {
    bool removed = false;
    for (int i = static_cast<int>(locations_.size()) - 1; i >= 0; --i) {
        if (locations_[i].instance != instance || locations_[i].occupant != occupant) continue;
        Drop(occupant);
        locations_.erase(locations_.begin() + i);
        removed = true;
    }
    return removed;
}

bool cSC4DisasterLayer::GetDisasterLocationByIndex(int index, cISC4DisasterInstance** instance, int* x, int* z) const {
    if (index < 0 || index >= static_cast<int>(locations_.size())) return false;
    const Location& location = locations_[index];
    if (!location.instance) return false;
    *instance = location.instance;
    location.instance->AddRef();
    if (x) *x = location.x;
    if (z) *z = location.z;
    return true;
}

// ------------------------------------------------------------------------------------- manager base

std::uint32_t cSC4DisasterManager::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4DisasterManager::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == kGZIID_cISC4DisasterManager || iid == 1) *out = static_cast<cISC4DisasterManager*>(this);
    if (!*out) return false;
    AddRef();
    return true;
}

bool cSC4DisasterManager::Init() {
    initialised_ = true;
    return true;
}

// Shutting down shuts every instance down; removals requested meanwhile are queued and replayed afterwards.
bool cSC4DisasterManager::Shutdown() {
    if (initialised_) {
        iterating_ = true;
        for (cISC4DisasterInstance* instance : instances_) {
            instance->Shutdown();
            instance->Release();
        }
        instances_.clear();
        iterating_ = false;
        ProcessTempAddRemoveLists();
        initialised_ = false;
    }
    return true;
}

bool cSC4DisasterManager::AddInstanceToList(cISC4DisasterInstance* instance) {
    if (iterating_) {
        pendingAdd_.push_back(instance);
        return true;
    }
    if (std::find(instances_.begin(), instances_.end(), instance) != instances_.end()) return true;
    instances_.push_back(instance);
    instance->AddRef();
    return true;
}

bool cSC4DisasterManager::RemoveInstanceFromList(cISC4DisasterInstance* instance) {
    if (iterating_) {
        pendingRemove_.push_back(instance);
        return true;
    }
    auto it = std::find(instances_.begin(), instances_.end(), instance);
    if (it == instances_.end()) return false;
    instances_.erase(it);
    instance->Release();
    return true;
}

int cSC4DisasterManager::GetActiveInstanceCount() const {
    int count = 0;
    for (cISC4DisasterInstance* instance : instances_) count += instance->GetPhase() == 1 ? 1 : 0;
    return count;
}

void cSC4DisasterManager::ProcessTempAddRemoveLists() {
    std::list<cISC4DisasterInstance*> add;
    add.swap(pendingAdd_);
    for (cISC4DisasterInstance* instance : add) AddInstanceToList(instance);
    std::list<cISC4DisasterInstance*> remove;
    remove.swap(pendingRemove_);
    for (cISC4DisasterInstance* instance : remove) RemoveInstanceFromList(instance);
}

bool cSC4DisasterManager::DoMessage(cIGZMessage2* message) {
    void* standard = nullptr;
    message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard);
    return OnStandardMessage(static_cast<cIGZMessage2Standard*>(standard));
}

// ------------------------------------------------------------------------------- effect disasters

cSC4DisasterInstanceEffect::cSC4DisasterInstanceEffect(cISC4VisualEffect* effect, std::uint32_t id)
    : id_(id), effect_(effect) {
    Retain(effect_);
}

cSC4DisasterInstanceEffect::~cSC4DisasterInstanceEffect() { Drop(effect_); }

std::uint32_t cSC4DisasterInstanceEffect::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4DisasterInstanceEffect::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DisasterInstance) *out = static_cast<cISC4DisasterInstance*>(this);
    else if (iid == 1) *out = this;
    if (!*out) return false;
    AddRef();
    return true;
}

bool cSC4DisasterInstanceEffect::GetLocation(int& x, int& z) const {
    x = cellX_;
    z = cellZ_;
    return true;
}

bool cSC4DisasterInstanceEffect::Init() {
    if (initialised_) return true;
    initialised_ = true;
    if (effect_) {
        effect_->SetCompletionMessage(kMessageEffectFinished, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this)));
        cSC4DisasterInstanceEffect* self = this;
        effect_->SetPointerParameter(0xd, &self, 1);
        effect_->Start(0);
    }
    layer_ = g_host ? g_host->DisasterLayer() : nullptr;
    Retain(layer_);
    if (layer_) {
        manager_ = layer_->GetDisasterManager(kDisasterManagerIdEffect);
        Retain(manager_);
    }
    if (manager_) manager_->AddInstanceToList(this);
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageEffectFinished);
        server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageEffectMoved);
    }
    if (layer_) layer_->DisasterHasBegun(this);
    return true;
}

bool cSC4DisasterInstanceEffect::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    if (effect_ && effect_->IsPlaying()) effect_->Stop(1);
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageEffectFinished);
        server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageEffectMoved);
    }
    if (layer_) {
        layer_->DisasterHasEnded();
        layer_->UnregisterDisasterLocations(this);
        layer_->Release();
        layer_ = nullptr;
    }
    if (manager_) {
        manager_->RemoveInstanceFromList(this);
        manager_->Release();
        manager_ = nullptr;
    }
    return true;
}

bool cSC4DisasterInstanceEffect::DoMessage(cIGZMessage2* message) {
    void* standard = nullptr;
    const std::uint32_t type = message->GetType();
    if (type != kMessageEffectMoved && type != kMessageEffectFinished) return true;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard) || !standard) return true;
    auto* m = static_cast<cIGZMessage2Standard*>(standard);
    if (type == kMessageEffectMoved) {
        if (m->GetVoid1() == static_cast<void*>(this)) {
            cellX_ = static_cast<std::int32_t>(m->GetData2());
            cellZ_ = static_cast<std::int32_t>(m->GetData3());
        }
    } else if (static_cast<std::uint32_t>(m->GetData1()) == static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this))) {
        Shutdown();
    }
    m->Release();
    return true;
}

void cSC4DisasterInstanceEffect::SetLocation(const float* position) {
    if (!g_host) return;
    g_host->PositionToCell(position[0], position[2], cellX_, cellZ_);
    if (layer_) layer_->RegisterDisasterLocation(this, cellX_, cellZ_, nullptr, 0, false);
}

void cSC4DisasterInstanceEffect::CancelAttract() {
    int zero = 0;
    if (effect_) effect_->SetIntParameter(0xb, &zero, 1);
}

bool cSC4DisasterManagerEffect::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DisasterManagerEffect) {
        *out = static_cast<cISC4DisasterManagerEffect*>(this);
        AddRef();
        return true;
    }
    return cSC4DisasterManager::QueryInterface(iid, out);
}

cISC4DisasterInstance* cSC4DisasterManagerEffect::StartDisasterEffect(cISC4VisualEffect* effect,
                                                                       const cS3DVector3& position,
                                                                       std::uint32_t id) {
    auto* instance = new cSC4DisasterInstanceEffect(effect, id);
    instance->AddRef();
    instance->Init();
    const float p[3] = {position.x, position.y, position.z};
    instance->SetLocation(p);
    return instance;
}

void cSC4DisasterManagerEffect::AttractDisasterEffect(const float* position) {
    if (GetActiveInstanceCount() == 0) return;
    if (auto* first = dynamic_cast<cSC4DisasterInstanceEffect*>(instances_.front())) first->SetLocation(position);
}

void cSC4DisasterManagerEffect::CancelAttract() {
    if (GetActiveInstanceCount() == 0) return;
    if (auto* first = dynamic_cast<cSC4DisasterInstanceEffect*>(instances_.front())) first->CancelAttract();
}

// -------------------------------------------------------------------------------- toxic disasters

cSC4DisasterInstanceToxic::~cSC4DisasterInstanceToxic() {
    for (cSC4ToxicOccupant* occupant : waste_) occupant->Release();
}

std::uint32_t cSC4DisasterInstanceToxic::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4DisasterInstanceToxic::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cSC4DisasterInstanceToxic || iid == 1) *out = this;
    else if (iid == kGZIID_cISC4DisasterInstance) *out = static_cast<cISC4DisasterInstance*>(this);
    if (!*out) return false;
    AddRef();
    return true;
}

bool cSC4DisasterInstanceToxic::Init() {
    if (initialised_) return true;
    initialised_ = true;
    timer_.Start();
    layer_ = g_host ? g_host->DisasterLayer() : nullptr;
    Retain(layer_);
    if (layer_) {
        manager_ = layer_->GetDisasterManager(kDisasterManagerIdToxic);
        Retain(manager_);
    }
    if (manager_) manager_->AddInstanceToList(this);
    if (cIGZMessageServer2* server = g_host ? g_host->NotificationServer() : nullptr)
        server->AddNotification(static_cast<cIGZMessageTarget2*>(this), 0xff);
    if (layer_) layer_->DisasterHasBegun(this);
    return true;
}

bool cSC4DisasterInstanceToxic::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    if (cIGZMessageServer2* server = g_host ? g_host->NotificationServer() : nullptr)
        server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), 0xff);
    if (layer_) {
        layer_->DisasterHasEnded();
        layer_->UnregisterDisasterLocations(this);
        layer_->Release();
        layer_ = nullptr;
    }
    if (manager_) {
        manager_->RemoveInstanceFromList(this);
        manager_->Release();
        manager_ = nullptr;
    }
    return true;
}

bool cSC4DisasterInstanceToxic::DoMessage(cIGZMessage2* message) {
    if (message->GetType() == kMessageToxicTick) Update();
    return true;
}

// Spilling waste on a cell that already has some adds to it; otherwise a new toxic occupant is placed.
bool cSC4DisasterInstanceToxic::SpillToxicWaste(int cellX, int cellZ, int amount) {
    for (cSC4ToxicOccupant* occupant : waste_) {
        if (occupant->GetCellX() == cellX && occupant->GetCellZ() == cellZ) {
            occupant->SetAmount(occupant->GetAmount() + static_cast<std::uint32_t>(amount));
            return true;
        }
    }
    auto* occupant = new cSC4ToxicOccupant();
    occupant->AddRef();
    occupant->SetCell(cellX, cellZ);
    occupant->Init();
    occupant->SetAmount(static_cast<std::uint32_t>(amount));
    if (g_host && g_host->Occupants()) g_host->Occupants()->InsertOccupant(occupant->AsOccupant(), 0);
    waste_.push_back(occupant);
    if (layer_) layer_->RegisterDisasterLocation(this, cellX, cellZ, occupant->AsOccupant(), 0, false);
    return true;
}

int cSC4DisasterInstanceToxic::GetToxicCountInCellRect(const SC4CellRect& rect) const {
    int count = 0;
    for (const cSC4ToxicOccupant* occupant : waste_) {
        const int x = occupant->GetCellX(), z = occupant->GetCellZ();
        if (z <= rect.z1 && x <= rect.x1 && rect.z0 <= z && rect.x0 <= x) ++count;
    }
    return count;
}

// Once a second: waste that was hosed shrinks four times as fast as the hose strength, otherwise by one; a
// cell that reaches zero is removed from the city (and its disaster location unregistered).
void cSC4DisasterInstanceToxic::Update() {
    const std::uint32_t elapsed = timer_.GetElapsedTimeU32();
    if (elapsed < nextUpdate_) return;
    nextUpdate_ = elapsed + 1000;
    for (auto it = waste_.begin(); it != waste_.end();) {
        cSC4ToxicOccupant* occupant = *it;
        auto next = std::next(it);
        const std::uint32_t hosed = occupant->GetHosed();
        std::uint32_t amount = occupant->GetAmount();
        if (hosed == 0) amount -= 1;
        else amount = amount < hosed * 4 ? 0 : amount - hosed * 4;
        occupant->SetAmount(amount);
        if (amount == 0) {
            if (g_host && g_host->Occupants()) g_host->Occupants()->RemoveOccupant(occupant->AsOccupant(), true, 0);
            occupant->Shutdown();
            waste_.erase(it);
            occupant->Release();
            if (layer_) layer_->UnregisterDisasterLocationByOccupant(this, occupant->AsOccupant());
        }
        it = next;
    }
    if (waste_.empty()) Shutdown();
}

bool cSC4DisasterManagerToxic::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DisasterManagerToxic) {
        *out = static_cast<cISC4DisasterManagerToxic*>(this);
        AddRef();
        return true;
    }
    return cSC4DisasterManager::QueryInterface(iid, out);
}

bool cSC4DisasterManagerToxic::Init() {
    const bool result = cSC4DisasterManager::Init();
    if (g_host) {
        g_host->RegisterCommand(kMessageToxicSpill, "ToxicSpill");
        if (cIGZMessageServer2* server = g_host->MessageServer())
            server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageToxicSpill);
    }
    return result;
}

bool cSC4DisasterManagerToxic::Shutdown() {
    if (g_host) {
        g_host->UnregisterCommand(kMessageToxicSpill);
        if (cIGZMessageServer2* server = g_host->MessageServer())
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageToxicSpill);
    }
    return cSC4DisasterManager::Shutdown();
}

// Returns a new reference: the live instance, or a freshly created one when no toxic disaster is running.
cSC4DisasterInstanceToxic* cSC4DisasterManagerToxic::GetInstance() {
    if (instances_.empty()) {
        auto* instance = new cSC4DisasterInstanceToxic();
        instance->Init();
        instance->AddRef();
        return instance;
    }
    void* found = nullptr;
    instances_.front()->QueryInterface(kGZIID_cSC4DisasterInstanceToxic, &found);
    return static_cast<cSC4DisasterInstanceToxic*>(found);
}

int cSC4DisasterManagerToxic::GetToxicCountInCellRect(const SC4CellRect& rect) {
    if (instances_.empty()) return 0;
    cSC4DisasterInstanceToxic* instance = GetInstance();
    const int count = instance ? instance->GetToxicCountInCellRect(rect) : 0;
    Drop(instance ? static_cast<cISC4DisasterInstance*>(instance) : nullptr);
    return count;
}

int cSC4DisasterManagerToxic::GetToxicCount() {
    if (instances_.empty()) return 0;
    cSC4DisasterInstanceToxic* instance = GetInstance();
    const int count = instance ? instance->GetToxicCount() : 0;
    Drop(instance ? static_cast<cISC4DisasterInstance*>(instance) : nullptr);
    return count;
}

bool cSC4DisasterManagerToxic::AttemptDisasterStart(int cellX, int cellZ) {
    cSC4DisasterInstanceToxic* instance = GetInstance();
    if (instance) {
        instance->SpillToxicWaste(cellX, cellZ, 0x78);
        instance->Release();
    }
    return false;
}

bool cSC4DisasterManagerToxic::OnStandardMessage(cIGZMessage2Standard* message) {
    if (!message || message->GetType() != kMessageToxicSpill) return true;
    auto* params = static_cast<cIGZCommandParameterSet*>(message->GetVoid1());
    if (!params) return true;
    const int cellX = (*params)[0].GetValSint32();
    const int cellZ = (*params)[1].GetValSint32();
    const int amount = (*params)[2].GetValSint32();
    cSC4DisasterInstanceToxic* instance = GetInstance();
    if (instance) {
        instance->SpillToxicWaste(cellX, cellZ, amount);
        instance->Release();
    }
    return true;
}

}  // namespace sc4

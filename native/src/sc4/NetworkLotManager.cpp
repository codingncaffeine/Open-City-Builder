// cSC4NetworkLotManager — recovered from SimCity 4 Deluxe Edition (Aspyr macOS x86_64) v1.2.1.
// See NetworkLotManager.h for the object layout, the quirk list and the seam documentation. The
// original addresses are in the comments; the vtable slot numbers refer to the primary interface
// (0x100aacd70) unless stated otherwise.
#include "sc4/NetworkLotManager.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
INetworkLotManagerHost* g_host = nullptr;

INetworkLotManagerHost& Host() { return *g_host; }

// The original formats "Missing lot configuration 0x%08x\n" with cRZString::Sprintf and then never
// prints it (the output call is gone from the binary) — the message is discarded exactly like this.
void DiscardMissingConfigurationMessage(std::uint32_t configurationId) {
    char message[64];
    std::snprintf(message, sizeof message, "Missing lot configuration 0x%08x\n", configurationId);
    (void)message;
}

// The key of a network-lot configuration exemplar: {0x6534284a, 0x89ac5643, configuration id}.
cGZPersistResourceKey ConfigurationKey(std::uint32_t configurationId) {
    cGZPersistResourceKey key;
    key.type = kNetworkLotConfigKeyType;
    key.group = kNetworkLotConfigKeyGroup;
    key.instance = configurationId;
    return key;
}

std::uint32_t RandomIndex(std::uint32_t count) {
    return NetworkLotManagerRandom().RandomUint32Uniform(count);
}
}  // namespace

void SetNetworkLotManagerHost(INetworkLotManagerHost* host) { g_host = host; }
INetworkLotManagerHost* NetworkLotManagerHost() { return g_host; }

cRZRandomSuper& NetworkLotManagerRandom() {
    static cRZRandomSuper random{0xffffffffu};   // the framework's process-wide sRandom
    return random;
}

// ------------------------------------------------------------------------------------------------ object

// @ 0x100404dce. The reserved words are initialised with the ctor's own values (0x8000000000000001 at
// +0x7c, 0x7fffffff80000000 at +0x84 and 0x7fffffff at +0x8c); Init overwrites most of them.
cSC4NetworkLotManager::cSC4NetworkLotManager() = default;

// @ 0x100404ebc. Destroys the two trailing hash tables, releases the next-piece configuration, then
// destroys the configuration and lot tables. The network lots themselves are only released by Shutdown
// (quirk: a destructor without Shutdown leaks them).
cSC4NetworkLotManager::~cSC4NetworkLotManager() {
    updateCells_.clear();
    potholes_.clear();
    if (nextPieceConfiguration_) {
        nextPieceConfiguration_->Release();
        nextPieceConfiguration_ = nullptr;
    }
    configurations_.clear();
    networkLots_.clear();
}

// @ 0x100404fbc. The shared count is at +0xb8; it starts at 0 (the original constructors leave it so).
std::uint32_t cSC4NetworkLotManager::AddRef() { return ++refCount_; }

// @ 0x100404ff8. With a count of 1 or 0 this calls the deleting destructor and returns 0 (no decrement).
std::uint32_t cSC4NetworkLotManager::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// @ 0x100405072. The iid is compared as a signed int, so the two negative ids (0x89a5ba3f and
// 0xe4fda3d4) take the first branch and iid 1 is the primary interface.
bool cSC4NetworkLotManager::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (static_cast<std::int32_t>(iid) < 1) {
        if (iid == kGZIID_cISC4NetworkLotManager) {
            *out = static_cast<cISC4NetworkLotManager*>(this);
        } else if (iid == kGZIID_cIGZSerializableNetworkLot) {
            *out = static_cast<cIGZSerializable*>(this);
        } else {
            return false;
        }
    } else if (iid == 1) {
        *out = static_cast<cISC4NetworkLotManager*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2NetworkLot) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// --------------------------------------------------------------------------------------------- lifetime

// @ 0x1004050d2. Subscribes to the six messages, measures the city, loads the configurations and
// reattaches the configurations of lots that came from a stream (by the id stored at lot +0x18).
bool cSC4NetworkLotManager::Init() {
    if (inited_) return true;
    inited_ = true;
    cIGZMessageServer2* server = Host().MessageServer();
    cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
    for (std::uint32_t message : kNetworkLotManagerMessages) {
        if (server) server->AddNotification(target, message);   // slot 0x28
    }
    const int cellsX = Host().CityCellCountX();
    const int cellsZ = Host().CityCellCountZ();
    reserved80_ = 0;
    reserved84_ = 0;
    maxCellX_ = cellsX - 1;
    maxCellZ_ = cellsZ - 1;
    if (cellsX < 1) maxCellX_ = 0;
    if (cellsZ < 1) maxCellZ_ = 0;
    reserved74_ = 0;
    reserved78_ = 0;
    dimensionFlag_ = static_cast<std::uint8_t>(
        ((static_cast<std::uint32_t>(maxCellX_) | static_cast<std::uint32_t>(maxCellZ_)) >> 30) & 2u);
    LoadAllConfigurations();

    std::vector<std::uint16_t> missing;
    for (auto& entry : networkLots_) {
        cSC4NetworkLot* lot = entry.second;
        cSC4NetworkLotConfiguration* found = nullptr;
        for (auto& bucket : configurations_) {
            for (cSC4NetworkLotConfiguration* configuration : bucket.second) {
                if (configuration->GetID() == lot->GetConfigurationId()) {
                    found = configuration;
                    break;
                }
            }
        }
        if (!found) {
            DiscardMissingConfigurationMessage(lot->GetConfigurationId());
            missing.push_back(entry.first);
            continue;
        }
        if (lot->GetConfiguration() != found) lot->SetConfiguration(found);   // AddRef new, release old
    }
    for (std::uint16_t key : missing) {   // (the original erases them without the release)
        auto it = networkLots_.find(key);
        if (it == networkLots_.end()) continue;
        it->second->Release();
        networkLots_.erase(it);
    }
    return true;
}

// @ 0x1004053c8. Asks the resource manager for the enumerator of the exemplar resource type and feeds
// every key to AddLotConfigurationStatic.
void cSC4NetworkLotManager::LoadAllConfigurations() {
    if (!Host().HasResourceManager()) return;
    INetworkLotResourceEnumerator* enumerator = Host().GetExemplarEnumerator(kNetworkLotConfigKeyType);
    if (!enumerator) return;
    enumerator->Enumerate(&cSC4NetworkLotManager::AddLotConfigurationStatic, this);
    enumerator->Release();
}

// @ 0x100405422. Unsubscribes, releases every network lot (the registry map is *not* cleared), releases
// every configuration and pothole and clears those two maps.
bool cSC4NetworkLotManager::Shutdown() {
    if (!inited_) return true;
    inited_ = false;
    cIGZMessageServer2* server = Host().MessageServer();
    cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
    for (std::uint32_t message : kNetworkLotManagerMessages) {
        if (server) server->RemoveNotification(target, message);   // slot 0x30
    }
    for (auto& entry : networkLots_) entry.second->Release();
    for (auto& bucket : configurations_) {
        for (cSC4NetworkLotConfiguration* configuration : bucket.second) configuration->Release();
    }
    configurations_.clear();
    for (auto& entry : potholes_) {
        INetworkLotVisualEffect* effect = entry.second;
        effect->StopEffect(true);   // slot 0x40
        effect->Release();
    }
    potholes_.clear();
    return true;
}

// ----------------------------------------------------------------------------------------------- stream

// @ 0x10040555a. Version 2: every lot as (key, occupant, configuration id), then every pothole.
bool cSC4NetworkLotManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    stream.SetUint32(static_cast<std::uint32_t>(networkLots_.size()));
    for (const auto& entry : networkLots_) {
        cSC4NetworkLot* lot = entry.second;
        stream.SetUint16(entry.first);
        Host().WriteGZSerializable(stream, lot->GetNetworkOccupant());
        // The binary dereferences the lot's configuration object here without a null check; a lot that
        // came from Read has no object yet (Init reattaches it), so the port falls back to the stored id.
        const cSC4NetworkLotConfiguration* configuration = lot->GetConfiguration();
        const std::uint32_t configurationId = configuration ? configuration->GetID() : lot->GetConfigurationId();
        stream.SetUint32(configurationId);
    }
    stream.SetUint32(static_cast<std::uint32_t>(potholes_.size()));
    for (const auto& entry : potholes_) {
        stream.SetUint16(entry.first);
        Host().WriteGZSerializable(stream, entry.second);
    }
    return !stream.GetError();
}

// @ 0x10040562a. Only version 2 is accepted. Lots are created with reference count 1 and the
// configuration *id* is restored (the pointer is reattached by Init); potholes are owned by the map.
bool cSC4NetworkLotManager::Read(cIGZIStream& stream) {
    std::int16_t version = 0;
    stream.GetSint16(version);
    if (version != 2) return false;
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t key = 0;
        stream.GetUint16(key);
        auto* lot = new cSC4NetworkLot();
        lot->AddRef();   // the original's inlined constructor leaves the count at 1
        void* occupant = nullptr;
        Host().ReadGZSerializable(stream, kGZIID_cISC4NetworkOccupant, &occupant);
        if (occupant) {
            auto* unknown = static_cast<cIGZUnknown*>(occupant);
            lot->SetNetworkOccupant(unknown);   // takes its own reference
            unknown->Release();                 // drop the stream's reference
        }
        std::uint32_t configurationId = 0;
        stream.GetUint32(configurationId);
        lot->SetConfigurationId(configurationId);
        networkLots_[key] = lot;
    }
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t key = 0;
        stream.GetUint16(key);
        void* effect = nullptr;
        Host().ReadGZSerializable(stream, kGZIID_cISC4PropOccupantVisualEffect, &effect);
        auto* visual = static_cast<INetworkLotVisualEffect*>(effect);
        if (visual) potholes_[key] = visual;   // the map takes the stream's reference
    }
    return !stream.GetError();
}

// ------------------------------------------------------------------------------------- configurations

// @ 0x100405868. Static enumerator callback: load the exemplar, accept only exemplar type 0x21, parse it
// and add the configuration.
void cSC4NetworkLotManager::AddLotConfigurationStatic(const cGZPersistResourceKey& key, void* context) {
    auto* self = static_cast<cSC4NetworkLotManager*>(context);
    if (!self || !g_host) return;
    cISCPropertyHolder* holder = Host().AcquireExemplar(key, kNetworkLotConfigExemplarClass);
    if (!holder) return;
    std::uint32_t exemplarType = 0;
    if (cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigExemplarTypeProperty, exemplarType) &&
        exemplarType == kNetworkLotConfigExemplarType) {
        auto* configuration = new cSC4NetworkLotConfiguration();
        configuration->AddRef();
        if (self->ReadLotConfiguration(holder, configuration)) self->AddLotConfiguration(configuration);
        configuration->Release();
    }
    Host().ReleaseExemplar(holder);
}

// @ 0x100405946. Reads the properties of a network-lot configuration. Takes a second reference on the
// configuration while it fills it in (quirk) and releases it before returning true.
bool cSC4NetworkLotManager::ReadLotConfiguration(cISCPropertyHolder* holder,
                                                 cSC4NetworkLotConfiguration* configuration) {
    if (!holder || !configuration) return false;
    std::uint8_t version = 1;
    cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigVersion, version);
    std::uint32_t pieceId = 0;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigPieceId, pieceId)) return false;
    configuration->AddRef();
    configuration->SetNetworkPieceId(pieceId);
    cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigName, configuration->Name());
    std::uint32_t id = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigId, id);
    configuration->SetID(id);

    std::vector<std::uint8_t> bytes;
    if (cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigZoneTypes, bytes) && !bytes.empty()) {
        std::vector<bool>& zoneTypes = configuration->ZoneTypeBits();
        for (std::uint8_t value : bytes) {
            if (value < zoneTypes.size()) zoneTypes[value] = true;
        }
    }
    if (cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigWealthTypes, bytes) && !bytes.empty()) {
        std::vector<bool>& wealthTypes = configuration->WealthTypeBits();
        for (std::uint8_t value : bytes) {
            if (value < wealthTypes.size()) wealthTypes[value] = true;
        }
    }
    if (cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigNeighborhood, bytes) && bytes.size() == 4) {
        std::memcpy(configuration->NeighborhoodBytes(), bytes.data(), 4);
    }
    cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigSize, configuration->Field58());
    cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigMinSlope, configuration->MinimumSlope());
    cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigMaxSlope, configuration->MaximumSlope());
    cSC4LotConfiguration::ConvertPropertiesToObjectArray(*holder, configuration->GetObjectArray(), version, 3);
    if (!configuration->GetObjectArray().empty()) {
        // The original reallocates the object array through a temporary (std::vector<T>(v).swap(v)).
        std::vector<cSC4LotConfigurationObject> copy(configuration->GetObjectArray());
        configuration->GetObjectArray().swap(copy);
    }
    cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigRotationMask, configuration->RotationMask());
    cSCPropertyHelp::GetPropertyValue(holder, kNetworkLotConfigFlipMode, configuration->FlipMode());
    cSC4LotConfiguration::CorrectObjectPositions(configuration->GetObjectArray(), kNetworkLotObjectBounds[0],
                                                 kNetworkLotObjectBounds[1], kNetworkLotObjectBounds[2],
                                                 kNetworkLotObjectBounds[3]);
    if (version < 2) {
        // Version-1 configurations are upgraded and written back into the database right away. The
        // segment the resource manager hands out is borrowed (the binary never releases it); the write
        // itself takes and drops its own reference.
        INetworkLotDBSegment* segment = Host().GetConfigDBSegment(ConfigurationKey(configuration->GetID()));
        WriteLotConfiguration(configuration, segment);
    }
    configuration->Release();
    return true;
}

// @ 0x100405c50. Takes a reference for the table, keyed by the configuration's network piece id.
void cSC4NetworkLotManager::AddLotConfiguration(cSC4NetworkLotConfiguration* configuration) {
    if (!configuration) return;
    configuration->AddRef();
    configurations_[configuration->GetNetworkPieceId()].push_back(configuration);
}

// @ 0x100405d3e. Writes the configuration properties into a fresh/existing exemplar, commits the
// exemplar into the database segment and updates the piece table (replace the entry with the same id or
// append). Returns true when the table was updated.
bool cSC4NetworkLotManager::WriteLotConfiguration(cSC4NetworkLotConfiguration* configuration,
                                                  INetworkLotDBSegment* segment) {
    if (segment) segment->AddRef();
    if (configuration) configuration->AddRef();
    if (!configuration || !Host().HasResourceManager()) {
        if (configuration) configuration->Release();
        if (segment) segment->Release();
        return false;
    }
    const cGZPersistResourceKey key = ConfigurationKey(configuration->GetID());
    cISCPropertyHolder* holder = Host().AcquireExemplar(key, kNetworkLotConfigExemplarClass);
    if (!holder) {
        configuration->Release();
        if (segment) segment->Release();
        return false;
    }
    holder->RemoveAllProperties(false);   // slot 0x90
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigVersion, std::uint8_t{2});
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigPieceId, configuration->GetNetworkPieceId());
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigName, configuration->Name());
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigId, configuration->GetID());
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigExemplarTypeProperty,
                                      kNetworkLotConfigExemplarType);
    std::vector<std::uint8_t> bytes;
    if (!configuration->ZoneTypeBits().empty()) {
        const std::vector<bool>& zoneTypes = configuration->ZoneTypeBits();
        for (std::size_t i = 0; i < zoneTypes.size(); ++i) {
            if (zoneTypes[i]) bytes.push_back(static_cast<std::uint8_t>(i));
        }
        cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigZoneTypes, bytes);
    }
    bytes.clear();
    if (!configuration->WealthTypeBits().empty()) {
        const std::vector<bool>& wealthTypes = configuration->WealthTypeBits();
        for (std::size_t i = 0; i < wealthTypes.size(); ++i) {
            if (wealthTypes[i]) bytes.push_back(static_cast<std::uint8_t>(i));
        }
        cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigWealthTypes, bytes);
    }
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigSize, configuration->Field58());
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigMinSlope, configuration->MinimumSlope());
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigMaxSlope, configuration->MaximumSlope());
    bytes.assign(configuration->NeighborhoodBytes(), configuration->NeighborhoodBytes() + 4);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigNeighborhood, bytes);
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigRotationMask, configuration->RotationMask());
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkLotConfigFlipMode, configuration->FlipMode());
    cSC4LotConfiguration::ConvertObjectArrayToProperties(*holder, configuration->GetObjectArray());
    bool stored = Host().StoreExemplar(holder, key, segment);
    Host().ReleaseExemplar(holder);
    bool updated = false;
    if (stored) {
        auto existing = configurations_.find(configuration->GetNetworkPieceId());
        if (existing == configurations_.end()) {
            configuration->AddRef();
            configurations_[configuration->GetNetworkPieceId()].push_back(configuration);
            updated = true;
        } else {
            std::vector<cSC4NetworkLotConfiguration*>& list = existing->second;
            bool replaced = false;
            for (cSC4NetworkLotConfiguration*& entry : list) {
                if (entry->GetID() == configuration->GetID()) {
                    if (entry != configuration) {
                        entry->Release();
                        entry = configuration;
                        configuration->AddRef();
                    }
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                configuration->AddRef();
                list.push_back(configuration);
            }
            updated = true;
        }
    }
    configuration->Release();
    if (segment) segment->Release();
    return updated;
}

// @ 0x1004062a6. Clears the set, then adds every configuration with a reference. Each pointer is
// inserted (a duplicate is dropped) but always referenced, like the original.
void cSC4NetworkLotManager::GetNetworkLotConfigurationSet(
    std::set<cSC4NetworkLotConfiguration*>& out) const {
    out.clear();
    for (const auto& bucket : configurations_) {
        for (cSC4NetworkLotConfiguration* configuration : bucket.second) {
            out.insert(configuration);
            configuration->AddRef();
        }
    }
}

// @ 0x1004063de. Walks the registry; the callback returns false to stop (the result is returned).
bool cSC4NetworkLotManager::IterateAllNetworkLots(
    bool (*callback)(std::uint32_t, std::uint32_t, cSC4NetworkLot*, void*), void* context) {
    if (!callback) return true;
    for (const auto& entry : networkLots_) {
        if (!callback(entry.first >> 8, entry.first & 0xffu, entry.second, context)) return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ messages

// @ 0x100406428. The six message branches (see kNetworkLotManagerMessages). Always returns true.
bool cSC4NetworkLotManager::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    switch (type) {
        case 0x89d3e9f3: {   // network occupant usable changed
            cIGZUnknown* unknown = standard->GetIGZUnknown();   // slot 0x100
            if (!unknown) break;
            INetworkLotNetworkOccupant* occupant = Host().NetworkOccupantOf(unknown);   // QI 0xa821ef94
            if (!occupant) break;
            if (occupant->HasAnyNetworkFlag(kNetworkLotOccupantFlagMask) &&
                !occupant->HasNetworkFlag(kNetworkLotRailFlag)) {
                if (standard->GetData1() == 0) {
                    DoNetworkOccupantRemoved(occupant);
                } else {
                    float position[3] = {0.0f, 0.0f, 0.0f};
                    occupant->AsOccupant()->GetPosition(position);
                    int cellX = 0, cellZ = 0;
                    Host().CityPositionToCell(position[0], position[2], cellX, cellZ);
                    UpdateRoadsInCell(cellX, cellZ);
                }
            }
            occupant->Release();
            break;
        }
        case 0x99ef1143: {   // occupant removed
            auto* unknown = static_cast<cIGZUnknown*>(standard->GetVoid1());   // slot 0x50
            if (!unknown) break;
            INetworkLotNetworkOccupant* occupant = Host().NetworkOccupantOf(unknown);   // QI 0xa821ef94
            if (!occupant) break;
            if (occupant->HasAnyNetworkFlag(kNetworkLotOccupantFlagMask) &&
                !occupant->HasNetworkFlag(kNetworkLotRailFlag)) {
                DoNetworkOccupantRemoved(occupant);
            }
            occupant->Release();
            break;
        }
        case 0xa9b6c2d6: {   // re-create a lot's objects
            auto* lot = static_cast<cSC4NetworkLot*>(standard->GetVoid1());   // slot 0x50
            if (lot) CreateLotObjects(lot);
            break;
        }
        case 0x07699455: {   // a rectangle of cells changed
            auto* rect = reinterpret_cast<const std::int32_t*>(
                static_cast<std::uintptr_t>(standard->GetData2()));   // slot 0x70
            if (!rect) break;
            const std::int32_t x0 = rect[0] - 1, z0 = rect[1] - 1, x1 = rect[2] + 1, z1 = rect[3] + 1;
            const std::int32_t maxX = x1 >= x0 ? x1 : x0;
            const std::int32_t maxZ = z1 >= z0 ? z1 : z0;
            const int cellsX = Host().CityCellCountX();
            const int cellsZ = Host().CityCellCountZ();
            for (std::int32_t z = z0; z <= maxZ; ++z) {
                for (std::int32_t x = x0; x <= maxX; ++x) {
                    if (z < cellsZ && x < cellsX && (x | z) >= 0) {
                        InsertUpdateCell(NetworkLotCellKey(x, z));
                    }
                }
            }
            ProcessUpdateList();
            break;
        }
        case 0xe8bda2e7: {   // lot state changed: mark the border cells around the lot
            cIGZUnknown* unknown = standard->GetIGZUnknown();   // slot 0x100
            if (!unknown) break;
            INetworkLot* lot = Host().LotOf(unknown);   // QI 0xe818fced
            if (!lot) break;
            std::int32_t rect[4] = {0, 0, 0, 0};
            lot->GetBoundingRect(rect);
            rect[0] -= 1;
            rect[1] -= 1;
            rect[2] += 1;
            rect[3] += 1;
            const std::int32_t minX = rect[0];
            const std::int32_t maxX = rect[2] >= rect[0] ? rect[2] : rect[0];
            const std::int32_t maxZ = rect[3] >= rect[1] ? rect[3] : rect[1];
            const int cellsX = Host().CityCellCountX();
            const int cellsZ = Host().CityCellCountZ();
            const std::int32_t width = maxX - minX;
            // Quirk kept: the original's loop starts at (0,0) and only reaches the rectangle on its
            // second pass, then steps x by `width` on the middle rows (the border ring) and by 1 on the
            // last row. The first row therefore begins at x = 1 and cell (0,0) is always marked.
            std::int32_t x = 0, z = 0;
            std::int32_t step = 1;
            bool started = false;
            for (;;) {
                if (!started) {
                    started = true;
                } else {
                    x += step;
                    if (x > maxX) {
                        if (maxZ <= z) break;
                        ++z;
                        step = width > 0 ? width : 1;
                        if (maxZ <= z) step = 1;
                        x = minX;
                    }
                }
                if (z < cellsZ && x < cellsX && (x | z) >= 0) InsertUpdateCell(NetworkLotCellKey(x, z));
            }
            ProcessUpdateList();
            lot->Release();
            break;
        }
        case 0x69247dc8: {   // pothole changed
            const int x = static_cast<int>(standard->GetData1());   // slot 0x48
            const int z = static_cast<int>(standard->GetData2());   // slot 0x70
            const bool damaged = standard->GetData3() != 0;          // slot 0x98
            DoRoadDamageChange(x, z, damaged);
            break;
        }
        default:
            break;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ cells

// @ 0x100406d10. Refreshes the road piece in a cell: restyle it and, unless it is a forced-config
// piece, re-evaluate the network lot there.
void cSC4NetworkLotManager::UpdateRoadsInCell(int x, int z) {
    INetworkLotNetworkOccupant* occupant = Host().FindNetworkOccupant(x, z, kNetworkLotOccupantFlagMask, true);
    if (!occupant) return;
    if (occupant->HasAnyNetworkFlag(kNetworkLotBusyFlagMask)) return;
    UpdateRoadStyle(x, z, occupant);
    if (occupant->HasNetworkFlag(kNetworkLotAvenueFlag)) return;
    const std::uint32_t zoneType = GetZoneTypeForNetwork(x, z);
    const std::uint32_t wealth = GetWealthForNetwork(x, z);
    UpdateLot(occupant, x, z, zoneType, wealth);
}

// @ 0x100406de6. A network occupant was removed: drop the street props and flora of its cell and release
// the lot in the registry.
void cSC4NetworkLotManager::DoNetworkOccupantRemoved(INetworkLotNetworkOccupant* occupant) {
    if (!occupant) return;
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->AsOccupant()->GetPosition(position);
    int cellX = 0, cellZ = 0;
    Host().CityPositionToCell(position[0], position[2], cellX, cellZ);
    Host().RemoveStreetPropsFromTile(cellX, cellZ);
    const std::int32_t rect[4] = {cellX, cellZ, cellX, cellZ};
    Host().DemolishFloraInCellArea(rect, 0);
    const std::uint16_t key = NetworkLotCellKey(cellX, cellZ);
    auto it = networkLots_.find(key);
    if (it != networkLots_.end()) {
        it->second->Release();
        networkLots_.erase(it);
    }
}

// @ 0x100406ee8. Creates the prop and flora objects the lot's configuration asks for, then tells the
// traffic map that the occupant's intersection changed.
void cSC4NetworkLotManager::CreateLotObjects(cSC4NetworkLot* lot) {
    if (!lot) return;
    std::vector<cSC4LotConfigurationObject>& objects = lot->GetLotConfigurationObjectArray();
    for (std::size_t index = 0; index < objects.size(); ++index) {
        cSC4LotConfigurationObject& object = objects[index];
        std::vector<std::uint32_t> ids;
        if (object.type == 4) {   // flora
            Host().GetObjectIDArray(&object, ids);
            if (ids.empty()) continue;
            const std::uint32_t id = ids[RandomIndex(static_cast<std::uint32_t>(ids.size()))];
            float position[3] = {0.0f, 0.0f, 0.0f};
            int rotation = 0;
            GetPositionForLotConfigurationObject(lot, &object, position, rotation);
            INetworkLotPropOccupant* flora = Host().AddFlora(id, position, true);
            if (flora) flora->SetLotTag(static_cast<std::uint32_t>(index));
        } else if (object.type == 1 && (object.flags & 1) == 0) {   // prop, not the mirrored variant
            Host().GetObjectIDArray(&object, ids);
            if (ids.empty()) continue;
            const std::uint32_t id = ids[RandomIndex(static_cast<std::uint32_t>(ids.size()))];
            CreateProp(lot, static_cast<std::uint32_t>(index), id);
        }
    }
    if (cIGZUnknown* occupant = lot->GetNetworkOccupant()) {
        Host().UpdateIntersection(static_cast<INetworkLotNetworkOccupant*>(occupant));
    }
}

// @ 0x100407106. Refreshes every marked cell (the set is consumed).
void cSC4NetworkLotManager::ProcessUpdateList() {
    while (!updateCells_.empty()) {
        const std::uint16_t key = *updateCells_.begin();
        UpdateRoadsInCell(key >> 8, key & 0xffu);
        updateCells_.erase(key);
    }
}

void cSC4NetworkLotManager::InsertUpdateCell(std::uint16_t key) { updateCells_.insert(key); }

// @ 0x100407166. Creates or removes the pothole visual effect of a cell.
void cSC4NetworkLotManager::DoRoadDamageChange(int x, int z, bool damaged) {
    const std::uint16_t key = NetworkLotCellKey(x, z);
    if (damaged) {
        if (potholes_.find(key) != potholes_.end()) return;
        INetworkLotVisualEffect* effect = Host().CreateVisualEffect();
        if (!effect) return;
        float position[3] = {0.0f, 0.0f, 0.0f};
        Host().CityCellCenterToPosition(x, z, position[0], position[2]);
        position[1] = Host().TerrainAltitude(position[0], position[2]);
        INetworkLotOccupantFilter* filter = Host().CreateNetworkOrFilter(kNetworkLotOccupantFlagMask);
        if (filter) filter->AddRef();
        INetworkLotNetworkOccupant* occupant = Host().GetFirstNetworkOccupant(x, z, filter);
        const char* name = "pothole";
        if (occupant) {
            position[1] = occupant->GetAltitude(position[0], position[2]);
            // The original indexes the table with the raw active-edge byte; masks are four bits.
            const unsigned edge = static_cast<unsigned>(occupant->GetActiveEdges()) & 0xfu;
            position[0] += static_cast<float>(kNetworkLotActiveEdgeOffset[edge][0]) * kNetworkLotQuarterCell;
            position[2] += static_cast<float>(kNetworkLotActiveEdgeOffset[edge][1]) * kNetworkLotQuarterCell;
            const std::uint32_t flags = occupant->GetNetworkFlag();
            if ((flags & 0x1fffu) == kNetworkLotPotholeRailFlag) name = "pothole_rail";
        }
        effect->SetEffectType(name);
        effect->AsOccupant()->SetPosition(position);
        Host().InsertOccupant(effect->AsOccupant());
        effect->StartEffect(true);
        potholes_[key] = effect;
        if (filter) filter->Release();
        if (occupant) occupant->Release();
    } else {
        auto it = potholes_.find(key);
        if (it == potholes_.end()) return;
        INetworkLotVisualEffect* effect = it->second;
        potholes_.erase(it);
        effect->StopEffect(true);
        Host().RemoveOccupant(effect->AsOccupant(), true, 0);
        effect->Release();
    }
}

// @ 0x1004074f4. Applies the network style (under-texture and variation) of the piece in a cell; avenue
// pieces take the whole-run style pass instead.
void cSC4NetworkLotManager::UpdateRoadStyle(int x, int z, INetworkLotNetworkOccupant* occupant) {
    if (!occupant) return;
    if (occupant->HasAnyNetworkFlag(kNetworkLotNoStyleFlagMask)) return;
    if (!occupant->IsUsable()) return;
    if (occupant->HasNetworkFlag(kNetworkLotAvenueFlag)) {
        UpdateAvenueStyle(x, z);
        return;
    }
    const std::uint32_t zoneType = GetZoneTypeForNetwork(x, z);
    const std::uint32_t wealth = GetWealthForNetwork(x, z);
    const unsigned zone = static_cast<unsigned>(zoneType) & 0xfu;
    const unsigned wealthIndex = static_cast<unsigned>(wealth) & 0x3u;
    std::uint32_t texture;
    if (!occupant->HasAnyNetworkFlag(kNetworkLotStyleTwoFlag) ||
        occupant->HasAnyNetworkFlag(kNetworkLotStyleDRoadFlag)) {
        texture = kNetworkLotUnderTextureId[zone * 4 + wealthIndex];
    } else {
        texture = 0x8400000u;
        if (zoneType == 0) texture = 0;
    }
    occupant->SetUnderTexture(texture);
    if (texture != 0) occupant->SetNetworkFlag(kNetworkLotUnderTextureFlag);
    const unsigned variationRow = static_cast<unsigned>(wealthIndex > 0 ? wealthIndex - 1 : 0);
    occupant->SetVariation(kNetworkLotVariation[zone * 3 + variationRow]);
}

// @ 0x100407640. Keeps the lot in a cell in sync with the piece that is there now.
void cSC4NetworkLotManager::UpdateLot(INetworkLotNetworkOccupant* occupant, int x, int z,
                                      std::uint32_t zoneType, std::uint32_t wealth) {
    if (!occupant) return;
    const std::uint16_t key = NetworkLotCellKey(x, z);
    auto it = networkLots_.find(key);
    if (it == networkLots_.end()) {
        if (occupant->IsUsable()) DoNetworkOccupantInserted(occupant, zoneType, wealth);
        return;
    }
    cSC4NetworkLot* lot = it->second;
    if (lot->UsesNextPieceConfiguration()) return;   // a forced configuration is never re-evaluated
    if (cSC4NetworkLotConfiguration* configuration = lot->GetConfiguration()) {
        if (IsLotCompatible(configuration, x, z, zoneType, wealth, occupant->GetRotation(),
                            occupant->GetFlip())) {
            return;
        }
    }
    auto* previous = static_cast<INetworkLotNetworkOccupant*>(lot->GetNetworkOccupant());
    if (!previous) return;
    previous->AddRef();
    DoNetworkOccupantRemoved(previous);
    DoNetworkOccupantInserted(previous, zoneType, wealth);
    previous->Release();
}

// @ 0x100407750. The tile/zone/wealth/rotation/flip/slope predicate that picks a configuration.
bool cSC4NetworkLotManager::IsLotCompatible(cSC4NetworkLotConfiguration* configuration, int x, int z,
                                            std::uint32_t zoneType, std::uint32_t wealth,
                                            std::uint8_t rotation, bool flipped) {
    if (!configuration) return false;
    const std::size_t side = configuration->Field58();
    if (side == 0) return false;   // the original divides by this byte
    const unsigned row = static_cast<unsigned>(z) % side;
    const unsigned column = static_cast<unsigned>(x) % side;
    if (((configuration->NeighborhoodBytes()[row] >> (column & 0x1fu)) & 1u) == 0) return false;
    const std::vector<bool>& zoneTypes = configuration->ZoneTypeBits();
    if (zoneType >= zoneTypes.size() || !zoneTypes[zoneType]) return false;
    const std::vector<bool>& wealthTypes = configuration->WealthTypeBits();
    if (wealth >= wealthTypes.size() || !wealthTypes[wealth]) return false;
    if (((configuration->RotationMask() >> (rotation & 0x1fu)) & 1u) == 0) return false;
    const std::uint8_t flipMode = configuration->FlipMode();
    if (flipMode == 1 && !flipped) return false;
    if (flipMode == 2 && flipped) return false;
    const std::int32_t rect[4] = {x, z, x, z};
    const float slope = Host().GetAreaSlope(rect, true);
    return configuration->MinimumSlope() <= slope && slope <= configuration->MaximumSlope();
}

// @ 0x10040784e. Creates the network lot for a newly placed piece (picking a compatible configuration
// when none is forced) and fills in its prop/flora objects.
void cSC4NetworkLotManager::DoNetworkOccupantInserted(INetworkLotNetworkOccupant* occupant,
                                                      std::uint32_t zoneType, std::uint32_t wealth) {
    if (!occupant) return;
    const std::uint32_t pieceId = occupant->PieceId();
    auto it = configurations_.find(pieceId);
    if (it == configurations_.end() && !nextPieceConfiguration_) return;
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->AsOccupant()->GetPosition(position);
    int cellX = 0, cellZ = 0;
    Host().CityPositionToCell(position[0], position[2], cellX, cellZ);
    cSC4NetworkLotConfiguration* chosen = nextPieceConfiguration_;
    if (!chosen) {
        std::vector<cSC4NetworkLotConfiguration*> candidates;
        candidates.reserve(it->second.size());
        for (cSC4NetworkLotConfiguration* configuration : it->second) {
            if (IsLotCompatible(configuration, cellX, cellZ, zoneType, wealth, occupant->GetRotation(),
                                occupant->GetFlip())) {
                candidates.push_back(configuration);
            }
        }
        if (candidates.empty()) return;
        chosen = candidates[RandomIndex(static_cast<std::uint32_t>(candidates.size()))];
    }
    auto* lot = new cSC4NetworkLot();
    lot->AddRef();
    lot->SetConfiguration(chosen);
    lot->SetNetworkOccupant(occupant);
    networkLots_[NetworkLotCellKey(cellX, cellZ)] = lot;
    CreateLotObjects(lot);
    if (nextPieceConfiguration_) {
        lot->SetUsesNextPieceConfiguration(true);
        nextPieceConfiguration_->Release();
        nextPieceConfiguration_ = nullptr;
    }
}

// @ 0x100407c02. Places one configuration object inside its lot: the object's (x, z) are rotated by the
// occupant's rotation and mirrored when it is flipped, and the altitude comes from the occupant.
void cSC4NetworkLotManager::GetPositionForLotConfigurationObject(cSC4NetworkLot* lot,
                                                               cSC4LotConfigurationObject* object,
                                                               float position[3], int& rotation) {
    if (!lot || !object) return;
    auto* occupant = static_cast<INetworkLotNetworkOccupant*>(lot->GetNetworkOccupant());
    if (!occupant) return;
    std::uint32_t cellX = 0, cellZ = 0;
    occupant->GetOccupiedCell(cellX, cellZ);
    const float baseX = static_cast<float>(cellX << 4);
    const float baseZ = static_cast<float>(cellZ << 4);
    const std::uint8_t occupantRotation = occupant->GetRotation();
    const float objectY = object->y;
    float x = 0.0f, z = 0.0f;
    if (occupantRotation > 3) {
        x = 0.0f;
        z = 0.0f;
    } else {
        switch (occupantRotation) {
            case 0:
                x = (baseX + 16.0f) - object->x;
                z = (baseZ + 16.0f) - object->z;
                break;
            case 1:
                x = object->z + baseX;
                z = (baseZ + 16.0f) - object->x;
                break;
            case 2:
                x = object->x + baseX;
                z = baseZ + object->z;
                break;
            default:   // case 3
                x = (baseX + 16.0f) - object->z;
                z = baseZ + object->x;
                break;
        }
    }
    position[0] = x;
    position[2] = z;
    position[1] = objectY + occupant->GetAltitude(x, z);
    if (!occupant->GetFlip()) {
        rotation = static_cast<int>((static_cast<std::uint32_t>(object->orientation) + occupantRotation) & 3u);
    } else {
        position[0] = (16.0f - 2.0f * (position[0] - baseX)) + position[0];
        rotation = kNetworkLotFlippedRotation[(object->orientation & 3u) * 4 + (occupantRotation & 3u)];
    }
}

// @ 0x100407df4. The network tools' redevelop entry point: clear the cell's street props and flora and
// rebuild the lot's objects.
void cSC4NetworkLotManager::RedevelopLot(std::uint32_t x, std::uint32_t z, cSC4NetworkLot* lot) {
    Host().RemoveStreetPropsFromTile(static_cast<int>(x & 0xffu), static_cast<int>(z & 0xffu));
    const std::int32_t rect[4] = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(z),
                                  static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)};
    Host().DemolishFloraInCellArea(rect, 0);
    CreateLotObjects(lot);
}

// @ 0x100407e66. Creates one street prop of the lot at the object's position (returning the created
// occupant; the recovered cSC4PropManager API drops that pointer, hence the seam).
INetworkLotPropOccupant* cSC4NetworkLotManager::CreateProp(cSC4NetworkLot* lot, std::uint32_t objectIndex,
                                                           std::uint32_t propType) {
    if (!lot || propType == 0) return nullptr;
    std::vector<cSC4LotConfigurationObject>& objects = lot->GetLotConfigurationObjectArray();
    if (objectIndex >= objects.size()) return nullptr;
    float position[3] = {0.0f, 0.0f, 0.0f};
    int rotation = 0;
    GetPositionForLotConfigurationObject(lot, &objects[objectIndex], position, rotation);
    INetworkLotPropOccupant* prop = Host().AddStreetProp(propType, position, rotation);
    if (!prop) return nullptr;
    prop->SetLotTag(objects[objectIndex].id);
    auto* occupant = static_cast<INetworkLotNetworkOccupant*>(lot->GetNetworkOccupant());
    if (occupant && occupant->GetFlip()) prop->SetFlag(0x80);
    return prop;
}

// @ 0x100407f2c.
cSC4NetworkLot* cSC4NetworkLotManager::GetNetworkLot(int x, int z) const {
    auto it = networkLots_.find(NetworkLotCellKey(x, z));
    return it == networkLots_.end() ? nullptr : it->second;
}

// @ 0x100407f60. True when a configuration of any piece has this id (the original's second argument is
// really the id; the decompiled "cISC4NetworkLotConfiguration&" is a mis-identified register).
bool cSC4NetworkLotManager::GetNetworkLotConfigurationById(std::uint32_t id) const {
    for (const auto& bucket : configurations_) {
        for (cSC4NetworkLotConfiguration* configuration : bucket.second) {
            if (configuration->GetID() == id) return true;
        }
    }
    return false;
}

// @ 0x100407fbe.
void cSC4NetworkLotManager::SetLotConfigurationForNextPiece(cSC4NetworkLotConfiguration* configuration) {
    if (nextPieceConfiguration_ == configuration) return;
    if (configuration) configuration->AddRef();
    cSC4NetworkLotConfiguration* previous = nextPieceConfiguration_;
    nextPieceConfiguration_ = configuration;
    if (previous) previous->Release();
}

// @ 0x10040800c. The zone type of a cell, or when the cell itself is unzoned the best (highest-priority)
// zone among its neighbours. The diagonals are only consulted when the best zone is 0, 7 or 14.
std::uint32_t cSC4NetworkLotManager::GetZoneTypeForNetwork(int x, int z) {
    const int zone = Host().ZoneTypeAt(x, z);
    if (zone != 0) return static_cast<std::uint32_t>(zone);
    const int cellsX = Host().CityCellCountX();
    const int cellsZ = Host().CityCellCountZ();
    int best = 0;
    bool diagonals = false;
    for (int i = 0; i < 8; ++i) {
        if (i >= 4 && !diagonals) break;
        const int nx = kNetworkLotDeltaX[i] + x;
        const int nz = kNetworkLotDeltaZ[i] + z;
        if (nx >= 0 && nz >= 0 && nx < cellsX && nz < cellsZ) {
            const int neighbour = Host().ZoneTypeAt(nx, nz);
            if (kNetworkLotZonePriority[neighbour & 0xf] < kNetworkLotZonePriority[best & 0xf]) {
                best = neighbour;
            }
        }
        // The diagonal rule is evaluated at i == 3 on every path (an out-of-bounds edge does not skip it).
        if (i == 3 && best <= 0xe && ((0x4081 >> best) & 1) != 0) diagonals = true;
    }
    return static_cast<std::uint32_t>(best);
}

// @ 0x100408130. The wealth of the neighbouring lots (the mode of their occupant/building wealth). The
// diagonals are only consulted when no orthogonal neighbour has a lot.
std::uint32_t cSC4NetworkLotManager::GetWealthForNetwork(int x, int z) {
    const int cellsX = Host().CityCellCountX();
    const int cellsZ = Host().CityCellCountZ();
    int counts[4] = {0, 0, 0, 0};
    bool diagonals = true;
    for (int i = 0; i < 8; ++i) {
        if (i >= 4 && !diagonals) break;
        const int nx = kNetworkLotDeltaX[i] + x;
        if (nx < 0) continue;
        const int nz = kNetworkLotDeltaZ[i] + z;
        if (nz >= cellsZ) continue;
        if (nx >= cellsX) continue;
        if (nz < 0) continue;
        INetworkLot* lot = Host().LotAt(nx, nz, false);
        if (!lot) continue;
        std::uint32_t wealth = lot->GetOccupantWealth();
        if (wealth == 0 && lot->IsInLivableState()) {
            const std::uint32_t buildingType = lot->GetBuildingType(false);
            wealth = buildingType ? Host().GetBuildingWealth(buildingType) : 0;
        }
        if (wealth > 3) wealth = 0;
        ++counts[wealth];
        if (i < 4) diagonals = false;
    }
    // Mac i386 0x0030c880: a wealth wins when its count is not below the best so far, which starts at the lots without a
    // wealth (count[0]); ties go to the higher wealth.
    int best = 0;
    for (int i = 1; i < 4; ++i) {
        if (counts[i] != 0 && counts[best] <= counts[i]) best = i;
    }
    return static_cast<std::uint32_t>(best);
}

// @ 0x100408330. The avenue style pass: walk the connected run of avenue pieces whose IsIntersection
// flag equals the start piece's, tally their zone/wealth, then restyle and re-evaluate every piece with
// the run's dominant zone and wealth. Pieces with a different flag (or no piece at all) only block the
// walk; every visited cell is also removed from the pending road-refresh set.
void cSC4NetworkLotManager::UpdateAvenueStyle(int x, int z) {
    INetworkLotNetworkOccupant* root = Host().FindNetworkOccupant(x, z, kNetworkLotAvenueFlag, true);
    if (!root) return;
    const bool rootIsIntersection = root->IsIntersection();
    const int cellsX = Host().CityCellCountX();
    const int cellsZ = Host().CityCellCountZ();
    std::unordered_set<std::uint16_t> queue;
    std::unordered_set<std::uint16_t> avenue;
    std::unordered_set<std::uint16_t> blocked;
    std::vector<int> zoneTally(16, 0);
    std::vector<int> wealthTally(4, 0);
    queue.insert(NetworkLotCellKey(x, z));
    while (!queue.empty()) {
        const std::uint16_t key = *queue.begin();
        queue.erase(queue.begin());
        const int cellX = key >> 8;
        const int cellZ = key & 0xffu;
        INetworkLotNetworkOccupant* occupant =
            Host().FindNetworkOccupant(cellX, cellZ, kNetworkLotAvenueFlag, true);
        if (!occupant || occupant->IsIntersection() != rootIsIntersection) {
            blocked.insert(key);   // the original records it and does not expand from here
            continue;
        }
        if (!avenue.insert(key).second) continue;
        ++zoneTally[GetZoneTypeForNetwork(cellX, cellZ) & 0xfu];
        ++wealthTally[GetWealthForNetwork(cellX, cellZ) & 0x3u];
        std::uint32_t edges = occupant->GetEdgeTypesFlag(kNetworkAvenueNetworkType);
        for (int i = 0; i < 4; ++i) {
            if ((edges & 7u) != 0) {
                const int nz = kNetworkLotDeltaZ[i] + cellZ;
                const int nx = kNetworkLotDeltaX[i] + cellX;
                if (nz <= cellsZ && nx <= cellsX && nx >= 0 && nz >= 0) {
                    const std::uint16_t neighbour = NetworkLotCellKey(nx, nz);
                    if (blocked.count(neighbour) == 0 && avenue.count(neighbour) == 0) {
                        queue.insert(neighbour);
                    }
                }
            }
            edges >>= 8;
        }
    }
    if (avenue.empty()) return;
    int bestZone = 0;
    int maxZone = 0;
    for (int i = 1; i < 16; ++i) {
        const int count = zoneTally[i];
        if (count > maxZone) bestZone = i;
        if (count >= maxZone) maxZone = count;
    }
    int bestWealth = 0;
    int maxWealth = 0;
    for (int i = 1; i < 4; ++i) {
        const int count = wealthTally[i];
        if (count > maxWealth) bestWealth = i;
        if (count >= maxWealth) maxWealth = count;
    }
    const std::uint32_t underTexture = kNetworkLotUnderTextureId[bestZone * 4 + bestWealth];
    const unsigned variationRow = static_cast<unsigned>(bestWealth > 0 ? bestWealth - 1 : 0);
    const std::uint8_t variation = kNetworkLotVariation[bestZone * 3 + variationRow];
    for (std::uint16_t key : avenue) {
        const int cellX = key >> 8;
        const int cellZ = key & 0xffu;
        INetworkLotNetworkOccupant* occupant =
            Host().FindNetworkOccupant(cellX, cellZ, kNetworkLotAvenueFlag, true);
        if (occupant && occupant->IsUsable()) {
            occupant->SetUnderTexture(underTexture);
            occupant->SetVariation(variation);
            UpdateLot(occupant, cellX, cellZ, static_cast<std::uint32_t>(bestZone),
                      static_cast<std::uint32_t>(bestWealth));
        }
        updateCells_.erase(key);
    }
}

}  // namespace sc4

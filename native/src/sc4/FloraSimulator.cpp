#include "sc4/FloraSimulator.h"
#include "rz/cRZRandom.h"
#include "sc4/SimStreamOps.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sc4/NetworkOccupant.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IFloraSimulatorHost* g_host = nullptr;
bool g_staticInitialized = false;                                           // mbStaticInitialized
std::map<std::uint32_t, cSC4StaticFloraData*> g_staticFloraDataMap;         // mStaticFloraDataMap
std::vector<std::uint32_t> g_floraTypeArray;                                // mFloraTypeArray (insertion order)
std::map<std::uint32_t, std::vector<std::uint32_t>> g_floraFamilyMap;       // mFloraFamilyMap
}  // namespace

IFloraSimulatorHost* FloraSimulatorHost() { return g_host; }
void SetFloraSimulatorHost(IFloraSimulatorHost* host) { g_host = host; }

// 0x1000c2252: the original answers +0 for iid 1 / 0x06c66177, +8 for the message target and +0x10 for
// the serializable. The port keeps a single cIGZSerializable base, so every accepted id resolves to `this`;
// the serializable id is what the city-load serializer (SC4ReadGZSerializable) asks for.
bool cSC4FloraSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 1 || iid == kFloraSimulatorInterfaceId || iid == kFloraSimulatorClassId)
        *out = static_cast<cIGZUnknown*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x1000c219a: below 2 destroys the object.
std::uint32_t cSC4FloraSimulator::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

// 0x1000be5a0. The original serialises version 2 then, in order:
//   +0x50..0x53 four bools, +0x54/+0x58 two floats, +0x5c/+0x60 two u32, +0x64/+0x68/+0x6c/+0x70 four
//   s32, +0x74 one u8, +0x78 the RandomIterator, +0xb8 the cRZRandom, +0xd8 the ilist<cISC4Occupant*>
//   (the obliterated-flora list), +0xf0/+0xf4 two floats. None of those instance fields are recovered in
//   this port (the seeding / daily-simulation state lives behind the IFloraSimulatorHost seam), so only the
//   version is written. PARTIAL: the field list above is the confirmed binary layout, not a guessed body.
bool cSC4FloraSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    return !stream.GetError();
}

// 0x1000be6d8. See Write: the port has none of the version-2 payload fields, so it accepts the version and
// stops. PARTIAL (confirmed field list above).
bool cSC4FloraSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    void* dbStream = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) || !dbStream) return !stream.GetError();   // the port's own reduced record
    static_cast<cIGZUnknown*>(dbStream)->Release();
    // The record of a city file: four flags, the growth parameters, the walk iterator, the seed generator and the occupants of the
    // flora in progress. The port's flora simulator keeps none of this state yet, so it is consumed.
    std::uint8_t u8 = 0;
    std::uint32_t u32 = 0;
    std::int32_t s32 = 0;
    float f32 = 0.0f;
    for (int i = 0; i < 4; ++i) stream.GetUint8(u8);
    stream.GetFloat32(f32); stream.GetFloat32(f32);
    stream.GetSint32(s32); stream.GetSint32(s32);
    for (int i = 0; i < 4; ++i) stream.GetUint32(u32);
    stream.GetUint8(u8);
    RandomIterator walk;
    stream >> walk;                                                    // +0x78
    cRZRandom random(1);
    random.Read(stream);                                               // +0xb8
    stream.GetUint32(u32);                                             // +0xd8: a list of occupant references
    if (u32 < 10000000u) {
        for (std::uint32_t i = 0; i < u32 && !stream.GetError(); ++i) {
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, 0x26bb767e, &raw, false);
            if (raw) static_cast<cIGZUnknown*>(raw)->Release();
        }
    }
    stream.GetFloat32(f32); stream.GetFloat32(f32);
    return !stream.GetError();
}

void cSC4FloraSimulator::MakeDefaultStaticFloraData(cSC4StaticFloraData& data) {
    static const std::uint32_t kDefaultTraits[14] = {
        0x44368000, 0x44368000, 0x43b68000, 0x40000000, 0x43000000, 0x44b68000, 0x44c30000,
        0x3f000000, 0x3e99999a, 0x3f000000, 0x3f800000, 0x40800000, 0x40400000, 0x40400000};
    data.floraType = 0;
    data.familyKey = 0;
    data.groupType = 0;
    data.aquatic = false;
    std::memcpy(data.traits, kDefaultTraits, sizeof data.traits);
    data.value48 = 0;
    data.value4c = 0;
    // 0x99 inside the 8x8 block of elevation indices 5..12 x terrain classes 5..12, zero elsewhere
    for (unsigned elevation = 0; elevation < 16; ++elevation)
        for (unsigned terrainClass = 0; terrainClass < 16; ++terrainClass)
            data.suitability[elevation][terrainClass] =
                ((terrainClass - 5u) | (elevation - 5u)) < 8u ? 0x99 : 0x00;
    data.value150 = 0;
}

bool cSC4FloraSimulator::StaticInit() {
    if (!g_staticInitialized) {
        g_staticInitialized = true;
        LoadAllStandardFloraTypes();
    }
    return true;
}

bool cSC4FloraSimulator::StaticShutdown() {
    if (g_staticInitialized) {
        g_staticInitialized = false;
        for (auto& entry : g_staticFloraDataMap) entry.second->Release();
        g_staticFloraDataMap.clear();
        g_floraTypeArray.clear();          // the family map is left in place, as in the original
    }
    return true;
}

void cSC4FloraSimulator::LoadAllStandardFloraTypes() {
    if (g_host) {
        g_host->EnumerateFloraResources(
            [](const cGZPersistResourceKey& key, void*) { AddStaticFloraDataFromResource(key); }, nullptr);
        PostFloraTypesUpdatedMessage();
    }
}

void cSC4FloraSimulator::PostFloraTypesUpdatedMessage() {
    if (g_host) g_host->PostFloraTypesUpdated();
}

void cSC4FloraSimulator::AddStaticFloraDataFromResource(const cGZPersistResourceKey& key) {
    if (!g_host) return;
    cISCResExemplar* exemplar = g_host->LoadExemplar(key);
    if (!exemplar) return;
    std::uint32_t exemplarType = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0x10, exemplarType) &&
        exemplarType == kFloraExemplarTypeValue) {
        AddStaticFloraData(key, exemplar);
    }
    exemplar->Release();
}

bool cSC4FloraSimulator::AddStaticFloraData(cSC4StaticFloraData* data) {
    const std::uint32_t type = data->floraType;
    if (g_staticFloraDataMap.count(type)) return false;
    g_staticFloraDataMap[type] = data;
    g_floraTypeArray.push_back(type);
    data->AddRef();
    for (std::uint32_t family : data->families) g_floraFamilyMap[family].push_back(type);
    // Group resolution: a type whose family key names this type is its own group; every type that grouped under
    // this type's family key is regrouped under this type.
    data->groupType = type;
    for (auto& entry : g_staticFloraDataMap) {
        cSC4StaticFloraData* other = entry.second;
        if (other->familyKey == type) data->groupType = type;
        if (other->groupType == data->familyKey) other->groupType = type;
    }
    return true;
}

bool cSC4FloraSimulator::AddStaticFloraData(const cGZPersistResourceKey& key, cISCResExemplar* exemplar) {
    if (!exemplar) {
        if (!g_host) return false;
        exemplar = g_host->LoadExemplar(key);
        if (!exemplar) return false;
    } else {
        exemplar->AddRef();
    }
    cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
    auto* data = new cSC4StaticFloraData();
    data->AddRef();
    MakeDefaultStaticFloraData(*data);
    exemplar->AddRef();
    data->exemplar = exemplar;

    cSCPropertyHelp::GetPropertyValue(holder, kFloraPropTypeId, data->floraType);
    std::vector<float> floats;
    if (holder->HasProperty(kFloraPropTraits) && cSCPropertyHelp::GetPropertyValue(holder, kFloraPropTraits, floats) &&
        floats.size() >= 14) {
        for (int i = 0; i < 14; ++i) data->traits[i] = floats[i];
    }
    if (holder->HasProperty(kFloraPropAquatic)) data->aquatic = true;
    cSCPropertyHelp::GetPropertyValue(holder, kFloraPropFamilyKey, data->familyKey);
    cSCPropertyHelp::GetPropertyValue(holder, kFloraPropValue48, data->value48);
    cSCPropertyHelp::GetPropertyValue(holder, kFloraPropValue4c, data->value4c);
    floats.clear();
    if (holder->HasProperty(kFloraPropSuitability) &&
        cSCPropertyHelp::GetPropertyValue(holder, kFloraPropSuitability, floats) && floats.size() >= 256) {
        for (int e = 0; e < 16; ++e)
            for (int c = 0; c < 16; ++c)
                data->suitability[e][c] = static_cast<std::uint8_t>(static_cast<std::int64_t>(floats[e * 16 + c] * 255.0));
    }
    cSCPropertyHelp::GetPropertyValue(holder, kFloraPropValue150, data->value150);
    data->flags = static_cast<std::uint8_t>((data->flags & 0xf8) | 6);
    bool flag = false;
    if (cSCPropertyHelp::GetPropertyValue(holder, kFloraPropFlagExcluded, flag))
        data->flags = static_cast<std::uint8_t>((data->flags & 0xfe) | (flag ? 1 : 0));
    if (cSCPropertyHelp::GetPropertyValue(holder, kFloraPropFlagBit, flag))
        data->flags &= flag ? 0xfb : 0xfd;
    cSCPropertyHelp::GetPropertyValue(holder, kFloraPropFamilies, data->families);
    data->cost = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kFloraPropCost, data->cost);

    const bool added = AddStaticFloraData(data);
    data->Release();
    exemplar->Release();
    return added;
}

int cSC4FloraSimulator::GetElevationIndex(float x, float z, bool belowSeaLevel) const {
    const float sea = g_host ? g_host->SeaLevel() : 0.0f;
    const float height = g_host ? g_host->TerrainHeight(x, z) : 0.0f;
    const float scaled = (belowSeaLevel ? sea - height : height - sea) * elevationScale_;
    const long index = std::lrintf(scaled);                                 // round half to even
    if (index <= 0) return 0;
    return index < 16 ? static_cast<int>(index) : 15;
}

float cSC4FloraSimulator::GetPropagationScoreForFlora(std::uint32_t type, float x, float z) const {
    const cSC4StaticFloraData* data = GetStaticFloraData(type);
    if (!data || !g_host) return 0.0f;
    const int terrainClass = g_host->TerrainClass(x, z);
    const int elevation = GetElevationIndex(x, z, data->aquatic);
    return float(data->suitability[elevation][terrainClass & 15]) * 0.003921569f;
}

std::uint32_t cSC4FloraSimulator::GetBestFloraTypeForLocation(float x, float z, float& score) const {
    const int terrainClass = g_host ? g_host->TerrainClass(x, z) : 0;
    const int aboveIndex = GetElevationIndex(x, z, false);
    const int belowIndex = GetElevationIndex(x, z, true);
    std::uint32_t best = 0;
    float bestScore = 0.0f;
    for (auto& entry : g_staticFloraDataMap) {
        const cSC4StaticFloraData* data = entry.second;
        if (data->flags & 1) continue;
        const int index = data->aquatic ? belowIndex : aboveIndex;
        const float value = float(data->suitability[index][terrainClass & 15]) * 0.003921569f;
        if (value > bestScore) {
            bestScore = value;
            best = entry.first;
        }
    }
    score = bestScore;
    return best;
}

cSC4StaticFloraData* cSC4FloraSimulator::GetStaticFloraData(std::uint32_t type) const {
    auto it = g_staticFloraDataMap.find(type);
    return it == g_staticFloraDataMap.end() ? nullptr : it->second;
}

std::uint32_t cSC4FloraSimulator::GetAllFloraTypes(std::uint32_t* out, std::uint32_t& count) const {
    const std::uint32_t total = static_cast<std::uint32_t>(g_floraTypeArray.size());
    if (!out) {
        count = total;
        return total;
    }
    if (total < count) count = total;
    for (std::uint32_t i = 0; i < count; ++i) out[i] = g_floraTypeArray[i];
    return count;
}

void cSC4FloraSimulator::GetAllFloraTypes(std::vector<std::uint32_t>& out) const {
    out.assign(g_floraTypeArray.begin(), g_floraTypeArray.end());
}

void cSC4FloraSimulator::GetAllFloraFamilyTypes(std::vector<std::uint32_t>& out) const {
    out.clear();
    for (auto& entry : g_floraFamilyMap) out.push_back(entry.first);
}

bool cSC4FloraSimulator::IsFloraTypeValid(std::uint32_t type) const { return g_staticFloraDataMap.count(type) != 0; }

bool cSC4FloraSimulator::IsFloraFamilyTypeValid(std::uint32_t family) const {
    return g_floraFamilyMap.count(family) != 0 || g_staticFloraDataMap.count(family) != 0;
}

int cSC4FloraSimulator::GetFloraTypesByFilter(std::unordered_set<std::uint32_t>& out, cISC4FloraTypeFilter* filter) const {
    int accepted = 0;
    for (auto& entry : g_staticFloraDataMap) {
        if (!filter || filter->Accepts(entry.first)) {
            out.insert(entry.first);
            ++accepted;
        }
    }
    return accepted;
}

int cSC4FloraSimulator::GetFloraTypesByFamily(std::unordered_set<std::uint32_t>& out, std::uint32_t family) const {
    auto it = g_floraFamilyMap.find(family);
    if (it == g_floraFamilyMap.end()) return 0;
    for (std::uint32_t type : it->second) out.insert(type);
    return static_cast<int>(it->second.size());
}

// ==================================================================================================== planting

namespace {
// cSC4FloraBlockingOccupantFilter::IsOccupantIncluded (0x62388a in the Mac i386 build): flora, buildings (0x278128a0),
// lots (0x6a11bd2b), props (0xa823821e) and the network pieces with any 0x1f4f flag keep a new tree away.
class FloraBlockingFilter final : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override {
        switch (occupant->GetType()) {
            case kOccupantTypeFlora: case 0x278128a0: case 0x6a11bd2b: case 0xa823821e: return true;
            case 0xc772bf98: {
                auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
                return network && (network->GetNetworkFlag() & 0x1f4f) != 0;
            }
            default: return false;
        }
    }
};

void ReleaseAll(OccupantList& list) {
    for (cISC4Occupant* occupant : list) occupant->Release();
    list.clear();
}
}  // namespace

// The cluster chain of a type: its cluster-type property (+4), 0 for an unknown type.
std::uint32_t cSC4FloraSimulator::NextClusterType(std::uint32_t type) {
    auto it = g_staticFloraDataMap.find(type);
    return it == g_staticFloraDataMap.end() ? 0 : it->second->familyKey;
}

// The type two trees of a and b merge into: the one after whichever of the two comes later in the other's chain, or 0
// when neither chain reaches the other.
std::uint32_t cSC4FloraSimulator::AdvanceClusterType(std::uint32_t a, std::uint32_t b) {
    for (std::uint32_t t = a; t != 0; t = NextClusterType(t))
        if (t == b) return NextClusterType(b);
    for (std::uint32_t t = b; t != 0; t = NextClusterType(t))
        if (t == a) return NextClusterType(a);
    return 0;
}

// 0x1000c1a24 (Mac i386 0x8c68e). Not on a lot: dry land only (unless aquatic) and no steeper than the type's slope
// limit (+0x2c); then the type's box (half width +0x40, height +0x44, 16 m below the ground) must lie inside the city
// and touch no blocking occupant other than `ignore`.
bool cSC4FloraSimulator::LocationIsOKForNewFlora(std::uint32_t type, float x, float z, cISC4Occupant* ignore) const {
    const cSC4StaticFloraData* data = GetStaticFloraData(type);
    if (!data || !g_host) return false;
    bool onLot = false;
    const float ground = g_host->GroundHeight(x, z, onLot);
    if (!onLot) {
        if (ground < g_host->SeaLevel() && !data->aquatic) return false;
        if (g_host->TerrainNormalY(x, z) <= 1.0f - data->traits[7]) return false;
    }
    const float half = data->traits[12];
    const float min[3] = {x - half, ground - 16.0f, z - half};
    const float max[3] = {x + half, ground + data->traits[13], z + half};
    float sizeX = 0.0f, sizeZ = 0.0f;
    g_host->CitySizeMeters(sizeX, sizeZ);
    if (min[0] < 0.0f || min[2] < 0.0f || sizeX < max[0] || sizeZ < max[2]) return false;
    cISC4OccupantManager* occupants = g_host->OccupantManager();
    if (!occupants) return true;
    FloraBlockingFilter filter;
    OccupantList found;
    occupants->GetOccupantsByBBox(found, min, max, &filter, 0xffffffffu);
    bool ok = true;
    for (cISC4Occupant* occupant : found) {
        if (!ok || occupant == ignore) continue;
        float otherMin[3], otherMax[3];
        occupant->GetBoundingBox(otherMin, otherMax);
        const bool apart = max[0] < otherMin[0] || otherMax[0] < min[0] || max[1] < otherMin[1] || otherMax[1] < min[1] ||
                           max[2] < otherMin[2] || otherMax[2] < min[2];
        if (!apart) ok = false;
    }
    ReleaseAll(found);
    return ok;
}

bool cSC4FloraSimulator::AddNewFloraOccupant(cISC4Occupant* occupant) {
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    return occupants && occupants->InsertOccupant(occupant, 0);
}

// 0x1000c1f6c (Mac i386 0x8d51a). Clustering: the flora of the type's cluster box (+0x48 across, +0x4c up) whose chain
// meets the new type is merged one tree at a time (each merge removes that tree, moves the new one to the centre of the
// merged boxes and advances the type) for as long as a pass merges something; the result must still pass
// LocationIsOKForNewFlora. The new occupant faces one of five orientations (0..4, as the original draws them), is born
// today and sits on the ground (the sea surface for land flora in the water).
cISC4Occupant* cSC4FloraSimulator::AddNewFloraOccupant(std::uint32_t type, float* xyz, bool skipClusters) {
    if (!g_host) return nullptr;
    if (!IsFloraTypeValid(type)) {
        if (type != 0) return nullptr;
        float score = 0.0f;
        type = GetBestFloraTypeForLocation(xyz[0], xyz[2], score);
        if (type == 0) {
            if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "flora: no species suits %.0f,%.0f\n", xyz[0], xyz[2]);
            return nullptr;
        }
    }
    if (!skipClusters) {
        cISC4OccupantManager* occupants = g_host->OccupantManager();
        for (;;) {
            const cSC4StaticFloraData* data = GetStaticFloraData(type);
            if (!data) break;
            const float height = g_host->TerrainHeight(xyz[0], xyz[2]);
            const float min[3] = {xyz[0] - data->value48, height - data->value4c, xyz[2] - data->value48};
            const float max[3] = {xyz[0] + data->value48, height + data->value4c, xyz[2] + data->value48};
            OccupantList found;
            if (occupants) occupants->GetOccupantsByBBox(found, min, max, kOccupantTypeFlora, 0xffffffffu);
            xyz[0] += xyz[0];                                    // the sums of the box centres, doubled
            xyz[1] += xyz[1];
            xyz[2] += xyz[2];
            int merged = 1;
            for (cISC4Occupant* occupant : found) {
                void* raw = nullptr;
                if (!occupant->QueryInterface(kGZIID_cISC4FloraOccupant, &raw) || !raw) continue;
                auto* flora = static_cast<cISC4FloraOccupant*>(raw);
                const std::uint32_t next = AdvanceClusterType(flora->GetFloraType(), type);
                if (next != 0) {
                    float otherMin[3], otherMax[3];
                    occupant->GetBoundingBox(otherMin, otherMax);
                    const float scale = 0.5f / static_cast<float>(merged + 1);
                    const float sumX = otherMin[0] + otherMax[0] + xyz[0];
                    const float sumZ = otherMin[2] + otherMax[2] + xyz[2];
                    if (LocationIsOKForNewFlora(next, scale * sumX, scale * sumZ, occupant)) {
                        occupants->RemoveOccupant(occupant, true, 0);
                        xyz[0] = sumX;
                        xyz[2] = sumZ;
                        type = next;
                        ++merged;
                    }
                }
                flora->Release();
            }
            ReleaseAll(found);
            const float scale = 0.5f / static_cast<float>(merged);
            xyz[0] *= scale;
            xyz[1] = -1.0f;                                      // re-read from the ground below
            xyz[2] *= scale;
            if (merged < 2) break;
        }
        if (!LocationIsOKForNewFlora(type, xyz[0], xyz[2], nullptr)) {
            if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "flora: plant type %#x at %.0f,%.0f blocked\n", type, xyz[0], xyz[2]);
            return nullptr;
        }
    }

    auto* occupant = new cSC4FloraOccupant();
    occupant->AddRef();
    occupant->SetFloraType(type);
    occupant->SetOrientation(static_cast<std::uint32_t>(random_.RandomSint32RangeUniform(0, 4)));
    if (xyz[1] <= 0.0f) {
        const float sea = g_host->SeaLevel();
        const cSC4StaticFloraData* data = GetStaticFloraData(type);
        bool onLot = false;
        xyz[1] = g_host->GroundHeight(xyz[0], xyz[2], onLot);
        if (data && !data->aquatic && xyz[1] < sea) xyz[1] = sea;
    }
    cISC4Occupant* result = nullptr;
    if (0.0f <= xyz[1]) {
        if (cSC4StaticFloraData* data = GetStaticFloraData(type)) occupant->SetStaticFloraData(data);
        occupant->cSC4FloraOccupant::SetPosition(xyz);
        occupant->SetBirthDate(g_host->SimDate());
        occupant->SetLastSeedingDate(occupant->GetBirthDate());
        occupant->cSC4Occupant::Init();
        if (AddNewFloraOccupant(occupant->AsOccupant())) result = occupant->AsOccupant();
    }
    occupant->Release();
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "flora: plant type %#x at %.0f,%.0f,%.0f -> %s\n", type, xyz[0], xyz[1], xyz[2], result ? "added" : "refused");
    return result;
}

// DoMessage's 0x278e6003 branch: the rectangle grown by one cell towards the origin and clamped to the city's cells.
void cSC4FloraSimulator::DoTerrainChangedMessage(std::uint32_t data1, std::uint32_t data2) {
    if (!g_host) return;
    float sizeX = 0.0f, sizeZ = 0.0f;
    g_host->CitySizeMeters(sizeX, sizeZ);
    const auto cellsX = static_cast<std::int32_t>(sizeX / 16.0f), cellsZ = static_cast<std::int32_t>(sizeZ / 16.0f);
    std::int32_t rect[4];
    rect[0] = std::max(static_cast<std::int32_t>(data1 >> 16) - 1, 0);
    rect[1] = std::max(static_cast<std::int32_t>(data2 >> 16) - 1, 0);
    rect[2] = std::min(static_cast<std::int32_t>(data1 & 0xffff), cellsX - 1);
    rect[3] = std::min(static_cast<std::int32_t>(data2 & 0xffff), cellsZ - 1);
    DoTerrainAltitudeChange(rect);
}

// 0x1000c19c0 (Mac i386 0x8c9e2). Every flora occupant of the cells whose ground moved (by 0.001 m or more) is removed, unless
// the debug option keeps them, in which case (and for the ones whose ground did not move) the occupant follows the ground.
void cSC4FloraSimulator::DoTerrainAltitudeChange(const std::int32_t rect[4]) {
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    if (!occupants) return;
    OccupantList found;
    if (!occupants->GetOccupantsByStandardCityCells(found, rect, rect + 2, kOccupantTypeFlora, 0xffffffffu)) return;
    int removed = 0;
    for (cISC4Occupant* occupant : found) {
        void* raw = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4FloraOccupant, &raw) && raw) {
            auto* flora = static_cast<cISC4FloraOccupant*>(raw);
            float position[3];
            flora->AsOccupant()->GetPosition(position);
            bool onLot = false;
            const float ground = g_host->GroundHeight(position[0], position[2], onLot);
            if (!keepFloraOnTerrainChange_ && 0.001f <= std::fabs(ground - position[1])) {
                occupants->RemoveOccupant(flora->AsOccupant(), true, 0);
                ++removed;
            } else {
                AdjustFloraPositionFromTerrainChange(occupant, flora);
            }
            flora->Release();
        }
    }
    if (std::getenv("SC4_DEBUG") && !found.empty())
        std::fprintf(stderr, "flora: terrain changed in cells %d,%d..%d,%d: %zu flora, %d removed\n", rect[0], rect[1], rect[2], rect[3], found.size(), removed);
    ReleaseAll(found);
}

// 0x1000c18f4 (Mac i386 0x8c916): land flora sits on the ground or the sea surface, whichever is higher; aquatic flora on the
// sea surface. A move of 0.001 m or more updates the position and tells the occupant manager.
void cSC4FloraSimulator::AdjustFloraPositionFromTerrainChange(cISC4Occupant* occupant, cISC4FloraOccupant* flora) {
    if (!g_host) return;
    const float sea = g_host->SeaLevel();
    const cSC4StaticFloraData* data = GetStaticFloraData(flora->GetFloraType());
    float position[3];
    occupant->GetPosition(position);
    bool onLot = false;
    const float ground = g_host->GroundHeight(position[0], position[2], onLot);
    float target = sea;
    if (data && !data->aquatic && sea <= ground) target = ground;
    if (0.001f <= std::fabs(target - position[1])) {
        position[1] = target;
        occupant->SetPosition(position);
        if (cISC4OccupantManager* occupants = g_host->OccupantManager()) occupants->MoveOccupant(occupant, true);
    }
}

}  // namespace sc4

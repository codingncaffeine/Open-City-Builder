#include "sc4/FloraSimulator.h"
#include "rz/cRZRandom.h"
#include "sc4/SimStreamOps.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gz/CommandParameterSet.h"
#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZString.h"
#include "rz/RZConvert.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutoHelpers.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IFloraSimulatorHost* g_host = nullptr;
bool g_staticInitialized = false;                                           // mbStaticInitialized
std::map<std::uint32_t, cSC4StaticFloraData*> g_staticFloraDataMap;         // mStaticFloraDataMap
std::vector<std::uint32_t> g_floraTypeArray;                                // mFloraTypeArray (insertion order)
std::map<std::uint32_t, std::vector<std::uint32_t>> g_floraFamilyMap;       // mFloraFamilyMap
// The records an exemplar-change reload drops from the map without releasing them (flora.md 5, quirk 2): the original leaks
// them, so they are never freed; kept reachable here so a leak check does not count them.
std::vector<cSC4StaticFloraData*>& g_droppedFloraData = *new std::vector<cSC4StaticFloraData*>();   // never destroyed

constexpr std::uint32_t kGZIID_cISC4FloraOccupantOriginal = 0xc83dd057;   // the id the original asks a flora occupant for
constexpr std::uint32_t kPropertyPreviewOccupant = 0x029244db;            // a preview occupant (foundations 5.2): ignored
constexpr std::uint32_t kOccupantTypeBuildingFlora = 0x278128a0;
constexpr std::uint32_t kOccupantTypeNetworkFlora = 0xc772bf98;
constexpr std::uint32_t kAllOccupants = 0xffffffffu;
constexpr const char* kAgentName = "cSC4FloraSimulator";

// floor by the 1.5 * 2^23 trick (0x006B031C, Mac GetElevationIndex / DoDailySimulation): n := rhe(v - 0.5f), plus one when
// float(n + 1) = v. Exact for |v| <= 2^22, all in float32.
std::int32_t FloorFloat(float v) {
    const float shifted = (v - 0.5f) + 12582912.0f;
    std::int32_t bits = 0;
    std::memcpy(&bits, &shifted, sizeof bits);
    const std::int32_t low = bits - 0x4b400000, high = bits - 0x4b3fffff;
    return v == static_cast<float>(high) ? high : low;                 // an unordered compare keeps `low`
}

// round half to even by the same trick (SimulateExistingFloraOccupant's seed count)
std::int32_t RoundHalfEven(float v) {
    const float shifted = v + 12582912.0f;
    std::int32_t bits = 0;
    std::memcpy(&bits, &shifted, sizeof bits);
    return bits - 0x4b400000;
}

// float -> u32, truncating; negative values give 0 and values from 2^32 up give 0xffffffff (the Mac build's conversion)
std::uint32_t TruncateToUint32(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 4294967296.0f) return 0xffffffffu;
    return static_cast<std::uint32_t>(v);
}

std::uint32_t ComputeBlocksPerMonth(std::int32_t cellsX, std::int32_t cellsZ, std::uint32_t period) {
    // period 0 is an integer divide fault in the original; the divisor 1 (a full pass every month) is flora.md 7's choice
    return (static_cast<std::uint32_t>(cellsX) * static_cast<std::uint32_t>(cellsZ) / (period ? period : 1u)) >> 2;
}

std::string Lower(const cRZString& s) {
    std::string out = s.ToChar();
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

IFloraSimulatorHost* FloraSimulatorHost() { return g_host; }
void SetFloraSimulatorHost(IFloraSimulatorHost* host) { g_host = host; }

// ---------------------------------------------------------------------------------------------- identity
// QueryInterface (Mac i386 0x00089c90): iid 1 / 0x06c66177 -> the simulator, 0x452294aa -> the message target, 0xe4fda3d4 ->
// the serializable; nothing else. The port's primary view is the cIGZSerializable base (the city keeps the object as that).
bool cSC4FloraSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kFloraSimulatorInterfaceId || iid == kGZIID_cIGZSerializable)
        *out = static_cast<cIGZSerializable*>(this);
    else if (iid == kGZIID_cIGZMessageTarget2)
        *out = static_cast<cIGZMessageTarget2*>(this);
    else
        return false;
    AddRef();
    return true;
}

// Release (Mac i386 0x00089c6a): below 2 the final release runs Shutdown and destroys the object.
std::uint32_t cSC4FloraSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

// The destructor (Mac i386 0x0008b2b2) frees the burnt list's nodes and the iterator's table; it releases nothing (Shutdown
// hands the burnt list to the occupant manager).
cSC4FloraSimulator::~cSC4FloraSimulator() = default;

// ---------------------------------------------------------------------------------------------- record (flora.md 10.1)
// Write (Mac i386 0x0008b080): version 2, the four flags, the size, the cell counts, today / period / blocks / wild seeds, the
// iterator flag, the block iterator, the generator, the burnt list (occupant references) and the two accumulators.
bool cSC4FloraSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    stream.SetUint8(enabled_ ? 1 : 0);
    stream.SetUint8(propagation_ ? 1 : 0);
    stream.SetUint8(propagationForced_ ? 1 : 0);
    stream.SetUint8(preserve_ ? 1 : 0);
    stream.SetFloat32(sizeX_);
    stream.SetFloat32(sizeZ_);
    stream.SetSint32(cellsX_);
    stream.SetSint32(cellsZ_);
    stream.SetUint32(today_);
    stream.SetUint32(period_);
    stream.SetUint32(blocksPerMonth_);
    stream.SetUint32(wildSeedsPerMonth_);
    stream.SetUint8(keepIteratorFlag_);
    stream << blockWalk_;
    stream << random_;
    stream << burnt_;
    stream.SetFloat32(burnAccumulator_);
    stream.SetFloat32(burntRemovalAccumulator_);
    return !stream.GetError();
}

// Read (Mac i386 0x0008af26): any version but 2 is refused before anything else is read; bools are "non-zero"; the burnt
// list is cleared and refilled (nothing for a count of 0 or from 10,000,000 on).
bool cSC4FloraSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    const auto readBool = [&stream](bool& value) {
        std::uint8_t byte = 0;
        stream.GetUint8(byte);
        value = byte != 0;
    };
    readBool(enabled_);
    readBool(propagation_);
    readBool(propagationForced_);
    readBool(preserve_);
    stream.GetFloat32(sizeX_);
    stream.GetFloat32(sizeZ_);
    stream.GetSint32(cellsX_);
    stream.GetSint32(cellsZ_);
    stream.GetUint32(today_);
    stream.GetUint32(period_);
    stream.GetUint32(blocksPerMonth_);
    stream.GetUint32(wildSeedsPerMonth_);
    stream.GetUint8(keepIteratorFlag_);
    stream >> blockWalk_;
    stream >> random_;
    stream >> burnt_;
    stream.GetFloat32(burnAccumulator_);
    stream.GetFloat32(burntRemovalAccumulator_);
    return !stream.GetError();
}

// ---------------------------------------------------------------------------------------------- lifecycle (flora.md 4)
// Init (Mac i386 0x0008b49e). The occupant manager, weather, terrain and lot manager the original holds are asked of the host
// at each use; the blocking filter is a stateless test (FloraBlockingFilter below).
bool cSC4FloraSimulator::Init() {
    if (initialized_) return true;
    initialized_ = true;
    StaticInit(this);
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        for (std::uint32_t type : {kFloraMsgTerrainHeightsChanged, kFloraMsgInsertOccupant, kFloraMsgRemoveOccupant,
                                   kFloraMsgExemplarChanged, kFloraMsgCityEstablished, kFloraMsgCityObliterated, kFloraCommandPlaceFlora})
            server->AddNotification(AsMessageTarget(), type);
        g_host->RegisterCommand(kFloraCommandPlaceFlora, "PlaceFlora", nullptr);   // the result is ignored
    }
    if (!simulatorHeld_ && g_host) {
        simulatorHeld_ = true;
        g_host->AddSimulatorAgent(AsMessageTarget(), 4, kAgentName, 3);   // SimNewDay, enabled before and after establishment
        g_host->AddSimulatorAgent(AsMessageTarget(), 6, kAgentName, 3);   // SimNewMonth
    }
    if (g_host && g_host->CityPresent()) {
        g_host->CitySizeMeters(sizeX_, sizeZ_);
        g_host->CityCellCounts(cellsX_, cellsZ_);
        blocksPerMonth_ = ComputeBlocksPerMonth(cellsX_, cellsZ_, period_);      // the period before the tuning read (saved or 6)
    }
    if (keepIteratorFlag_ == 0) {
        const std::int32_t maxX = std::max(cellsX_ / 2 - 1, 0);
        const std::int32_t maxZ = std::max(cellsZ_ / 2 - 1, 0);
        blockWalk_.SetRect(0, 0, maxX, maxZ);
        blockWalk_.SetModular(false);                                    // table mode
        blockWalk_.Reset(true);
    }
    // the default record under key 0, flags 0x01 (flora.md 3.2 choice: the original's byte is heap leftovers); when key 0 is
    // already there the original leaks the new record, the port releases it
    auto* fallback = new cSC4StaticFloraData();
    fallback->AddRef();
    MakeDefaultStaticFloraData(*fallback);
    fallback->flags = 0x01;
    if (!g_staticFloraDataMap.emplace(0u, fallback).second) fallback->Release();
    ReadSimulatorTunableValues();
    propagation_ = !(g_host && g_host->CityEstablished());
    if (propagationForced_) propagation_ = true;
    if (g_host && g_host->CheatCodeManagerPresent()) {
        g_host->RegisterCheatCode(kFloraCheatCode, "Flora");
        g_host->AddCheatNotification(AsMessageTarget(), kFloraCheatCode);
    }
    if (cIGZPersistDBSegment* segment = g_host ? g_host->ObliteratedCitySegment() : nullptr) LoadObliterated(segment);
    return true;
}

// Shutdown (Mac i386 0x0008a0fa). The static registry is not touched.
bool cSC4FloraSimulator::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (!burnt_.empty()) {
        if (cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr) {
            occupants->ReleaseOccupantList(burnt_);
        } else {
            for (cISC4Occupant* occupant : burnt_) occupant->Release();
            burnt_.clear();
        }
    }
    if (g_host && g_host->CheatCodeManagerPresent()) {
        g_host->RemoveCheatNotification(AsMessageTarget(), kFloraCheatCode);
        g_host->UnregisterCheatCode(kFloraCheatCode);
    }
    if (simulatorHeld_) {
        simulatorHeld_ = false;
        if (g_host) g_host->RemoveSimulatorAgent(AsMessageTarget());
    }
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        for (std::uint32_t type : {kFloraMsgCityObliterated, kFloraMsgCityEstablished, kFloraMsgExemplarChanged, kFloraMsgTerrainHeightsChanged,
                                   kFloraMsgInsertOccupant, kFloraMsgRemoveOccupant, kFloraCommandPlaceFlora})
            server->RemoveNotification(AsMessageTarget(), type);
        g_host->UnregisterCommand(kFloraCommandPlaceFlora);
    }
    return true;
}

// ReadSimulatorTunableValues (Mac i386 0x00089e4c, flora.md 7): only when the tuning exemplar loads; each property only when
// present. blocksPerMonth and the per-2^20-m^2 wild-seed scaling are recomputed every time (the scaling of a value the
// exemplar does not carry compounds, quirk 11).
void cSC4FloraSimulator::ReadSimulatorTunableValues() {
    if (!g_host) return;
    cISCResExemplar* exemplar = g_host->LoadExemplar(cGZPersistResourceKey{kFloraDirectoryTypeId, 0x7a4a8458, kFloraTuningInstance});
    if (!exemplar) return;
    const cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
    float value = 0.0f;
    if (cSCPropertyHelp::GetPropertyValue(holder, 0xc9dbdd00, value)) burnRate_ = 1.0f / value;
    if (cSCPropertyHelp::GetPropertyValue(holder, 0xc9dbdd01, value)) burntRemovalStep_ = value;
    std::uint32_t count = 0;
    if (cSCPropertyHelp::GetPropertyValue(holder, 0x8761abd0, count)) period_ = count;
    if (cSCPropertyHelp::GetPropertyValue(holder, 0x085489c7, count)) wildSeedsPerMonth_ = count;
    if (cSCPropertyHelp::GetPropertyValue(holder, 0xc9dbdd02, value)) elevationScale_ = 16.0f / value;
    blocksPerMonth_ = ComputeBlocksPerMonth(cellsX_, cellsZ_, period_);
    wildSeedsPerMonth_ = TruncateToUint32(static_cast<float>(wildSeedsPerMonth_) * sizeX_);
    wildSeedsPerMonth_ = TruncateToUint32(static_cast<float>(wildSeedsPerMonth_) * sizeZ_) >> 20;
    exemplar->Release();
}

// ---------------------------------------------------------------------------------------------- messages (flora.md 5)
// DoMessage (Mac i386 0x0008cb78): always true.
bool cSC4FloraSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    void* raw = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) {
        if (type == kFloraMsgSimNewDay) DoDailySimulation();
        else if (type == kFloraMsgCityEstablished) propagation_ = propagationForced_;
        else if (type == kFloraMsgCityObliterated) propagation_ = true;
        return true;
    }
    auto* standard = static_cast<cIGZMessage2Standard*>(raw);
    // InsertOccupant / RemoveOccupant: void1 = the occupant, data3 = its type; a preview occupant is ignored
    const auto previewOccupant = [](cISC4Occupant* occupant) {
        cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
        return !occupant || (holder && holder->HasProperty(kPropertyPreviewOccupant));
    };
    switch (type) {
        case kFloraMsgSimNewDay:
            DoDailySimulation();
            break;
        case kFloraMsgSimNewMonth:
            today_ = static_cast<std::uint32_t>(standard->GetData1());
            DoMonthlySimulation();
            break;
        case kFloraMsgInsertOccupant: {
            auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
            if (!previewOccupant(occupant)) DoOccupantInserted(occupant, static_cast<std::uint32_t>(standard->GetData3()));
            break;
        }
        case kFloraMsgRemoveOccupant: {
            auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
            if (!previewOccupant(occupant)) DoOccupantRemoved(occupant, static_cast<std::uint32_t>(standard->GetData3()));
            break;
        }
        case kFloraCommandPlaceFlora:
            PlaceFloraCommand(static_cast<cIGZUnknown*>(standard->GetVoid1()));
            break;
        case kFloraMsgExemplarChanged:
            ReloadFloraTypes(static_cast<std::uint32_t>(standard->GetData2()));   // data1 is read and ignored
            break;
        case kFloraMsgCityEstablished:
            propagation_ = propagationForced_;
            break;
        case kFloraMsgCityObliterated:
            propagation_ = true;
            break;
        case kFloraMsgTerrainHeightsChanged:
            if (g_host && g_host->TerrainInitialized())
                DoTerrainChangedMessage(static_cast<std::uint32_t>(standard->GetData1()), static_cast<std::uint32_t>(standard->GetData2()));
            break;
        case kFloraMsgCheatCode: {
            // "Flora <word> ...": the cheat code manager hands the typed line over in void2 (borrowed). Another cheat, or a
            // bare "Flora", takes the exemplar-changed path with the message's data2 (quirk 2).
            bool handled = false;
            if (static_cast<std::uint32_t>(standard->GetData1()) == kFloraCheatCode) {
                const auto* text = static_cast<const cIGZString*>(standard->GetVoid2());
                cRZCmdLine line(text ? text->ToChar() : "");
                if (line.argc() >= 2) {
                    DoCheat(text->ToChar());
                    handled = true;
                }
            }
            if (!handled) ReloadFloraTypes(static_cast<std::uint32_t>(standard->GetData2()));
            break;
        }
        default:
            break;
    }
    standard->Release();
    return true;
}

// The "Flora" cheat (flora.md 5.2): word 1, lower-cased: on / off / preserve [off] / propogation [off] (sic) / blast n m s.
void cSC4FloraSimulator::DoCheat(const char* text) {
    cRZCmdLine line(text);
    const std::string word = Lower(line[1]);
    const auto secondIsOff = [&line] { return line.argc() >= 3 && Lower(line[2]) == "off"; };
    if (word == "on") {
        EnableFlora(true);
    } else if (word == "off") {
        EnableFlora(false);
    } else if (word == "preserve") {
        preserve_ = true;
        if (secondIsOff()) preserve_ = false;
    } else if (word == "propogation") {
        propagationForced_ = propagation_ = true;
        if (secondIsOff()) propagationForced_ = propagation_ = false;
    } else if (word == "blast") {
        // n centers, m seeds around each, Gaussian spread s (words 2..4, read whatever the word count; a missing word is 0)
        const std::uint32_t centers = RZ::ConvertToUint(line[2].ToChar());
        const std::uint32_t seeds = RZ::ConvertToUint(line[3].ToChar());
        const double spread = static_cast<double>(RZ::ConvertToUint(line[4].ToChar()));
        for (std::uint32_t i = 0; i != centers; ++i) {
            const double u = random_.RandomDoubleUniform();
            const float centerX = sizeX_ * static_cast<float>(u);
            const double v = random_.RandomDoubleUniform();
            const float centerZ = sizeZ_ * static_cast<float>(v);
            for (std::uint32_t j = 0; j < seeds; ++j) {
                const float x = static_cast<float>(random_.RandomDoubleGaussian() * spread + static_cast<double>(centerX));
                const float z = static_cast<float>(random_.RandomDoubleGaussian() * spread + static_cast<double>(centerZ));
                if (0.0f <= x && 0.0f <= z && x < sizeX_ && z <= sizeZ_) {   // z inclusive (quirk 13)
                    float score = 0.0f;
                    const std::uint32_t best = GetBestFloraTypeForLocation(x, z, score);
                    DropSeed(best, x, z);
                }
            }
        }
    }
}

// The PlaceFlora command (0xab99a64b): parameters 0 (type, u32), 1 and 2 (floats). Quirk 3: parameter 2 lands in y and z is
// -1, so the location test fails and nothing is planted. A missing parameter reads the set's empty variant.
void cSC4FloraSimulator::PlaceFloraCommand(cIGZUnknown* parameters) {
    if (!parameters) return;   // the original dereferences it
    auto* set = static_cast<cIGZCommandParameterSet*>(static_cast<void*>(parameters));
    const std::uint32_t type = (*set)[0].GetValUint32();
    float position[3] = {0.0f, 0.0f, -1.0f};
    position[0] = (*set)[1].GetValFloat32();
    position[1] = (*set)[2].GetValFloat32();
    AddNewFloraOccupant(type, position, false);   // the result is ignored
}

// The exemplar-changed message (and the cheat fallback): a registered type (0 included) reloads every flora type, the old
// records dropped unreleased; the tuning exemplar's instance re-reads the tuning. The group is never checked.
void cSC4FloraSimulator::ReloadFloraTypes(std::uint32_t instance) {
    if (GetStaticFloraData(instance)) {
        g_floraTypeArray.clear();
        for (auto& entry : g_staticFloraDataMap) g_droppedFloraData.push_back(entry.second);
        g_staticFloraDataMap.clear();
        g_floraFamilyMap.clear();
        LoadAllStandardFloraTypes(this);
    }
    if (instance == kFloraTuningInstance) ReadSimulatorTunableValues();
}

// ---------------------------------------------------------------------------------------------- static registry (3.2, 3.3)
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

// StaticInit (Mac i386 0x0008a49e): once per process. The original also registers an application-shutdown callback that only
// frees the registry at exit (flora.md 3.2: nothing observable depends on it); the port keeps the records until StaticShutdown.
bool cSC4FloraSimulator::StaticInit(cSC4FloraSimulator* simulator) {
    if (!g_staticInitialized) {
        g_staticInitialized = true;
        LoadAllStandardFloraTypes(simulator);
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

// LoadAllStandardFloraTypes (Mac i386 0x00089d90): every 0x6534284a resource whose exemplar has 0x10 = 0xf, in the resource
// manager's order, then FloraTypesUpdated (posted even when there is no resource manager).
void cSC4FloraSimulator::LoadAllStandardFloraTypes(cSC4FloraSimulator* simulator) {
    if (g_host)
        g_host->EnumerateFloraResources(
            [](const cGZPersistResourceKey& key, void*) { AddStaticFloraDataFromResource(key); }, nullptr);
    PostFloraTypesUpdatedMessage(simulator);
}

// FloraTypesUpdated 0x87a6277f (flora.md 9): queued; void1 = IGZUnknown = the simulator, data2 = the number of map entries.
void cSC4FloraSimulator::PostFloraTypesUpdatedMessage(cSC4FloraSimulator* simulator) {
    cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
    if (!server) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kFloraMsgTypesUpdated);
    cIGZUnknown* unknown = simulator ? static_cast<cIGZSerializable*>(simulator) : nullptr;
    message->SetVoid1(unknown);
    message->SetIGZUnknown(unknown);
    message->SetData2(static_cast<std::uint64_t>(g_staticFloraDataMap.size()));
    server->MessagePost(message, false);
    message->Release();
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
    // Seed types (flora.md 6.11): the new record seeds itself; every record whose seed type is the new record's next cluster
    // type seeds the new type from now on (the original's "other.next == type" test assigns a value it already has).
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

// GetElevationIndex (Mac i386 0x0008a380, flora.md 6.1): floor((altitude - sea) * scale) (sea - altitude for water types)
// clamped to 0..15, all float32.
int cSC4FloraSimulator::GetElevationIndex(float x, float z, bool belowSeaLevel) const {
    const float sea = g_host ? g_host->SeaLevel() : 0.0f;
    const float height = g_host ? g_host->TerrainHeight(x, z) : 0.0f;
    const float scaled = (belowSeaLevel ? sea - height : height - sea) * elevationScale_;
    const std::int32_t index = std::min(FloorFloat(scaled), 15);
    return index < 1 ? 0 : index;
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
// and touch no blocking occupant other than `ignore` (SC4GeometryUtilities::BoundingBoxesIntersect: strict on every axis,
// boxes that only touch do not block).
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
    occupants->GetOccupantsByBBox(found, min, max, &filter, kAllOccupants);
    bool ok = true;
    for (cISC4Occupant* occupant : found) {
        if (!ok || occupant == ignore) continue;
        float otherMin[3], otherMax[3];
        occupant->GetBoundingBox(otherMin, otherMax);
        if (SC4GeometryUtilities::BoundingBoxesIntersect(min, max, otherMin, otherMax, nullptr, nullptr)) ok = false;
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
// LocationIsOKForNewFlora. The new occupant faces one of four orientations (RandomSint32RangeUniform(0, 4)), is born
// today and sits on the ground (the sea surface for land flora in the water).
cISC4Occupant* cSC4FloraSimulator::AddNewFloraOccupant(std::uint32_t type, float* xyz, bool skipClusters) {
    if (!g_host) return nullptr;
    // Type 0 asks for the best species even though Init registers a default record under key 0 (flora.md 6.5 step 1).
    if (type == 0) {
        float score = 0.0f;
        type = GetBestFloraTypeForLocation(xyz[0], xyz[2], score);
        if (type == 0) {
            if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "flora: no species suits %.0f,%.0f\n", xyz[0], xyz[2]);
            return nullptr;
        }
    } else if (!IsFloraTypeValid(type)) {
        return nullptr;
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
            if (occupants) occupants->GetOccupantsByBBox(found, min, max, kOccupantTypeFlora, kAllOccupants);
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

// DropSeed (Mac i386 0x0008be92, flora.md 6.6): a seed takes on an OK spot whose score reaches the type's minimum, with a
// chance that falls as the score rises (quirk 5); one RandomDoubleUniform only past both tests. True when a planting was
// attempted (whatever its result).
bool cSC4FloraSimulator::DropSeed(std::uint32_t type, float x, float z) {
    if (!LocationIsOKForNewFlora(type, x, z, nullptr)) return false;
    const cSC4StaticFloraData* data = GetStaticFloraData(type);
    const float score = GetPropagationScoreForFlora(type, x, z);
    const float minimum = data->traits[8];
    if (!(minimum <= score)) return false;
    const float u = static_cast<float>(random_.RandomDoubleUniform());
    if (!((score - minimum) * u < (1.0f - minimum) * data->traits[9])) return false;
    float position[3] = {x, 0.0f, z};
    AddNewFloraOccupant(type, position, false);
    return true;
}

// ==================================================================================================== simulation

// DoDailySimulation (Mac i386 0x0008abcc, flora.md 6.8): at most one accumulator step a day (quirk 8), then the whole
// burnt-tree removals the second accumulator holds.
void cSC4FloraSimulator::DoDailySimulation() {
    burnAccumulator_ += burnRate_;
    if (1.0f <= burnAccumulator_) {
        burnAccumulator_ -= 1.0f;
        burntRemovalAccumulator_ += burntRemovalStep_;
    }
    const float removals = burntRemovalAccumulator_;
    if (1.0f <= removals) {
        const std::int32_t count = FloorFloat(removals);
        burntRemovalAccumulator_ = removals - static_cast<float>(count);
        RemoveBurntFlora(count);
    }
}

// RemoveBurntFlora (Mac i386 0x0008ab5e): the oldest burnt trees first, while there are any.
void cSC4FloraSimulator::RemoveBurntFlora(std::int32_t count) {
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    for (std::int32_t i = 0; count != i && !burnt_.empty();) {
        ++i;
        cISC4Occupant* occupant = burnt_.front();
        burnt_.pop_front();
        if (occupants) occupants->RemoveOccupant(occupant, true, 0);
        occupant->Release();
    }
}

// DoMonthlySimulation (Mac i386 0x0008c352, flora.md 6.7): `blocksPerMonth` 2x2-cell blocks in the shuffled order (a pass
// that runs out restarts with a reshuffle and costs no block), each flora occupant of a block visited; then, while
// propagation is on, the wild seeds.
void cSC4FloraSimulator::DoMonthlySimulation() {
    if (!enabled_) return;
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    OccupantList found;
    for (std::uint32_t blocks = blocksPerMonth_; blocks != 0;) {
        int bx = 0, bz = 0;
        if (!blockWalk_.Next(bx, bz)) {
            blockWalk_.Reset(true);
            continue;
        }
        const int min[2] = {bx * 2, bz * 2}, max[2] = {bx * 2 + 1, bz * 2 + 1};
        if (occupants) occupants->GetOccupantsByStandardCityCells(found, min, max, kOccupantTypeFlora, kAllOccupants);
        for (cISC4Occupant* occupant : found) {
            void* raw = nullptr;
            if (occupant->IsFlagSet(1) && occupant->QueryInterface(kGZIID_cISC4FloraOccupantOriginal, &raw) && raw) {
                auto* flora = static_cast<cISC4FloraOccupant*>(raw);
                SimulateExistingFloraOccupant(flora);
                flora->Release();
            }
            occupant->Release();
        }
        found.clear();
        --blocks;
    }
    if (!propagation_ || !g_host || !g_host->TerrainInitialized()) return;
    // Wild seeds (6.7.3): a random registered type (drawn again, at no cost, when it has flag bit 0), a random spot. The
    // original loops forever when no type is eligible and reads past an empty type array; the port then plants nothing.
    const bool anyEligible = std::any_of(g_floraTypeArray.begin(), g_floraTypeArray.end(), [this](std::uint32_t t) {
        const cSC4StaticFloraData* data = GetStaticFloraData(t);
        return data && (data->flags & 1) == 0;
    });
    if (!anyEligible) return;
    for (std::uint32_t seeds = wildSeedsPerMonth_; seeds != 0;) {
        const std::uint32_t type = g_floraTypeArray[random_.RandomUint32Uniform(static_cast<std::uint32_t>(g_floraTypeArray.size()))];
        const cSC4StaticFloraData* data = GetStaticFloraData(type);
        if (!data || (data->flags & 1)) continue;
        const std::uint32_t x = random_.RandomUint32Uniform(TruncateToUint32(sizeX_));
        const std::uint32_t z = random_.RandomUint32Uniform(TruncateToUint32(sizeZ_));
        DropSeed(type, static_cast<float>(x), static_cast<float>(z));
        --seeds;
    }
}

// SimulateExistingFloraOccupant (Mac i386 0x0008bf9c, flora.md 6.7.2): old age (or a burnt / dying state) is a rebirth as a
// young plant; otherwise the stage follows the age, a score under the type's minimum makes it dying (removed), and a plant
// past its seeding age drops its seeds around itself every seeding interval while propagation is on.
void cSC4FloraSimulator::SimulateExistingFloraOccupant(cISC4FloraOccupant* flora) {
    const std::uint32_t today = today_;
    const std::uint32_t birth = flora->GetBirthDate();
    const std::uint32_t type = flora->GetFloraType();
    const cSC4StaticFloraData* data = GetStaticFloraData(type);
    const std::int32_t state = flora->GetState();
    if (!data) return;
    const float age = static_cast<float>(today - birth);
    if (state > 2 || data->traits[6] <= age || data->traits[5] <= age) {
        flora->SetBirthDate(today_);
        flora->SetState(0);
        flora->AsOccupant()->PostOccupantMessage(kFloraMsgChangeOccupant, 0);
        return;
    }
    std::int32_t stage = age < data->traits[0] ? (data->traits[0] * 0.5f <= age ? 1 : 0) : 2;
    flora->AddRef();
    float position[3] = {0.0f, 0.0f, 0.0f};
    flora->AsOccupant()->GetPosition(position);
    const float score = GetPropagationScoreForFlora(type, position[0], position[2]);
    bool canSeed = false;
    if (propagation_ || (data->flags & 2) == 0) {
        if (score < data->traits[8]) stage = 4;
        else canSeed = !(age < data->traits[1]);
    }
    if (stage != state) {
        flora->SetState(stage);
        if (stage == 1 || stage == 2) {
            flora->AsOccupant()->PostOccupantMessage(kFloraMsgChangeOccupant, 0);
        } else if (stage != 3) {                                         // 0 (only after a non-zero state) or 4: removed
            if (cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr)
                occupants->RemoveOccupant(flora->AsOccupant(), true, 0);
        }
    }
    if (propagation_ && canSeed) {
        const std::uint32_t now = g_host ? g_host->SimDate() : 0;
        const std::uint32_t last = flora->GetLastSeedingDate();
        const std::uint32_t seedType = data->groupType;
        if (static_cast<float>(last) + data->traits[2] < static_cast<float>(now)) {
            const float spread = data->traits[4];
            const std::int32_t seeds = RoundHalfEven(data->traits[3]);
            for (std::int32_t i = 0; i < seeds; ++i) {
                const float x = static_cast<float>(random_.RandomDoubleRangeUniform(static_cast<double>(position[0] - spread),
                                                                                   static_cast<double>(position[0] + spread)));
                const float z = static_cast<float>(random_.RandomDoubleRangeUniform(static_cast<double>(position[2] - spread),
                                                                                   static_cast<double>(position[2] + spread)));
                if (0.0f <= x && x < sizeX_ && 0.0f <= z && z < sizeZ_) DropSeed(seedType, x, z);
                flora->SetLastSeedingDate(now);
            }
        }
    }
    flora->Release();
}

// DoOccupantInserted (Mac i386 0x0008a2e6, flora.md 6.10.1): a building clears the flora within 2 m of its box at any height;
// a blocking network piece the flora under its box (from y 0 up).
void cSC4FloraSimulator::DoOccupantInserted(cISC4Occupant* occupant, std::uint32_t occupantType) {
    float margin = 0.0f;
    if (occupantType == kOccupantTypeBuildingFlora) {
        margin = 2.0f;
    } else {
        if (occupantType != kOccupantTypeNetworkFlora) return;
        FloraBlockingFilter filter;
        if (!filter.IsOccupantIncluded(occupant)) return;
    }
    float min[3], max[3];
    occupant->GetBoundingBox(min, max);
    min[1] = 0.0f;
    const std::uint32_t top = 0x7effffff;                               // 1.7e38
    std::memcpy(&max[1], &top, sizeof top);
    for (int i = 0; i < 3; ++i) {                                       // cS3DBoundingBox::Inflate
        min[i] -= margin;
        max[i] += margin;
    }
    DemolishFloraInBoundingBox(min, max, 0);
}

// DoOccupantRemoved (Mac i386 0x0008ac80): a removed flora occupant leaves the burnt list (and its reference with it).
void cSC4FloraSimulator::DoOccupantRemoved(cISC4Occupant* occupant, std::uint32_t occupantType) {
    if (occupantType != kOccupantTypeFlora || burnt_.empty()) return;
    auto it = std::find(burnt_.begin(), burnt_.end(), occupant);
    if (it == burnt_.end()) return;
    occupant->Release();
    burnt_.erase(it);
}

// EnableFlora (Mac i386 0x0008aaa0, flora.md 6.10.4): only a true -> false change removes every flora occupant of the city.
void cSC4FloraSimulator::EnableFlora(bool enable) {
    if (enabled_ == enable) return;
    enabled_ = enable;
    if (enable) return;
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    if (!occupants) return;
    OccupantList found;
    if (occupants->GetOccupantsByStandardCityCells(found, nullptr, nullptr, kOccupantTypeFlora, kAllOccupants))
        for (cISC4Occupant* occupant : found) DemolishFloraOccupant(occupant, 0);
    ReleaseAll(found);
}

// ==================================================================================================== obliterated city

// SaveObliterated (Mac i386 0x0008a746, flora.md 10.3): the count record, then each flora occupant's own record (written with
// its flag 1 cleared, only when the record opened). Always true.
bool cSC4FloraSimulator::SaveObliterated(cIGZPersistDBSegment* segment) const {
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    OccupantList flora;
    if (occupants) occupants->GetOccupantsByOccupantManagerCells(flora, nullptr, nullptr, kOccupantTypeFlora, kAllOccupants);
    {
        cSC4AutoSaveStream count(segment, kObliteratedFloraRecordType, kObliteratedFloraCountGroup, 1, 0);
        if (count.IsOpen()) count.GetStream()->SetUint32(static_cast<std::uint32_t>(flora.size()));
    }
    std::uint32_t index = 0;
    for (cISC4Occupant* occupant : flora) {
        cSC4AutoSaveStream record(segment, kObliteratedFloraRecordType, kObliteratedFloraOccupantGroup, index, 0);
        void* raw = nullptr;
        if (record.IsOpen() && occupant->QueryInterface(kGZIID_cIGZSerializable, &raw) && raw) {
            occupant->ClearFlag(1);
            static_cast<cIGZSerializable*>(raw)->Write(*record.GetStream());
            occupant->SetFlag(1);
            static_cast<cIGZSerializable*>(raw)->Release();
        }
        occupant->Release();
        ++index;
    }
    return true;
}

// LoadObliterated (Mac i386 0x0008a5e8, flora.md 10.3): the count (0 when its record does not open), then each record that
// opens becomes a new flora occupant, read (the result ignored), initialised and inserted. False only without an occupant
// manager (after the count was read).
bool cSC4FloraSimulator::LoadObliterated(cIGZPersistDBSegment* segment) {
    std::uint32_t count = 0;
    {
        cSC4AutoLoadStream header(segment, kObliteratedFloraRecordType, kObliteratedFloraCountGroup, 1);
        if (header.IsOpen()) header.GetStream()->GetUint32(count);
    }
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    if (!occupants) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        cSC4AutoLoadStream record(segment, kObliteratedFloraRecordType, kObliteratedFloraOccupantGroup, i);
        if (!record.IsOpen()) continue;
        auto* flora = new cSC4FloraOccupant();
        flora->AddRef();
        flora->Read(*record.GetStream());
        cISC4Occupant* occupant = flora->AsOccupant();
        occupant->Init();
        occupants->InsertOccupant(occupant, 0);
        flora->Release();
    }
    return true;
}

// ==================================================================================================== terrain changes

// DoMessage's 0x278e6003 branch: the rectangle grown by one cell towards the origin and clamped to the city's cells.
void cSC4FloraSimulator::DoTerrainChangedMessage(std::uint32_t data1, std::uint32_t data2) {
    std::int32_t rect[4];
    rect[0] = std::max(static_cast<std::int32_t>(data1 >> 16) - 1, 0);
    rect[1] = std::max(static_cast<std::int32_t>(data2 >> 16) - 1, 0);
    rect[2] = std::min(static_cast<std::int32_t>(data1 & 0xffff), cellsX_ - 1);
    rect[3] = std::min(static_cast<std::int32_t>(data2 & 0xffff), cellsZ_ - 1);
    DoTerrainAltitudeChange(rect);
}

// 0x1000c19c0 (Mac i386 0x8c9e2). Every flora occupant of the cells whose ground moved (by 0.001 m or more, or by NaN) is
// removed, unless the "preserve" cheat keeps them, in which case (and for the ones whose ground did not move) the occupant
// follows the ground.
void cSC4FloraSimulator::DoTerrainAltitudeChange(const std::int32_t rect[4]) {
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    if (!occupants) return;
    OccupantList found;
    if (!occupants->GetOccupantsByStandardCityCells(found, rect, rect + 2, kOccupantTypeFlora, kAllOccupants)) return;
    int removed = 0;
    for (cISC4Occupant* occupant : found) {
        void* raw = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4FloraOccupant, &raw) && raw) {
            auto* flora = static_cast<cISC4FloraOccupant*>(raw);
            float position[3];
            flora->AsOccupant()->GetPosition(position);
            bool onLot = false;
            const float ground = g_host->GroundHeight(position[0], position[2], onLot);
            if (!preserve_ && !(std::fabs(ground - position[1]) < 0.001f)) {
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

// ==================================================================================================== burning and demolition

bool cSC4FloraSimulator::KillFloraOccupant(cISC4Occupant* occupant) {
    cISC4FloraOccupant* flora = nullptr;
    bool killed = false;
    if (occupant->QueryInterface(kGZIID_cISC4FloraOccupant, reinterpret_cast<void**>(&flora)) && occupant->IsFlagSet(1)) {
        if (flora->GetState() != 3) {
            flora->SetState(3);
            flora->SetFloraType(kBurntFloraType);
            flora->SetStaticFloraData(GetStaticFloraData(kBurntFloraType));
            occupant->PostOccupantMessage(kFloraMsgChangeOccupant, 0);
            occupant->AddRef();
            burnt_.push_back(occupant);
        }
        killed = true;
    }
    if (flora) flora->Release();
    return killed;
}

// Mac i386 0x00089bfc (vslot 0x4c): the occupant manager removes the plant and shuts it down; the int is unused.
bool cSC4FloraSimulator::DemolishFloraOccupant(cISC4Occupant* occupant, int) {
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    return occupants && occupants->RemoveOccupant(occupant, true, 0);   // manager vslot 0x54
}

// Mac i386 0x0008a9d2 (vslot 0x50): every flora occupant on the inclusive cell rect {x0, z0, x1, z1} is demolished; false
// when one of them could not be. The int is unused.
bool cSC4FloraSimulator::DemolishFloraInCellArea(const std::int32_t rect[4], int) {
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    if (!occupants) return true;
    OccupantList found;
    const int a[2] = {rect[0], rect[1]}, b[2] = {rect[2], rect[3]};
    if (!occupants->GetOccupantsByStandardCityCells(found, a, b, kOccupantTypeFlora, kAllOccupants)) return true;   // vslot 0xa0
    bool all = true;
    for (cISC4Occupant* occupant : found) {
        if (!DemolishFloraOccupant(occupant, 0)) all = false;
        occupant->Release();
    }
    found.clear();
    return all;
}

// Mac i386 0x0008a904 (vslot 0x54): the same over the flora of a box; the int is unused (quirk 7).
bool cSC4FloraSimulator::DemolishFloraInBoundingBox(const float* min, const float* max, int) {
    cISC4OccupantManager* occupants = g_host ? g_host->OccupantManager() : nullptr;
    if (!occupants) return true;
    OccupantList found;
    if (!occupants->GetOccupantsByBBox(found, min, max, kOccupantTypeFlora, kAllOccupants)) return true;
    bool all = true;
    for (cISC4Occupant* occupant : found) {
        if (!DemolishFloraOccupant(occupant, 0)) all = false;
        occupant->Release();
    }
    found.clear();
    return all;
}

}  // namespace sc4

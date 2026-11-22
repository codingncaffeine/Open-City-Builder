#include "sc4/SaveGame.h"

#include <ctime>
#include <time.h>

#include "gz/GZCOMDirector.h"
#include "gz/cIGZSerializable.h"
#include "sc4/AutoHelpers.h"

#include "gz/cIGZPersistDBSerialRecord.h"
#include "rz/cRZRandom.h"

namespace sc4 {

namespace {
SaveGameServices g_services;
const cGZPersistResourceKey kVersionKey{0xea3ff4f5, 0xea3ff506, 0xea3ff4f5};
constexpr std::uint32_t kStringRecordType = 0x2026960b;
}  // namespace

SaveGameServices& GetSaveGameServices() { return g_services; }

std::uint32_t SC4ReadAppLoadSaveVersion(cIGZPersistDBSegment* segment) {
    std::uint32_t version = 0, size = 4;
    if (segment->ReadRecord(kVersionKey, &version, size) != 0xffffffffu) return version;
    return segment->TestForRecord(kVersionKey) ? 0 : kLoadSaveVersionDefault;
}

bool SC4WriteAppLoadSaveVersion(cIGZPersistDBSegment* segment) {
    std::uint32_t version = kLoadSaveVersionDefault;
    return segment->WriteRecord(kVersionKey, &version, 4);
}

// Opens a city file as a database segment (the caller closes it with SC4CloseSaveGame). Unreadable versions are refused
// when asked for.
cIGZPersistDBSegment* SC4OpenSaveGame(const cIGZString& path, bool write, bool requireKnownVersion) {
    cIGZPersistDBSegment* segment = g_services.createCitySegment ? g_services.createCitySegment() : nullptr;
    if (!segment) return nullptr;
    if (segment->Init()) {
        segment->SetPath(path);
        if (segment->Open(!write, write)) {
            const std::uint32_t version = SC4ReadAppLoadSaveVersion(segment);
            if (version - kLoadSaveVersionOldest < 5 || !requireKnownVersion) return segment;
        }
        segment->Shutdown();
    }
    segment->Release();
    return nullptr;
}

bool SC4CloseSaveGame(cIGZPersistDBSegment* segment) {
    const bool open = segment->IsOpen();
    if (open) {
        segment->Close();
        segment->Shutdown();
        segment->Release();
    }
    return open;
}

bool SC4ReadStringRecord(cIGZPersistDBSegment* segment, std::uint32_t group, std::uint32_t instance, cIGZString& out) {
    cIGZPersistDBRecord* record = nullptr;
    if (!segment->OpenRecord(cGZPersistResourceKey{kStringRecordType, group, instance}, &record, 1)) return false;
    bool ok = false;
    cIGZPersistDBSerialRecord* serial = nullptr;
    if (record->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, reinterpret_cast<void**>(&serial))) {
        ok = serial->GetFieldString(out);
        serial->Release();
    }
    record->Close();
    record->Release();
    return ok;
}

bool SC4WriteStringRecord(cIGZPersistDBSegment* segment, std::uint32_t group, std::uint32_t instance, const cIGZString& value) {
    cIGZPersistDBRecord* record = nullptr;
    if (!segment->OpenRecord(cGZPersistResourceKey{kStringRecordType, group, instance}, &record, 3)) return false;
    bool ok = false;
    cIGZPersistDBSerialRecord* serial = nullptr;
    if (record->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, reinterpret_cast<void**>(&serial))) {
        ok = serial->SetFieldString(value);
        serial->Release();
    }
    record->Close();
    record->Release();
    return ok;
}

// The regional city record of a city file. The object comes from COM (class 0x6a037c24) or the host's factory.
bool SC4ReadRegionalCity(cIGZPersistDBSegment* segment, cRZAutoRefCount<cISC4RegionalCity>& out) {
    cSC4AutoLoadStream stream(segment, 0xca027edb, 0xca027ee1, 0);
    if (!stream.IsOpen()) return false;
    cRZAutoRefCount<cISC4RegionalCity> city;
    if (cIGZCOM* com = GZCOM()) com->CreateInstance(0x6a037c24, 0x69d2be1d, reinterpret_cast<void**>(city.AsOutParam()));
    if (!city && g_services.createRegionalCity) city = g_services.createRegionalCity();
    if (!city) return false;
    cIGZSerializable* serializable = nullptr;
    if (!city->QueryInterface(0xe4fda3d4, reinterpret_cast<void**>(&serializable))) return false;
    const bool ok = serializable->Read(*stream.GetStream());
    serializable->Release();
    if (!ok) return false;
    out = city;
    out->Init();
    return true;
}

bool SC4WriteRegionalCity(cIGZPersistDBSegment* segment, cISC4RegionalCity* city) {
    cSC4AutoSaveStream stream(segment, 0xca027edb, 0xca027ee1, 0, 1);
    if (!stream.IsOpen()) return false;
    cIGZSerializable* serializable = nullptr;
    if (!city->QueryInterface(0xe4fda3d4, reinterpret_cast<void**>(&serializable))) return false;
    const bool ok = serializable->Write(*stream.GetStream());
    serializable->Release();
    return ok;
}

// i386 0x001636e4: the city file at `path` opened for writing (a known version required), the record written, the file closed.
bool SC4WriteRegionalCity(const cIGZString& path, cISC4RegionalCity* city) {
    cIGZPersistDBSegment* segment = SC4OpenSaveGame(path, true, true);
    if (!segment) return false;
    const bool ok = SC4WriteRegionalCity(segment, city);
    SC4CloseSaveGame(segment);
    return ok;
}

std::uint32_t SC4CreateCitySerialNumber() {
    const std::time_t now = std::time(nullptr);
    cRZRandom random(0xffffffffu);
    std::tm epoch{};
    epoch.tm_mon = 1;
    epoch.tm_year = 100;
    epoch.tm_wday = 1;
    epoch.tm_yday = 1;
    epoch.tm_isdst = 1;
    const std::time_t base = std::mktime(&epoch);
    const std::uint32_t offset = static_cast<std::uint32_t>(base) - static_cast<std::uint32_t>(::timezone);
    return (random.RandomUint32Uniform() & 0xf) | ((static_cast<std::uint32_t>(now) - offset) * 16u);   // wraps like the 32-bit original
}

}  // namespace sc4

// City-file helpers of the application (SC4OpenSaveGame .. SC4CreateCitySerialNumber, 0x1001e9090..0x1001e9ff4).
//  * The save version lives in a 4-byte record {0xea3ff4f5, 0xea3ff506, 0xea3ff4f5}; a file without it counts as version
//    0x42 (when the record exists but cannot be read the version is 0). Versions 0x3e..0x42 can be loaded.
//  * The regional city record {0xca027edb, 0xca027ee1, 0} (stream version 1) holds the region's view of the city; the
//    object is created through COM (class 0x6a037c24) or, when that fails, by the host's factory.
//  * String records are {0x2026960b, group, instance} serial records holding one string.
//  * City serial numbers: the seconds since 1 Jan 2000 (local time) shifted left by four, plus a random nibble.
#pragma once
#include <cstdint>
#include <functional>

#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZString.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/ISC4RegionalCity.h"

namespace sc4 {

struct SaveGameServices {
    std::function<cIGZPersistDBSegment*()> createCitySegment;     // new cSC4DBSegment (AddRef'd)
    std::function<cISC4RegionalCity*()> createRegionalCity;       // new cSC4RegionalCity (AddRef'd) when COM has none
};
SaveGameServices& GetSaveGameServices();

constexpr std::uint32_t kLoadSaveVersionDefault = 0x42, kLoadSaveVersionOldest = 0x3e;
std::uint32_t SC4ReadAppLoadSaveVersion(cIGZPersistDBSegment* segment);       // @ 0x1001e9148
bool SC4WriteAppLoadSaveVersion(cIGZPersistDBSegment* segment);               // @ 0x1001e9585
cIGZPersistDBSegment* SC4OpenSaveGame(const cIGZString& path, bool write, bool requireKnownVersion);   // @ 0x1001e9090
bool SC4CloseSaveGame(cIGZPersistDBSegment* segment);                         // @ 0x1001e91b6
bool SC4ReadStringRecord(cIGZPersistDBSegment* segment, std::uint32_t group, std::uint32_t instance, cIGZString& out);   // @ 0x1001e9453
bool SC4WriteStringRecord(cIGZPersistDBSegment* segment, std::uint32_t group, std::uint32_t instance, const cIGZString& value);   // @ 0x1001e94ea
bool SC4ReadRegionalCity(cIGZPersistDBSegment* segment, cRZAutoRefCount<cISC4RegionalCity>& out);   // @ 0x1001e9603
bool SC4WriteRegionalCity(cIGZPersistDBSegment* segment, cISC4RegionalCity* city);                  // @ 0x1001e97dc
bool SC4WriteRegionalCity(const cIGZString& path, cISC4RegionalCity* city);                          // @ 0x1001e9799
std::uint32_t SC4CreateCitySerialNumber();                                    // @ 0x1001e9ff4

}  // namespace sc4

// The native host of cSC4BuildingDevelopmentSimulator (NativeBuildingDevelopment.cpp).
#pragma once
#include <cstdint>

#include <vector>

namespace sc4 { struct BuildingProfile; class cSC4Demand; struct DemandBuildingProfile; class cISC4BuildingOccupant; }
struct cGZPersistResourceKey;

namespace native {

// Replaces the generated neutral host; installed before the city's components Init.
void InstallBuildingDevelopmentHost();

// The building development simulator's catalogue for the growth developers' host (NativeGrowthDevelopers.cpp cannot include
// BuildingDevelopmentSimulator.h: the headers clash). Null / 0 / false without a city or a building development simulator.
sc4::cISC4BuildingOccupant* NativeAddNewBuildingOccupant(std::uint32_t type, float position[3], int rotation);   // vslot 0x1c
const sc4::BuildingProfile* NativeBuildingProfile(std::uint32_t buildingType);            // vslot 0x138
float NativeMaxCapacity(int zoneType, std::uint32_t purpose, std::uint32_t wealth);      // vslot 0x168
sc4::cSC4Demand* NativeDemandChannel(std::uint32_t type, std::uint32_t keyOffset);       // SL::spDemandSim vslot 0x30
std::uint32_t NativeDeveloperType(std::uint32_t purpose, std::int32_t wealth);           // GetDeveloperType_Static
void NativeAllBuildingTypes(std::vector<std::uint32_t>& out);
bool NativeBuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key);
std::int32_t NativeDayOfLastSuccessfulBuild(std::uint32_t developerType);
const sc4::DemandBuildingProfile* NativeDemandBuildingProfile(std::uint32_t buildingType);   // the census's copy of the profile
void NativeEnsureBuildingCatalogue();   // cSC4BuildingDevelopmentSimulator::StaticInit (idempotent)
bool NativeIsBuildingType(std::uint32_t id);                                           // IsBuildingTypeValid
bool NativeIsBuildingFamily(std::uint32_t id);                                         // IsBuildingFamilyTypeValid
void NativeBuildingsInFamily(std::uint32_t family, std::vector<std::uint32_t>& out);   // GetBuildingTypesByFamily
bool NativeIsBuildingInFamily(std::uint32_t type, std::uint32_t family);
bool NativeRetainingWallExists(std::uint32_t id);                                      // GetLotRetainingWallKeyFromType
bool NativeBuildingFitsInZone(std::uint32_t buildingPurpose, std::uint32_t zoneType);    // @ 0x100150a86
std::uint16_t NativeFreeConstructionResources();                                       // vslot 0x180
bool NativeClaimConstructionResources(std::uint16_t amount);                           // vslot 0x190
void NativeReleaseConstructionResources(std::uint16_t amount);                         // vslot 0x198
bool NativeBuildingCheat(int which);   // 0 CheatIgnorePlumbing (0x1b0), 1 CheatIgnorePower (0x1b8), 2 CheatIgnoreCaps (0x1c0)

}  // namespace native

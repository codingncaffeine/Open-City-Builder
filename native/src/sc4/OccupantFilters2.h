// More occupant filters (0x1000a62f6..0x1000a7038, 0x1005c7744, 0x10071eebe): occupant-type lists, view level,
// network flags, surface networks, "placed" networks, plus the always-true lot-configuration filter.
// The class ids of the serializable ones: Types 0xe9c75ee8, ViewLevel 0xaac3c01d, Network 0xeac3a3d2.
// Everything that needs the network occupant (interface 0xa821ef94: flags are vslot 0x60, "is placed" vslot
// 0xa8) or the view goes through IOccupantFilterWorld.
#pragma once
#include <cstdint>
#include "sc4/CommonIIDs.h"
#include <unordered_set>
#include <vector>

#include "sc4/OccupantManager.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4OccupantTypesFilter = 0xe9c75ee8, kGZCLSID_cSC4ViewLevelOccupantFilter = 0xaac3c01d,
                        kGZCLSID_cSC4NetworkOccupantFilter = 0xeac3a3d2;

class IOccupantFilterWorld {
public:
    virtual ~IOccupantFilterWorld() = default;
    // QueryInterface(0xa821ef94) then the network flags (vslot 0x60); false when the occupant is no network.
    virtual bool NetworkFlags(cISC4Occupant* occupant, std::uint32_t& flags) = 0;
    virtual bool NetworkIsPlaced(cISC4Occupant* occupant) = 0;           // network occupant vslot 0xa8
    virtual int OccupantViewLevel(cISC4Occupant* occupant) = 0;          // SC4GetOccupantViewLevel
    // spCity 0x17c PositionToCell, spLotManager 0x14 the lot there, lot 0x84 GetState; -1 without a lot
    virtual int LotStateAtPosition(float x, float z) { (void)x; (void)z; return -1; }
};
IOccupantFilterWorld* OccupantFilterWorld();
void SetOccupantFilterWorld(IOccupantFilterWorld* world);

// Occupant types given as a list; included when the type is on it. Layout: +0x18 vector<u32>.
class cSC4OccupantTypesFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantTypeIncluded(std::uint32_t type) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4OccupantTypesFilter; }
    void AddType(std::uint32_t type) { types_.push_back(type); }

private:
    std::vector<std::uint32_t> types_;      // +0x18
};

// The same with a hash set (+0x18). Class id of the original is not given by its own function (inherited).
class cSC4OccupantTypesFilterHashed : public cSC4OccupantFilter {
public:
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return types_.count(type) != 0; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    void AddOccupantType(std::uint32_t type) { types_.insert(type); }        // @ 0x10030d52c (the original's own hash set)
    void AddType(std::uint32_t type) { AddOccupantType(type); }

private:
    std::unordered_set<std::uint32_t> types_;
};

// Occupants whose view level (surface / underground ...) equals `level` (+0x14).
class cSC4ViewLevelOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4ViewLevelOccupantFilter; }
    void SetViewLevel(std::uint8_t level) { level_ = level; }

private:
    std::uint32_t level_ = 0;    // +0x14
};

// Network occupants sharing any flag with `mask` (+0x14).
class cSC4NetworkOccupantFilter : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4NetworkOccupantFilter; }
    void SetMask(std::uint32_t mask) { mask_ = mask; }

private:
    std::uint32_t mask_ = 0;     // +0x14
};

// Networks of the surface kinds (any flag of 0x1f4f). A bare counted object (cRZRefCount at +8), not serializable.
class cSC4SurfaceNetworkOccupantFilter final : public cISC4OccupantFilter {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { ++refCount_; return refCount_; }
    std::uint32_t Release() override;
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool IsOccupantTypeIncluded(std::uint32_t) override { return false; }
    bool IsPropertyHolderIncluded(cISCPropertyHolder*) override { return false; }

private:
    std::uint32_t refCount_ = 0;
};

// Wraps another filter and additionally requires the network occupant to be placed (+0x18 inner filter).
class cSC4PlacedNetworkOccupantFilter : public cSC4OccupantFilter {
public:
    explicit cSC4PlacedNetworkOccupantFilter(cISC4OccupantFilter* inner);
    ~cSC4PlacedNetworkOccupantFilter() override;
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool IsPropertyHolderIncluded(cISCPropertyHolder* holder) override { return inner_->IsPropertyHolderIncluded(holder); }
    static bool IsOccPlaced(cISC4Occupant* occupant);

private:
    cISC4OccupantFilter* inner_;
};

// cSC4RealisticOccupantFilter (class 0x8a7a1c2c, i386 0x000795c8..0x0007a192): the default filter, and then, unless their
// flag is set, no zots or signs (occupant group 0x5001; flag +0xd, the TastyZots cheat), no lot base texture on an empty lot
// (state 1; flag +0xc, the Zoneria cheat) and no signpost or text decal prop (type 0xa823821e answering 0x4b44fbe2 or
// 0x2b3b7d86; flag +0xe).
constexpr std::uint32_t kGZCLSID_cSC4RealisticOccupantFilter = 0x8a7a1c2c;
class cSC4RealisticOccupantFilter : public cSC4DefaultOccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4RealisticOccupantFilter; }
    bool showEmptyLotTextures = false;   // +0xc
    bool showZots = false;               // +0xd
    bool showSigns = false;              // +0xe
};

}  // namespace sc4

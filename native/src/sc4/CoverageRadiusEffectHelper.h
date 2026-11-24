// cSC4CoverageRadiusEffectHelper (i386 0x00511918..0x00512056, Mac 0x100770f80..): the coverage rings of the city's existing
// police, fire, health and education buildings. The city view owns one (+0x5f8, view vslot 0x188); the place-building tool
// shows the rings of the category it plops, the query tool those of the building whose dialog is open. Each ring is a visual
// effect ("PlopMode_<Category>_Existing") centred on the building's lot and scaled to twice its coverage radius.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "gz/StreamOps.h"
#include "sc4/View3DWin.h"

namespace sc4 {

class cISC4Occupant;

// A ring effect as the helper drives it (cISC4VisualEffect vslots 0xc Start, 0x10 Stop, 0x14 IsStarted, 0x20 SetTransform).
class ICoverageRingEffect {
public:
    virtual ~ICoverageRingEffect() = default;
    virtual void Start(int transition) = 0;
    virtual void Stop(int transition) = 0;
    virtual bool IsStarted() = 0;
    virtual void SetEffectTransform(const cS3DTransform& transform) = 0;
};

// What the helper asks the city for.
class ICoverageRadiusHost {
public:
    virtual ~ICoverageRadiusHost() = default;
    virtual bool IsPoliceStation(cISC4Occupant* occupant) = 0;   // SL::spPoliceSim 0x58
    virtual bool IsFireStation(cISC4Occupant* occupant) = 0;     // SL::spFireProtectionSim 0x1c
    // The buildings of a category (0 police 0x54, 1 fire 0x58, 2 hospitals res 0xc4, 3 schools res 0xbc), one reference each
    // (the caller releases them), with their coverage radius at `share` (police 0x50 / fire 0x54 / residential 0xb4: the
    // last out value); `hasRadius` false when the simulator refused. False when the category's simulator is missing.
    struct Building { cISC4Occupant* occupant = nullptr; bool hasRadius = false; float radius = 0.0f; };
    virtual bool Buildings(int category, float share, std::vector<Building>& out) = 0;
    // lot manager 0x38 GetOccupantLot, then the lot's 0x3c GetCenter; false without a lot
    virtual bool LotCenter(cISC4Occupant* occupant, float center[3]) = 0;
    // SL::spEffectsManager 0x18 CreateVisualEffect(name): an owned effect, or null
    virtual ICoverageRingEffect* CreateEffect(const char* name) = 0;
};
ICoverageRadiusHost* CoverageRadiusHost();
void SetCoverageRadiusHost(ICoverageRadiusHost* host);

class cSC4CoverageRadiusEffectHelper final : public ICoverageHelper3D {
public:
    cSC4CoverageRadiusEffectHelper() = default;                    // i386 0x005119be
    ~cSC4CoverageRadiusEffectHelper() override;                    // 0x005119b2
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }                  // a member of the view, not reference counted
    std::uint32_t Release() override { return 1; }

    void Init() override;                                          // 0x00512056: RecalcCoverage
    void Shutdown() override;                                      // 0x00511c4c: every ring stopped and forgotten
    void SetCategories(std::uint32_t categories);                  // 0x00511bde: the same, then the new categories
    std::uint32_t GetCategories() const { return categories_; }    // +0x14
    void RecalcCoverage();                                         // 0x00511cb4
    // 0x00511918: 1 police station, 2 fire station, 4 health (property 0x091b42a2), 8 education (property 0x691b42b3)
    static std::uint32_t GetCategoriesForOccupant(cISC4Occupant* occupant);
    std::size_t RingCount() const { return rings_.size(); }        // test view

private:
    struct RingEffectState {
        ICoverageRingEffect* effect = nullptr;   // +0 (an auto reference)
        float radius = 0.0f;                     // +4
        bool seen = false;                       // +8 touched by the current RecalcCoverage
    };
    void UpdateEffectForOccupant(cISC4Occupant* occupant, float radius, const char* effectName);   // 0x005119de
    void StopAndClear();
    std::unordered_map<cISC4Occupant*, RingEffectState> rings_;    // +0 hash_map keyed by the occupant pointer
    std::uint32_t categories_ = 0;                                 // +0x14
};

}  // namespace sc4

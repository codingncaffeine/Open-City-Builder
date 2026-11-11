// cSC4FireOccupant and cSC4Flame — the flames burning on a building (61 + 7 functions, 0x10045e7cc..0x1004607f6).
// Class id 0x29d25f1d, interface 0x69d25c60, occupant type 0x69d25c5f, record version 3.
//
// A fire occupant is attached to a target occupant: SetTarget derives the number of top and side flames from the
// target's footprint (two response curves), SetStage (1..5) picks the effect names fire_N / wfire_N, and the flames are
// placed either on sample points of the target's model (when the renderer can supply them) or randomly on its bounding
// box. Quirk kept from the original: the bounding-box fallback creates top+side+1 flames (a do-while with <=).
// The flame effect, zoom level, model sampling and the building-on-fire notice are seams (FireServices).
// Layout of cSC4Flame (0x58): +0 active, +1 visible, +2 side flame, +4 quarter turns, +8 position, +0x14 transform,
// +0x50 effect handle. cSC4FireOccupant: +0x68 top flames, +0x6c side flames, +0x70 counter, +0x74 stage, +0x78 max
// stage, +0x7c hosed, +0x80 extinguish, +0x88 target, +0x90 flames, +0xa8 flames come from model points, +0xac family.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "gz/StreamOps.h"
#include "sc4/Occupant.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4FireOccupant = 0x29d25f1d;
constexpr std::uint32_t kGZIID_cISC4FireOccupant = 0x69d25c60;
constexpr std::uint32_t kOccupantTypeFire = 0x69d25c5f;
constexpr std::uint32_t kOccupantGroupFire = 0x5005;
constexpr std::uint32_t kPropertyOnFire = 0x69dbebe1;            // set on the target while it burns
constexpr std::uint32_t kPropertyMaxFireStage = 0x49beda31;
#ifndef SC4_kOccupantTypeBuilding_DEFINED
#define SC4_kOccupantTypeBuilding_DEFINED
constexpr std::uint32_t kOccupantTypeBuilding = 0x278128a0;
#endif

// A running particle effect (slots 3..7 of the effect object).
class IFlameEffect : public cIGZUnknown {
public:
    virtual void Start(int flags) = 0;
    virtual void Stop(int flags) = 0;
    virtual bool IsRunning() = 0;
    virtual void Reserved() {}
    virtual void SetTransform(const cS3DTransform& transform) = 0;
};

// One sampled point on the target's model (0x14 bytes).
struct FirePoint {
    float position[3];
    std::uint8_t side;
    std::uint8_t pad[3];
    std::uint32_t quarterTurns;
};
static_assert(sizeof(FirePoint) == 0x14);

struct FireServices {
    std::function<IFlameEffect*(const char* name)> createEffect;                          // effects manager; one reference
    std::function<int()> viewZoom;                                                        // 3D view zoom level (1 = closest)
    std::function<bool(unsigned count, cISC4Occupant* target, std::vector<FirePoint>& out)> pointsOnModel;
    std::function<void(cISC4Occupant* target)> buildingOnFire;                            // the "fire_occupant" notice
    std::function<void(cISC4Occupant* target)> fireEnded;
};
FireServices& GetFireServices();

class cSC4Flame {
public:
    cSC4Flame() = default;
    ~cSC4Flame();
    void Update(std::uint32_t stage);                 // @ 0x10045e840
    void UpdateTransform();                           // @ 0x10045e92c
    void Shutdown();                                  // @ 0x10045ea20
    void SetVisible(bool visible);                    // @ 0x10045ea6e
    bool Write(cIGZOStream& stream) const;            // @ 0x10045eaa8
    bool Read(cIGZIStream& stream);                   // @ 0x10045eb08

    bool active = false;                    // +0 (set once the flame is placed)
    bool visible = true;                    // +1
    bool side = false;                      // +2
    std::uint32_t quarterTurns = 0;         // +4
    float position[3] = {};                 // +8
    cS3DTransform transform;                // +0x14
    IFlameEffect* effect = nullptr;         // +0x50
};

class cSC4FireOccupant : public cSC4Occupant {
public:
    cSC4FireOccupant();
    ~cSC4FireOccupant() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    bool Init() override;
    bool Shutdown() override;
    void GetPosition(float* xyz) override;
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* minXyz, float* maxXyz) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4FireOccupant; }

    void SetHosed(int delta);                         // the hose level never goes below zero
    bool SetTarget(cISC4Occupant* target);
    void SetStage(std::uint32_t stage);
    void CreateFlames();
    void ViewUpdate();
    std::uint32_t Extinguish(std::uint32_t amount);
    void SetVisible(bool visible);
    cSC4Occupant* AsOccupant() { return this; }
    cISC4Occupant* GetTarget() const { return target_; }
    std::uint32_t GetStage() const { return stage_; }
    std::uint32_t GetMaxStage() const { return maxStage_; }
    std::uint32_t GetCounter() const { return counter_; }
    void SetCounter(std::uint32_t counter) { counter_ = counter; }
    std::uint32_t GetHosed() const { return hosed_; }
    bool IsExtinguishing() const { return extinguish_ != 0; }   // +0x80: the deadline Extinguish() stored
    std::uint32_t GetFamily() const { return family_; }
    void SetFamily(std::uint32_t family) { family_ = family; }
    const std::vector<cSC4Flame*>& Flames() const { return flames_; }
    std::uint32_t TopFlameCount() const { return topFlames_; }
    std::uint32_t SideFlameCount() const { return sideFlames_; }

    static cSC4ResponseCurve* mpTopFireCurve;         // footprint area -> top flames
    static cSC4ResponseCurve* mpSideFireCurve;        // wall area -> side flames

private:
    void ReleaseFlame(cSC4Flame* flame);

    std::uint32_t topFlames_ = 0;       // +0x68
    std::uint32_t sideFlames_ = 0;      // +0x6c
    std::uint32_t counter_ = 0;         // +0x70
    std::uint32_t stage_ = 0xffffffff;  // +0x74
    std::uint32_t maxStage_ = 0;        // +0x78
    std::uint32_t hosed_ = 0;           // +0x7c
    std::uint32_t extinguish_ = 0;      // +0x80
    cISC4Occupant* target_ = nullptr;   // +0x88
    std::vector<cSC4Flame*> flames_;    // +0x90
    bool fromModelPoints_ = false;      // +0xa8
    std::uint32_t family_ = 0;          // +0xac
};

}  // namespace sc4

#include "sc4/SimFireCrew.h"

#include <cfloat>
#include <cmath>

#include "rz/cRZMessage2Standard.h"
#include "s3d/S3DMath.h"

namespace sc4 {

namespace {
ISimFireCrewHost* g_host = nullptr;

constexpr std::uint32_t kCrewMessageOccupantRemoved = 0x99ef1143, kCrewToxicIID = 0xabf98cb3;
constexpr std::uint32_t kModelType = 0x5ad0e817, kModelGroup = 0xbadb57f1;
constexpr std::uint32_t kSimFireCrewModel[3] = {0x21010000, 0x21010000, 0x21030000};       // 0x0084c2c4
constexpr std::uint32_t kSimFireDirectorModel[3] = {0x21020000, 0x21020000, 0x21040000};   // 0x0084c2b8
constexpr std::uint32_t kSimCallForHelpModel[3] = {0x21050000, 0x21060000, 0x21070000};    // 0x0084c2a0
constexpr std::uint32_t kCompetenceSoundIDs[3] = {0x29de8fd6, 0xc9de8ee4, 0xc9de8ee4};     // 0x0084c2ac
constexpr const char* kWaterStreamEffectName[3] = {"water_stream", "water_stream", "water_sputter"};
constexpr const char* kFoamStreamEffectName[3] = {"foam_stream", "foam_stream", "foam_sputter"};
constexpr const char* kWaterSplashEffectName = "water_splash";
constexpr const char* kFoamSplashEffectName = "foam_splash";
constexpr const char* kHoseRegName = "regpoint01";
constexpr float kOrientationEpsilon = 1.0e-5f;       // cSC4TrafficAutomaton::kfAutomatonOrientationEpsilon
constexpr float kHalfPi = 1.5707964f, kQuarterPi = 0.7853982f;

cS3DVector3 Sub(const cS3DVector3& a, const cS3DVector3& b) { return cS3DVector3{a.x - b.x, a.y - b.y, a.z - b.z}; }
float Length(const cS3DVector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
cS3DVector3 Normalized(const cS3DVector3& v) {
    const float length = Length(v);
    return length > 0.0f ? cS3DVector3{v.x / length, v.y / length, v.z / length} : v;
}
// cS3DMatrix3::RotateY applied to a direction.
cS3DVector3 RotatedY(const cS3DVector3& v, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return cS3DVector3{c * v.x + s * v.z, v.y, c * v.z - s * v.x};
}
void MakeIdentity(cS3DTransform& t) {
    t = cS3DTransform{};
}
}  // namespace

ISimFireCrewHost* SimFireCrewHost() { return g_host; }
void SetSimFireCrewHost(ISimFireCrewHost* host) { g_host = host; }

// @ 0x004907d0
bool cSC4ActiveToxicOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (!occupant || occupant->GetType() != kOccupantTypeToxic) return false;
    cSC4ToxicOccupant* spill = nullptr;
    if (!occupant->QueryInterface(kCrewToxicIID, reinterpret_cast<void**>(&spill)) || !spill) return false;
    const bool active = spill->IsActive();
    spill->Release();
    return active;
}

bool cSC4CrewFireOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (!occupant || occupant->GetType() != kOccupantTypeFire) return false;
    cSC4FireOccupant* fire = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4FireOccupant, reinterpret_cast<void**>(&fire)) || !fire) return false;
    const bool burning = fire->GetTarget() != nullptr;
    fire->Release();
    return burning;
}

cSC4SimFireCrew::cSC4SimFireCrew(cSC4FireTruck* truck, IPedestrianHost* host) : cSC4Pedestrian(host, cGZPersistResourceKey{}), truck_(truck) {
    if (truck_) truck_->AddRef();
    fireFilter_ = new cSC4CrewFireOccupantFilter();
    fireFilter_->AddRef();
    spillFilter_ = new cSC4ActiveToxicOccupantFilter();
    spillFilter_->AddRef();
}

cSC4SimFireCrew::~cSC4SimFireCrew() {
    if (stream_) stream_->Release();
    if (splash_) splash_->Release();
    if (closestFire_) closestFire_->Release();
    if (closestSpill_) closestSpill_->Release();
    if (fireFilter_) fireFilter_->Release();
    if (spillFilter_) spillFilter_->Release();
    if (truck_) truck_->Release();
}

void cSC4SimFireCrew::StopEffect(ICrewEffect* effect, int transition) {
    if (effect && effect->IsStarted()) effect->Stop(transition);
}

void cSC4SimFireCrew::HideEffects() {
    StopEffect(stream_, 1);
    StopEffect(splash_, 1);
}

// The old target loses this crew's hose, the new one gains it.
void cSC4SimFireCrew::SetClosestFire(cSC4FireOccupant* fire, bool aimed) {
    const int strength = HoseStrength(aimed);
    if (closestFire_) closestFire_->SetHosed(-strength);
    if (fire) fire->AddRef();
    if (closestFire_) closestFire_->Release();
    closestFire_ = fire;
    if (closestFire_) closestFire_->SetHosed(strength);
}

void cSC4SimFireCrew::SetClosestToxicSpill(cSC4ToxicOccupant* spill, bool aimed) {
    const int strength = HoseStrength(aimed);
    if (closestSpill_) closestSpill_->SetHosed(-strength);
    if (spill) spill->AddRef();
    if (closestSpill_) closestSpill_->Release();
    closestSpill_ = spill;
    if (closestSpill_) closestSpill_->SetHosed(strength);
}

void cSC4SimFireCrew::SendFireCrewMessage(std::uint32_t type, cISC4Occupant* occupant, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3,
                                          std::uint32_t d4) {
    if (!Host()) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetIGZUnknown(occupant);
    message->SetData1(d1);
    message->SetData2(d2);
    message->SetData3(d3);
    message->SetData4(d4);
    Host()->PostMessage(message);
    message->Release();
}

// A figure calling for help keeps facing the camera; the hose team re-aims at once.
void cSC4SimFireCrew::SetZoomAndRotation(int zoom, int rotation) {
    cSC4Automaton::SetZoomAndRotation(zoom, rotation);
    if (!truck_) return;
    if (truck_->GetDispatchState() == 1) {
        if (!g_host) return;
        float view[3] = {};
        g_host->ViewDirection(view);
        const float facing[3] = {-view[0], -view[1], -view[2]};
        MoveTo(nullptr, facing);
    } else {
        DoAnimationTick(0, 0.0f);
    }
}

bool cSC4SimFireCrew::DoMessage(cIGZMessage2* message) {
    if (message->GetType() != kCrewMessageOccupantRemoved) return cSC4Automaton::DoMessage(message);
    void* removed = static_cast<cIGZMessage2Standard*>(message)->GetVoid1();
    if ((closestFire_ && static_cast<cISC4Occupant*>(closestFire_) == removed) ||
        (closestSpill_ && static_cast<cISC4Occupant*>(closestSpill_) == removed)) {
        SetClosestFire(nullptr, false);
        SetClosestToxicSpill(nullptr, false);
        StopEffect(stream_, 1);
        StopEffect(splash_, 0);
    }
    return true;
}

void cSC4SimFireCrew::ClampToCityBounds(cS3DVector3& point) {
    int countX = 0, countZ = 0;
    if (!g_host || !g_host->CityCells(countX, countZ)) return;
    const float sizeX = static_cast<float>(countX << 4), sizeZ = static_cast<float>(countZ << 4);
    if (point.x < 0.0f) point.x = 0.0f;
    else if (sizeX <= point.x) point.x = sizeX - 1.0f;
    if (point.z < 0.0f) point.z = 0.0f;
    else if (sizeZ <= point.z) point.z = sizeZ - 1.0f;
}

bool cSC4SimFireCrew::Shutdown() {
    if (IsInitialized()) {
        SendFireCrewMessage(kMessageFireCrewStatus, this, 0, 0, 0, 0);
        if (stream_) {
            stream_->Stop(1);
            stream_->Release();
            stream_ = nullptr;
        }
        if (splash_) {
            splash_->Stop(1);
            splash_->Release();
            splash_ = nullptr;
        }
        SetClosestFire(nullptr, false);
        SetClosestToxicSpill(nullptr, false);
        if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) server->RemoveNotification(this, kCrewMessageOccupantRemoved);
        if (fireFilter_) { fireFilter_->Release(); fireFilter_ = nullptr; }
        if (spillFilter_) { spillFilter_->Release(); spillFilter_ = nullptr; }
    }
    return cSC4Pedestrian::Shutdown();
}

// Where the water leaves the hose: the crew model's place plus its hose registration point of the current frame.
void cSC4SimFireCrew::GetWaterHoseTransform(cS3DTransform& out) {
    MakeIdentity(out);
    if (Models().empty()) return;
    IAutomatonModel* model = Models()[0];
    const int frame = model->GetFrame();
    const int count = static_cast<int>(hosePoints_.size());
    cS3DTransform place;
    model->GetTransform(place);
    out.translation[0] += place.translation[0];
    out.translation[1] += place.translation[1];
    out.translation[2] += place.translation[2];
    out.flags |= 1;
    ++out.changeCounter;
    if (count == 0) return;
    const cS3DVector3 point = hosePoints_[frame < count ? frame : count - 1];
    // the point turned by the model's rotation (the translation cleared)
    const float px = place.rotation[0][0] * point.x + place.rotation[1][0] * point.y + place.rotation[2][0] * point.z;
    const float py = place.rotation[0][1] * point.x + place.rotation[1][1] * point.y + place.rotation[2][1] * point.z;
    const float pz = place.rotation[0][2] * point.x + place.rotation[1][2] * point.y + place.rotation[2][2] * point.z;
    const bool rotated = (place.flags & 4) != 0;
    out.translation[0] += rotated ? px : point.x;
    out.translation[1] += rotated ? py : point.y;
    out.translation[2] += rotated ? pz : point.z;
    ++out.changeCounter;
}

bool cSC4SimFireCrew::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    if (cSC4Pedestrian::Write(stream)) {
        SC4WriteGZSerializable(stream, truck_ ? static_cast<cISC4Occupant*>(truck_) : nullptr);
        stream.SetFloat32(competence_);
        stream.SetUint8(level_);
        stream.SetSint32(foundCount_);
        stream << reportedDone_;
        stream << target_;
        stream.SetFloat32(streamTime_);
        stream << wander_;
    }
    return !stream.GetError();
}

bool cSC4SimFireCrew::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (cSC4Pedestrian::Read(stream)) {
        cSC4Automaton* loaded = nullptr;
        SC4ReadGZSerializable(stream, 0xa9b40f05, reinterpret_cast<void**>(&loaded), false);
        cSC4FireTruck* truck = dynamic_cast<cSC4FireTruck*>(loaded);
        if (truck_) truck_->Release();
        truck_ = truck;
        if (!truck && loaded) loaded->Release();
        stream.GetFloat32(competence_);
        stream.GetUint8(level_);
        stream.GetSint32(foundCount_);
        stream >> reportedDone_;
        stream >> target_;
        stream.GetFloat32(streamTime_);
        stream >> wander_;
    }
    return !stream.GetError();
}

void cSC4SimFireCrew::InitRegTranslationArray() {
    if (Models().empty() || !g_host) return;
    std::vector<cS3DVector3> points;
    if (g_host->RegPointTranslations(Models()[0], kHoseRegName, points)) hosePoints_ = points;
}

// At the scene the crew gets the models of its competence level (the hose team and the director); a blocked engine's figure
// is one of the three calling-for-help animations.
bool cSC4SimFireCrew::Init() {
    if (IsInitialized()) return true;
    bool hoseTeam = false;
    if (truck_ && truck_->GetDispatchState() == 5) {
        level_ = 0;
        while (level_ <= 2) {
            if (g_host && g_host->CompetenceThreshold(level_) <= competence_) {
                if (level_ == 2) SendFireCrewMessage(kMessageFireCrewStatus, this, 0xffffffffu, 0x40, 0, 0);
                break;
            }
            ++level_;
        }
        if (level_ > 2) level_ = 2;   // the original indexes its three-entry tables with the loop's end value
        AddModelResourceKey(cGZPersistResourceKey{kModelType, kModelGroup, kSimFireCrewModel[level_]});
        AddModelResourceKey(cGZPersistResourceKey{kModelType, kModelGroup, kSimFireDirectorModel[level_]});
        hoseTeam = true;
    } else {
        const std::uint32_t pick = g_host ? g_host->RandomBelow(3) : 0;
        AddModelResourceKey(cGZPersistResourceKey{kModelType, kModelGroup, kSimCallForHelpModel[pick]});
    }
    SetAutomataFlag(0x2000, true);
    SetAutomataFlag(0x4000, true);
    const bool result = cSC4Pedestrian::Init();
    if (hoseTeam) InitRegTranslationArray();
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) server->AddNotification(this, kCrewMessageOccupantRemoved);
    return result;
}

// Places the crew. At the scene: the hose team a little ahead of the engine towards the call, the director off to the side;
// the first aim is the call's position. A figure calling for help stands beside the blocked engine, facing the camera.
void cSC4SimFireCrew::SetupFireCrew() {
    if (!truck_ || !g_host || !Host()) return;
    cS3DVector3 truckPosition;
    truck_->GetPosition(&truckPosition.x);
    if (truck_->GetDispatchState() == 5) {
        cS3DVector3 direction, target;
        if (truck_->FindState(kStatePlayerDrive) == nullptr) {
            target = truck_->GetDispatchDestination();
            direction = Sub(target, truckPosition);
            if (Length(direction) <= kOrientationEpsilon) truck_->GetOrientation(&direction.x);
            else direction = Normalized(direction);
        } else {
            truck_->GetOrientation(&direction.x);
            direction = RotatedY(direction, g_host->RandomUint32() < 0x7fffffffu ? kHalfPi : -kHalfPi);
            target.x = 16.0f * direction.x * 3.0f + truckPosition.x;
            target.y = direction.y * 16.0f * 3.0f + truckPosition.y;
            target.z = direction.z * 16.0f * 3.0f + truckPosition.z;
        }
        const float crewDistance = g_host->CrewDistance();
        cS3DVector3 crew{crewDistance * direction.x + truckPosition.x, crewDistance * direction.y + truckPosition.y,
                         crewDistance * direction.z + truckPosition.z};
        ClampToCityBounds(crew);
        crew.y = Host()->RoadAltitude(crew.x, crew.z);
        MoveTo(&crew.x, &direction.x);
        direction = RotatedY(direction, kQuarterPi);
        const float directorDistance = g_host->DirectorDistance();
        cS3DVector3 director{directorDistance * direction.x + truckPosition.x, directorDistance * direction.y + truckPosition.y,
                             directorDistance * direction.z + truckPosition.z};
        ClampToCityBounds(director);
        director.y = Host()->RoadAltitude(director.x, director.z);
        if (Models().size() > 1) {
            IAutomatonModel* model = Models()[1];
            model->AddRef();
            cS3DTransform place;
            model->GetTransform(place);
            nS3D::Matrix3 facing;
            nS3D::AlignZToDirWithYUp(direction, facing);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) place.rotation[r][c] = facing.m[3 * r + c];
            place.translation[0] = director.x;
            place.translation[1] = director.y;
            place.translation[2] = director.z;
            place.flags |= 5;
            ++place.changeCounter;
            model->SetTransform(place);
            g_host->MoveModelInRenderer(model);
            model->Release();
        }
        target_ = target;
        wander_ = target_;
        SendFireCrewMessage(kMessageFireCrewHoseOn, truck_, kCompetenceSoundIDs[level_], 0, 0, 0);
    } else {
        // PARTIAL: the original moves the figure to the engine's other side when this one is off the engine's network lot.
        cS3DTransform place;
        truck_->GetTransform(place);
        cS3DVector3 side{0.0f, 0.0f, -1.0f};
        if (place.flags & 4) {
            side = cS3DVector3{place.rotation[2][0] * -1.0f, place.rotation[2][1] * -1.0f, place.rotation[2][2] * -1.0f};
        }
        side = RotatedY(side, kHalfPi);
        const float distance = g_host->DirectorDistance();
        cS3DVector3 spot{distance * side.x + truckPosition.x, distance * side.y + truckPosition.y, distance * side.z + truckPosition.z};
        ClampToCityBounds(spot);
        spot.y = Host()->RoadAltitude(spot.x, spot.z);
        float view[3] = {};
        g_host->ViewDirection(view);
        const float facing[3] = {-view[0], -view[1], -view[2]};
        MoveTo(&spot.x, facing);
    }
    SetOpacity(0.0f, 0.0f);
    SetOpacity(1.0f, Host()->FadeDurationSeconds());
}

// The nearest fire (then spill) within the hose radius of `point` (the player's aim) or of the crew; without an aim only what
// lies ahead, towards the engine's call. A target the crew already has is kept while it lasts.
bool cSC4SimFireCrew::FindTargetsInRadius(const cS3DVector3* point) {
    const bool aimed = point != nullptr;
    if (closestFire_ && closestFire_->GetTarget() == nullptr) SetClosestFire(nullptr, false);
    if (closestSpill_ && !closestSpill_->IsActive()) SetClosestToxicSpill(nullptr, false);
    if (closestFire_ || closestSpill_) return true;
    if (!g_host || !truck_) return false;
    const float radius = g_host->HoseRadius();
    cS3DVector3 origin;
    if (aimed) origin = *point;
    else GetPosition(&origin.x);
    const float boxMin[3] = {origin.x - radius, -FLT_MAX, origin.z - radius};
    const float boxMax[3] = {radius + origin.x, FLT_MAX, radius + origin.z};
    const cS3DVector3 ahead = Normalized(Sub(truck_->GetDispatchDestination(), origin));
    cISC4Occupant* chosen = nullptr;
    bool found = false;
    float best = FLT_MAX;
    auto consider = [&](cISC4Occupant* occupant) {
        cS3DVector3 position;
        occupant->GetPosition(&position.x);
        cS3DVector3 to = Sub(position, origin);
        const float distance = Length(to);
        if (!(distance < best)) return;
        to = Normalized(to);
        if (aimed || 0.0f < to.x * ahead.x + to.y * ahead.y + to.z * ahead.z) {
            occupant->AddRef();
            if (chosen) chosen->Release();
            chosen = occupant;
            found = true;
            best = distance;
        }
    };
    OccupantList list;
    if (g_host->OccupantsInBox(boxMin, boxMax, fireFilter_, list)) {
        for (cISC4Occupant* occupant : list) {
            cSC4FireOccupant* fire = nullptr;
            const bool burning = occupant->QueryInterface(kGZIID_cISC4FireOccupant, reinterpret_cast<void**>(&fire)) && fire &&
                                 fire->GetTarget() != nullptr;
            if (fire) fire->Release();
            if (burning) consider(occupant);
        }
        for (cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
    if (!chosen && g_host->OccupantsInBox(boxMin, boxMax, spillFilter_, list)) {
        for (cISC4Occupant* occupant : list) {
            cSC4ToxicOccupant* spill = nullptr;
            const bool active = occupant->QueryInterface(kCrewToxicIID, reinterpret_cast<void**>(&spill)) && spill &&
                                spill->IsActive();
            if (spill) spill->Release();
            if (active) consider(occupant);
        }
        for (cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
    if (!chosen) {
        SetClosestFire(nullptr, false);
        SetClosestToxicSpill(nullptr, false);
    } else {
        cSC4ToxicOccupant* spill = nullptr;
        if (chosen->QueryInterface(kCrewToxicIID, reinterpret_cast<void**>(&spill)) && spill) {
            SetClosestFire(nullptr, false);
            SetClosestToxicSpill(spill, aimed);
            spill->Release();
        } else {
            cSC4FireOccupant* fire = nullptr;
            const bool isFire = chosen->QueryInterface(kGZIID_cISC4FireOccupant, reinterpret_cast<void**>(&fire)) && fire;
            SetClosestToxicSpill(nullptr, false);
            SetClosestFire(isFire ? fire : nullptr, isFire && aimed);
            if (fire) fire->Release();
        }
        chosen->Release();
    }
    if (closestFire_ || closestSpill_) ++foundCount_;
    return found;
}

bool cSC4SimFireCrew::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    const bool result = cSC4Pedestrian::DoAnimationTick(unknown, milliseconds);
    if (!truck_) return result;
    const bool playerDriving = truck_->FindState(kStatePlayerDrive) != nullptr;
    bool working = false;
    if (CheckAutomataFlag(1) || (!playerDriving && truck_->GetDispatchState() != 5)) {
        SendFireCrewMessage(kMessageFireCrewHoseOff, truck_, kCompetenceSoundIDs[level_ < 3 ? level_ : 2], 0, 0, 0);
        StopEffect(stream_, 1);
        StopEffect(splash_, 1);
    } else {
        cSC4FireOccupant* fireBefore = closestFire_;
        cSC4ToxicOccupant* spillBefore = closestSpill_;
        cS3DVector3 aim;
        const bool aimed = truck_->GetStreamTargetPoint(aim);
        const bool found = FindTargetsInRadius(aimed ? &aim : nullptr);
        if (!aimed && (!found || playerDriving)) {
            if (stream_ && stream_->IsStarted()) {
                stream_->Stop(1);
                streamTime_ = 0.0f;
            }
            StopEffect(splash_, 1);
            if (foundCount_ > 0 && !reportedDone_) {
                SendFireCrewMessage(kMessageFireCrewStatus, this, 1, 0x50, 0, 0);
                reportedDone_ = true;
            }
            truck_->HandleCrewFinished();
        } else {
            working = true;
            cS3DTransform hose;
            GetWaterHoseTransform(hose);
            cS3DVector3 target = target_;
            if (aimed) {
                target = aim;
            } else if (closestSpill_) {
                closestSpill_->GetPosition(&target.x);
            } else if (closestFire_) {
                closestFire_->GetPosition(&target.x);
            }
            target_ = target;
            if (aimed || (!playerDriving && fireBefore != closestFire_) || spillBefore != closestSpill_) {
                // the hose team turns to its target
                const cS3DVector3 back = Normalized(Sub(Position(), target));
                const float yaw = std::atan2(-back.x, -back.z);   // SC4ViewUtilities::GetYawAngle2D
                if (!Models().empty()) {
                    IAutomatonModel* model = Models()[0];
                    cS3DTransform place;
                    model->GetTransform(place);
                    const float c = std::cos(yaw), s = std::sin(yaw);
                    place.rotation[0][0] = c;  place.rotation[0][1] = 0.0f; place.rotation[0][2] = -s;
                    place.rotation[1][0] = 0.0f; place.rotation[1][1] = 1.0f; place.rotation[1][2] = 0.0f;
                    place.rotation[2][0] = s;  place.rotation[2][1] = 0.0f; place.rotation[2][2] = c;
                    place.flags |= 4;
                    ++place.changeCounter;
                    model->SetTransform(place);
                }
                if (!stream_ && g_host) {
                    const std::uint8_t level = level_ < 3 ? level_ : 2;
                    stream_ = g_host->CreateEffect(closestSpill_ ? kFoamStreamEffectName[level] : kWaterStreamEffectName[level]);
                    splash_ = g_host->CreateEffect(closestSpill_ ? kFoamSplashEffectName : kWaterSplashEffectName);
                }
            }
            if (stream_) {
                const cS3DVector3 hosePoint{hose.translation[0], hose.translation[1], hose.translation[2]};
                hose.scale = Length(Sub(target, hosePoint)) * 0.25f;
                if (hose.scale != 1.0f) hose.flags |= 2;
                else hose.flags &= static_cast<std::uint8_t>(~2);
                ++hose.changeCounter;
                stream_->SetSourceTransform(hose);
                if (level_ == 2) {
                    // the clumsy crew's stream drifts around the crew
                    cS3DVector3 drift;
                    GetPosition(&drift.x);
                    drift.x += static_cast<float>(g_host->RandomRange(-75.0, 75.0));
                    drift.z += static_cast<float>(g_host->RandomRange(-75.0, 75.0));
                    const float step = milliseconds / 500.0f;
                    target.x = (drift.x - wander_.x) * step + wander_.x;
                    target.y = (drift.y - wander_.y) * step + wander_.y;
                    target.z = (drift.z - wander_.z) * step + wander_.z;
                    wander_ = target;
                } else {
                    target.x = (hosePoint.x - Position().x) + target.x;
                    target.y = (hosePoint.y - Position().y) + target.y;
                    target.z = (hosePoint.z - Position().z) + target.z;
                }
                ClampToCityBounds(target);
                stream_->SetTarget(target);
                if (splash_) {
                    cS3DTransform landing;
                    MakeIdentity(landing);
                    landing.translation[0] = target.x;
                    landing.translation[1] = target.y;
                    landing.translation[2] = target.z;
                    landing.flags |= 1;
                    ++landing.changeCounter;
                    splash_->SetSourceTransform(landing);
                }
                if (!stream_->IsStarted()) {
                    stream_->Start(0);
                    streamTime_ = 0.0f;
                } else {
                    streamTime_ += milliseconds / 1000.0f;
                }
                if (splash_ && 0.8f <= streamTime_ && !splash_->IsStarted()) splash_->Start(0);
            }
        }
    }
    (void)working;
    // A figure calling for help starts another of its animations when one has played through.
    if (truck_ && !CheckAutomataFlag(1) && truck_->GetDispatchState() == 1 && !Models().empty() && g_host) {
        IAutomatonModel* model = Models()[0];
        if (model->GetFrameCount() - 1 <= model->GetFrame()) {
            const cGZPersistResourceKey key{kModelType, kModelGroup, kSimCallForHelpModel[g_host->RandomBelow(3)]};
            RemoveModelInstance(model);
            AddModelInstance(key);
        }
    }
    return result;
}

}  // namespace sc4

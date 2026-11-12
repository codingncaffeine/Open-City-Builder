#include "sc4/RiotPolice.h"

#include <cmath>

#include "rz/cRZMessage2Standard.h"
#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
IRiotPoliceHost* g_host = nullptr;

constexpr std::uint32_t kCallForHelpType = 0x5ad0e817, kCallForHelpGroup = 0xbadb57f1;
constexpr std::uint32_t kRiotPoliceType = 0x29a5d1ec, kRiotPoliceGroup = 0x49a593e7;
// 0x0084c220: six sims, three animations each
constexpr std::uint32_t kSimCallForHelpResourceInstance[18] = {
    0x02301e00, 0x02301f00, 0x02302000, 0x02601e00, 0x02601f00, 0x02602000, 0x02401e00, 0x02401f00, 0x02402000,
    0x02701e00, 0x02701f00, 0x02702000, 0x02501e00, 0x02501f00, 0x02502000, 0x02801e00, 0x02801f00, 0x02802000};
// 0x0084c268 and 0x0084c274: three entries each; index 3 is the word that follows the table in the original's data
constexpr std::uint32_t kCompetenceSoundIDs[4] = {0x29de8fd6, 0xc9de8ee4, 0xc9de8ee4, 0x04600000};
constexpr std::uint32_t kRiotPoliceModelResourceInstance[4] = {0x04600000, 0x04600000, 0x07300000, 0x80000000};
constexpr float kOrientationEpsilon = 1.0e-5f;       // cSC4TrafficAutomaton::kfAutomatonOrientationEpsilon
constexpr float kMaxStoplightDistance = 1.0f;        // cSC4TrafficAutomaton::kfMaxStoplightDistance (0x00a706d8)
constexpr float kHalfPi = 1.5707964f;

float Length(const cS3DVector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
// cS3DMatrix3::RotateY applied to a direction.
cS3DVector3 RotatedY(const cS3DVector3& v, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return cS3DVector3{c * v.x + s * v.z, v.y, c * v.z - s * v.x};
}
}  // namespace

IRiotPoliceHost* RiotPoliceHost() { return g_host; }
void SetRiotPoliceHost(IRiotPoliceHost* host) { g_host = host; }

cSC4RiotPolice::cSC4RiotPolice(cSC4PatrolCar* car, IPedestrianHost* host) : cSC4Pedestrian(host, cGZPersistResourceKey{}), car_(car) {
    if (car_) car_->AddRef();
}

cSC4RiotPolice::~cSC4RiotPolice() {
    if (car_) car_->Release();
}

void cSC4RiotPolice::SendRiotPoliceMessage(std::uint32_t type, cISC4Occupant* occupant, std::uint32_t d1, std::uint32_t d2,
                                           std::uint32_t d3, std::uint32_t d4) {
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

bool cSC4RiotPolice::Shutdown() {
    if (IsInitialized()) SendRiotPoliceMessage(kMessageRiotPoliceStatus, this, 0, 0, 0, 0);
    return cSC4Pedestrian::Shutdown();
}

void cSC4RiotPolice::ClampToCityBounds(cS3DVector3& point) {
    int countX = 0, countZ = 0;
    if (!g_host || !g_host->CityCells(countX, countZ)) return;
    const float sizeX = static_cast<float>(countX << 4), sizeZ = static_cast<float>(countZ << 4);
    if (point.x < 0.0f) point.x = 0.0f;
    else if (sizeX <= point.x) point.x = sizeX - kMaxStoplightDistance;
    if (point.z < 0.0f) point.z = 0.0f;
    else if (sizeZ <= point.z) point.z = sizeZ - kMaxStoplightDistance;
}

// The model by what the car is doing: a sim calling for help beside a blocked car, the squad of the station's competence at a
// call. Any other dispatch state leaves the figure without a model. The model's exemplar becomes the figure's own.
bool cSC4RiotPolice::Init() {
    if (IsInitialized()) return true;
    cGZPersistResourceKey key{};
    const std::uint32_t state = car_ ? car_->GetDispatchState() : 0;
    if (state == 1) {
        figure_ = static_cast<std::uint8_t>(RandomBelow(6));
        const std::uint32_t animation = RandomBelow(3);
        key = cGZPersistResourceKey{kCallForHelpType, kCallForHelpGroup, kSimCallForHelpResourceInstance[figure_ * 3 + animation]};
        AddModelResourceKey(key);
    } else if (state == 5) {
        level_ = 0;
        while (level_ <= 2) {
            if (g_host && g_host->CompetenceThreshold(level_) <= competence_) {
                if (level_ == 2) SendRiotPoliceMessage(kMessageRiotPoliceStatus, this, 0xffffffffu, 0x40, 0, 0);
                break;
            }
            ++level_;
        }
        key = cGZPersistResourceKey{kRiotPoliceType, kRiotPoliceGroup, kRiotPoliceModelResourceInstance[level_]};
        AddModelResourceKey(key);
    }
    if (g_host) g_host->AddController(this, kRiotPoliceController);
    SetAutomataFlag(0x2000, true);
    SetAutomataFlag(0x4000, true);
    if (key.instance != 0 && g_host && GetTrafficAutomataManager()) {
        if (cISCResExemplar* exemplar = g_host->ModelExemplar(key)) SetDefaultExemplar(exemplar);
    }
    return cSC4Pedestrian::Init();
}

// A sim calling for help starts another of its three animations when one ends. A figure being removed tells the audio that a
// squad is no longer at work, unless its car is still at the call.
bool cSC4RiotPolice::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    const bool result = cSC4Pedestrian::DoAnimationTick(unknown, milliseconds);
    if (!car_) return result;
    if (!CheckAutomataFlag(1)) {
        if (car_->GetDispatchState() == 1 && !Models().empty()) {
            IAutomatonModel* model = Models()[0];
            if (model->GetFrameCount() - 1 <= model->GetFrame()) {
                const std::uint32_t animation = RandomBelow(3);
                PlayAnim(cGZPersistResourceKey{kCallForHelpType, kCallForHelpGroup, kSimCallForHelpResourceInstance[figure_ * 3 + animation]});
            }
        }
    } else if (car_->GetDispatchState() != 5) {
        SendRiotPoliceMessage(kMessageRiotPoliceOff, car_, kCompetenceSoundIDs[level_ < 4 ? level_ : 3], 0, 0, 0);
    }
    return result;
}

// A sim calling for help keeps facing the camera.
void cSC4RiotPolice::SetZoomAndRotation(int zoom, int rotation) {
    cSC4Automaton::SetZoomAndRotation(zoom, rotation);
    if (!car_ || !g_host || car_->GetDispatchState() != 1) return;
    float view[3] = {};
    g_host->ViewDirection(view);
    const float facing[3] = {-view[0], -view[1], -view[2]};
    MoveTo(nullptr, facing);
}

// Places the figure and fades it in. Calling for help: six meters to the side of the blocked car, on the side that is still on
// the car's own road tile, facing the camera; a car that is on no tile gets no figure. The squad: eight meters ahead of the car
// towards the call (the way the car faces when it stands on the call), facing it.
void cSC4RiotPolice::SetupRiotPolice() {
    if (!car_ || !g_host || !Host()) return;
    const std::uint32_t state = car_->GetDispatchState();
    cS3DVector3 carPosition;
    car_->GetPosition(&carPosition.x);
    if (state == 1) {
        cS3DTransform place;
        car_->GetTransform(place);
        cS3DVector3 side{0.0f, 0.0f, -1.0f};
        if (place.flags & 4) side = cS3DVector3{place.rotation[2][0] * -1.0f, place.rotation[2][1] * -1.0f, place.rotation[2][2] * -1.0f};
        side = RotatedY(side, kHalfPi);
        cS3DVector3 spot{kRiotPoliceCallForHelpDistance * side.x + carPosition.x, side.y * kRiotPoliceCallForHelpDistance + carPosition.y,
                         side.z * kRiotPoliceCallForHelpDistance + carPosition.z};
        ITrafficAutomatonOccupant* tile = car_->GetNetworkOccupant();
        if (!tile) {
            Remove(true);
        } else {
            SC4CellRect cells;
            tile->AsOccupant()->GetBoundingCityCells(cells);
            int cellX = 0, cellZ = 0;
            g_host->PositionToCell(spot.x, spot.z, cellX, cellZ);
            if (cellX < cells.x0 || cells.x1 < cellX || cellZ < cells.z0 || cells.z1 < cellZ) {
                side = cS3DVector3{-side.x, -side.y, -side.z};
                spot = cS3DVector3{side.x * kRiotPoliceCallForHelpDistance + carPosition.x, side.y * kRiotPoliceCallForHelpDistance + carPosition.y,
                                   side.z * kRiotPoliceCallForHelpDistance + carPosition.z};
            }
            ClampToCityBounds(spot);
            spot.y = Host()->RoadAltitude(spot.x, spot.z);
            float view[3] = {};
            g_host->ViewDirection(view);
            const float facing[3] = {-view[0], -view[1], -view[2]};
            MoveTo(&spot.x, facing);
        }
    } else if (state == 5) {
        const cS3DVector3 target = car_->GetDispatchDestination();
        cS3DVector3 direction{target.x - carPosition.x, target.y - carPosition.y, target.z - carPosition.z};
        const float length = Length(direction);
        if (length <= kOrientationEpsilon) car_->GetOrientation(&direction.x);
        else direction = cS3DVector3{direction.x / length, direction.y / length, direction.z / length};
        cS3DVector3 spot{kRiotPoliceSquadDistance * direction.x + carPosition.x, direction.y * kRiotPoliceSquadDistance + carPosition.y,
                         direction.z * kRiotPoliceSquadDistance + carPosition.z};
        ClampToCityBounds(spot);
        spot.y = Host()->RoadAltitude(spot.x, spot.z);
        MoveTo(&spot.x, &direction.x);
        SendRiotPoliceMessage(kMessageRiotPoliceOn, car_, kCompetenceSoundIDs[level_ < 4 ? level_ : 3], 0, 0, 0);
    }
    SetOpacity(0.0f, 0.0f);
    SetOpacity(1.0f, 1.0f);
}

// Version 2: the pedestrian's record, the car, the competence, the level and a flag.
bool cSC4RiotPolice::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    if (cSC4Pedestrian::Write(stream)) {
        SC4WriteGZSerializable(stream, car_ ? static_cast<const cISC4Occupant*>(car_) : nullptr);
        stream.SetFloat32(competence_);
        stream.SetUint8(level_);
        stream << flag289_;
    }
    return !stream.GetError();
}

bool cSC4RiotPolice::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (cSC4Pedestrian::Read(stream)) {
        cSC4Automaton* loaded = nullptr;
        SC4ReadGZSerializable(stream, 0xa9b40f05, reinterpret_cast<void**>(&loaded), false);
        cSC4PatrolCar* car = dynamic_cast<cSC4PatrolCar*>(loaded);
        if (car_) car_->Release();
        car_ = car;
        if (!car && loaded) loaded->Release();
        stream.GetFloat32(competence_);
        stream.GetUint8(level_);
        stream >> flag289_;
    }
    return !stream.GetError();
}

}  // namespace sc4

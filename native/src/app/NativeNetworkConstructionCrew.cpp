// The network construction crews in a native city: the services of cSC4NetworkConstructionCrew (INetworkConstructionCrewHost):
// the network manager the crew registers with, the message server, the animation tick, the vehicle and pedestrian managers
// that make the construction vehicles and workers, the effects manager's dust, the terrain's water test and the global
// generator. The network manager calls and the kit verb `crews` are in app/NativeNetworkCrewReport.cpp.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/NativeAnimationTicks.h"
#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeEffects.h"
#include "gz/StreamOps.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/AutomataManager.h"
#include "sc4/Automaton.h"
#include "sc4/NetworkConstructionCrew.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/ServiceLocator.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/VisualEffect.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
sc4::ITrafficAutomatonOccupant* NativeTrafficPiece(sc4::cISC4Occupant* occupant);   // app/NativeTrafficNetworkMap.cpp (one reference)
void NativeRegisterNetworkCrew(sc4::cSC4NetworkConstructionCrew* crew);     // app/NativeNetworkCrewReport.cpp
void NativeUnregisterNetworkCrew(sc4::cSC4NetworkConstructionCrew* crew);

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_CREWDEBUG") != nullptr;
    return on;
}

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}

cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<cSTETerrain*>(serializable);
}

// An automaton the crew holds: the references are the automaton's; the adapter goes with the crew's last one. The manager
// hands the automaton out without a reference of the caller's (the crew takes one, as the original).
class CrewAutomaton {
public:
    explicit CrewAutomaton(cSC4Automaton* automaton) : automaton_(automaton) {}
    cSC4Automaton* Automaton() const { return automaton_; }
protected:
    void Hold() {
        automaton_->AddRef();
        ++refs_;
    }
    bool Drop() {
        automaton_->Release();
        return --refs_ <= 0;
    }
    cSC4Automaton* automaton_;
    int refs_ = 0;
};

class Vehicle final : public INetworkCrewVehicle, public CrewAutomaton {
public:
    Vehicle(cSC4Automaton* automaton, cSC4TrafficAutomaton* traffic) : CrewAutomaton(automaton), traffic_(traffic) {}
    void AddRef() override { Hold(); }
    void Release() override { if (Drop()) delete this; }
    void SetAutomataFlag(std::uint32_t flag, bool on) override { automaton_->SetAutomataFlag(flag, on); }
    void SetOpacity(float opacity, float seconds) override { automaton_->SetOpacity(opacity, seconds); }
    void GetPosition(float* xyz) override { automaton_->GetPosition(xyz); }
    void GetOrientation(float* xyz) override { automaton_->GetOrientation(xyz); }
    void MoveTo(const float* position, const float* direction) override { automaton_->MoveTo(position, direction); }
    void RemoveCallback(cIGZMessageTarget2* target) override { automaton_->RemoveCallback(target); }
    void SetPath(cSC4NetworkOccupant* piece, std::uint32_t pathKey, bool keepDestination) override {
        if (!traffic_) return;
        ITrafficAutomatonOccupant* occupant = piece ? NativeTrafficPiece(piece) : nullptr;
        traffic_->SetPath(occupant, pathKey, nullptr, keepDestination);
        if (occupant) occupant->Release();
    }
    void SetPathOffset(float offset) override { if (traffic_) traffic_->SetPathOffset(offset); }
    bool InterpolateAlongPath(float distance, const float* from, float* out) override {
        if (!traffic_) return false;
        const cS3DVector3 start{from[0], from[1], from[2]};
        cS3DVector3 end;
        const bool more = traffic_->InterpolateAlongPath(distance, start, end, nullptr, nullptr, nullptr);
        out[0] = end.x;
        out[1] = end.y;
        out[2] = end.z;
        return more;
    }
private:
    cSC4TrafficAutomaton* traffic_;
};

class Worker final : public INetworkCrewWorker, public CrewAutomaton {
public:
    using CrewAutomaton::CrewAutomaton;
    void AddRef() override { Hold(); }
    void Release() override { if (Drop()) delete this; }
    void SetAutomataFlag(std::uint32_t flag, bool on) override { automaton_->SetAutomataFlag(flag, on); }
    void SetOrientation(const float* xyz) override { automaton_->SetOrientation(xyz); }
    int ModelFrameCount() override {
        IAutomatonModel* model = automaton_->GetModelInstance();
        return model ? model->GetFrameCount() : -1;
    }
    void SetModelFrame(int frame) override {
        if (IAutomatonModel* model = automaton_->GetModelInstance()) model->SetFrame(frame);
    }
};

class Effect final : public INetworkCrewEffect {
public:
    explicit Effect(cSC4VisualEffect* effect) : effect_(effect) {}
    void Release() override {
        effect_->Release();
        delete this;
    }
    void SetSourcePosition(const float* xyz) override {
        cS3DTransform transform;
        transform.translation[0] = xyz[0];
        transform.translation[1] = xyz[1];
        transform.translation[2] = xyz[2];
        transform.flags = 1;
        effect_->SetSourceTransform(transform);
    }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
private:
    cSC4VisualEffect* effect_;
};

class CrewHost final : public INetworkConstructionCrewHost {
public:
    void RegisterCrew(cSC4NetworkConstructionCrew* crew) override {
        NativeRegisterNetworkCrew(crew);
        if (Trace()) std::fprintf(stderr, "crew %p: registered\n", static_cast<void*>(crew));
    }
    void UnregisterCrew(cSC4NetworkConstructionCrew* crew) override {
        if (Trace()) std::fprintf(stderr, "crew %p: unregistered\n", static_cast<void*>(crew));
        NativeUnregisterNetworkCrew(crew);
    }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        if (SL::spMS2) SL::spMS2->AddNotification(target, type);
    }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        if (SL::spMS2) SL::spMS2->RemoveNotification(target, type);
    }
    bool AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        cSC4AnimationTickManager* atm = AnimationTickManager();
        return atm && atm->AddTickableObject(target, priority);
    }
    bool RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        cSC4AnimationTickManager* atm = AnimationTickManager();
        return atm && atm->RemoveTickableObject(target, priority);
    }
    bool SoundSystemPresent() override { return SL::spSS != nullptr; }
    void PostConstructionMessage(std::uint32_t type, cISC4Occupant* occupant, cIGZMessageTarget2* crew, std::uint32_t data2,
                                 std::uint32_t data3) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        message->SetIGZUnknown(occupant);
        message->SetVoid1(crew);
        message->SetData2(data2);
        message->SetData3(data3);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    int CurrentViewLevel() override { return CityViewLevel(); }
    bool VehicleManagerPresent() override { return CityAs<cSC4AutomataManager>(CityComponent::VehicleManager) != nullptr; }
    INetworkCrewVehicle* CreateVehicle(std::uint32_t group, const float* position, std::uint32_t flags, const std::uint32_t key[3],
                                       std::uint8_t priority) override {
        auto* manager = CityAs<cSC4AutomataManager>(CityComponent::VehicleManager);
        if (!manager) return nullptr;
        const cS3DVector3 where{position[0], position[1], position[2]};
        IAutomataAutomaton* view = manager->CreateAutomaton(group, where, flags, cGZPersistResourceKey{key[0], key[1], key[2]}, nullptr, priority, nullptr);
        cSC4Automaton* automaton = view ? AutomatonOf(view) : nullptr;
        if (Trace()) std::fprintf(stderr, "crew: vehicle model 0x%08x at (%.1f, %.1f, %.1f) -> %p\n", key[2], position[0], position[1], position[2],
                                  static_cast<void*>(automaton));
        return automaton ? new Vehicle(automaton, dynamic_cast<cSC4TrafficAutomaton*>(automaton)) : nullptr;
    }
    void RemoveVehicle(INetworkCrewVehicle* vehicle) override {
        auto* manager = CityAs<cSC4AutomataManager>(CityComponent::VehicleManager);
        if (manager && vehicle) manager->RemoveAutomaton(AutomatonView(static_cast<Vehicle*>(vehicle)->Automaton()), false);
    }
    INetworkCrewWorker* CreateWorker(std::uint32_t group, const float* position, std::uint32_t flags) override {
        auto* manager = CityAs<cSC4AutomataManager>(CityComponent::PedestrianManager);
        if (!manager) return nullptr;
        const cS3DVector3 where{position[0], position[1], position[2]};
        IAutomataAutomaton* view = manager->CreateAutomaton(group, where, flags, cGZPersistResourceKey{0, 0, 0}, nullptr, 0, nullptr);
        cSC4Automaton* automaton = view ? AutomatonOf(view) : nullptr;
        return automaton ? new Worker(automaton) : nullptr;
    }
    void RemoveWorker(INetworkCrewWorker* worker) override {
        auto* manager = CityAs<cSC4AutomataManager>(CityComponent::PedestrianManager);
        if (manager && worker) manager->RemoveAutomaton(AutomatonView(static_cast<Worker*>(worker)->Automaton()), false);
    }
    INetworkCrewEffect* CreateEffect(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        return effect ? new Effect(effect) : nullptr;
    }
    bool CellHasWater(std::uint32_t x, std::uint32_t z, bool& terrain) override {
        cSTETerrain* t = Terrain();
        terrain = t != nullptr;
        return t && t->CellHasWater(x, z);
    }
    cRZRandomSuper* Random() override { return &random_; }   // the original draws from ::_sRandom
private:
    cRZRandomSuper random_{0x3c1du};
} g_crewHost;

}  // namespace

void InstallNetworkConstructionCrewHost() { SetNetworkConstructionCrewHost(&g_crewHost); }

}  // namespace native

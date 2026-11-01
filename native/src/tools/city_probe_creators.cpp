// city_probe_creators - probe tooling, not part of the game: the classes a real city file contains whose
// GZCOM creator is still a nullptr stub (or that are only registered in cSC4COMDirector::AddCls, not in the
// gzcom table). CreateCityProbeSerializable(classId) hands each one back as a cIGZSerializable with exactly
// one reference, constructed against the neutral stub hosts of city_probe_bindings.cpp. The probe only needs
// the object to come up and its Read() to run as far as possible; every host call answers neutrally.
//
// Two kinds of recovered class are handled here:
//  * COM-compatible: the class really derives from cIGZSerializable, so it is returned directly.
//  * "plain" serializables: the decompilation kept Write/Read/GetGZCLSID but dropped the cIGZUnknown base.
//    Those are wrapped in PlainSerializableAdapter (tools/city_probe_plain.h) so the factory can return them.
//
// Simulators whose Read/Write is not yet recovered (cSC4PowerSimulator, cSC4PlumbingSimulator, ...) are not
// constructed here: there is no serialization to run, so they stay on the probe's uncreatable list.
#include <cstdint>

#include "rz/cRZRandom.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/AutomataGenerator.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4Demand.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/CommercialSimulator.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/Demolition.h"
#include "sc4/FloraOccupant.h"
#include "sc4/IndustrialSimulator.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/OccupantManager.h"
#include "sc4/Lot.h"
#include "sc4/Ordinance.h"
#include "sc4/PropManager.h"
#include "sc4/WealthRequester.h"
#include "sc4/WeatherSimulator.h"
#include "tools/city_probe_hosts.h"
#include "rz/cRZString.h"
#include "sc4/DBSegment.h"
#include "tools/city_probe_plain.h"

namespace sc4 {
namespace {

// cSC4AutomataGenerator reaches the rest of the engine through IAutomataGeneratorHost; the probe answers
// everything neutrally (nothing to spawn, no networks, no terrain) so only its stream methods matter.
class ProbeAutomataGeneratorHost final : public IAutomataGeneratorHost {
public:
    bool BaseInit() override { return true; }
    bool BaseShutdown() override { return true; }
    void BaseActivate(bool) override {}
    void BaseDoAnimationTick(float, std::uint32_t, const ControllerTime&) override {}
    bool BaseWrite(cIGZOStream&) override { return true; }
    // cSC4AutomataController::Read (@ 0x1000f9c82): the base record of every controller. The references (the occupant and the automata)
    // are consumed; the port's generator keeps none of them.
    bool BaseRead(cIGZIStream& stream) override {
        const auto skipReference = [&stream]() {
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, kGZIID_cIGZSerializable, &raw, false);
            if (raw) static_cast<cIGZUnknown*>(raw)->Release();
        };
        std::uint16_t version = 0;
        stream.GetUint16(version);
        skipReference();                                       // the occupant
        cRZString name;
        stream.GetGZStr(name);
        float f = 0;
        for (int i = 0; i < 5; ++i) stream.GetFloat32(f);      // position (3), age, lifetime
        std::uint32_t u = 0, count = 0;
        stream.GetUint32(u);                                   // sound handle
        stream.GetUint32(u);                                   // flags
        stream.GetUint32(count);
        for (std::uint32_t i = 0; i < count && count < 1000000u && !stream.GetError(); ++i) skipReference();
        return !stream.GetError();
    }
    bool CheckFlag(std::uint32_t) override { return false; }
    void GetPosition(float out[3]) override { out[0] = out[1] = out[2] = 0; }
    void GetLotFacingPoint(float out[3]) override { out[0] = out[1] = out[2] = 0; }
    void GetOccupantBoundingBox(float outMin[3], float outMax[3]) override {
        outMin[0] = outMin[1] = outMin[2] = 0; outMax[0] = outMax[1] = outMax[2] = 0;
    }
    bool GetBoundingBoxIntersection(const float[2], const float[2], float out[2]) override {
        out[0] = out[1] = 0; return false;
    }
    const void* GetBehaviorTemplate(float, const float[3]) override { return nullptr; }
    bool HasOccupant() override { return false; }
    std::uint32_t OccupantType() override { return 0; }
    bool OccupantHasFlag(std::uint32_t) override { return false; }
    void OccupantLotCell(int& x, int& z) override { x = z = 0; }
    int OccupantFacing() override { return -1; }
    const void* Clock() override { return nullptr; }
    void ClockNow(float& time, std::int32_t& day) override { time = 0; day = 0; }
    bool IsAutomataLayerVisible() override { return false; }
    int CityCount() override { return 0; }
    float OccupantMeasureA() override { return 0; }
    float OccupantMeasureB() override { return 0; }
    bool LotMeasure(int, int, float& out) override { out = 0; return false; }
    bool NetworkCellHasType(int, int, std::uint32_t) override { return false; }
    bool NetworkPositionAccepts(const float[3], std::uint32_t) override { return false; }
    float NetworkHeight(float, float) override { return 0; }
    bool TerrainHeightEnabled() override { return false; }
    float TerrainHeight(float, float) override { return 0; }
    IAutomatonManager* ManagerForGroup(std::uint32_t) override { return nullptr; }
};
ProbeAutomataGeneratorHost g_automataGeneratorHost;

}  // namespace
}  // namespace sc4

// cSC4LotConfigurationManager lives in its own translation unit: its header and BuildingDevelopmentSimulator.h
// both define kGZCLSID_cSC4BuildingDevelopmentSimulator (the former with a misnamed interface id), so they
// cannot be included together.
cIGZSerializable* CreateCityProbeLotConfigurationManager();

// The probe's serializable factory consults this after gzcom::FindClassFactory (and its creator) fails.
// Returns a cIGZSerializable with one reference, or nullptr when the class is not constructible here.
cIGZSerializable* CreateCityProbeSerializable(std::uint32_t classId) {
    using namespace sc4;
    switch (classId) {
        // ---- COM-compatible, no-arg constructors -----------------------------------------------------------
        case 0x098f964d: {  // cSC4OccupantManager
            auto* object = new cSC4OccupantManager();
            object->AddRef();
            return object;
        }
        case 0xa990c022: {  // cSC4IndustrialSimulator
            auto* object = new cSC4IndustrialSimulator();
            object->AddRef();
            return object;
        }
        case 0xa990c08e: {  // cSC4OrdinanceSimulator
            auto* object = new cSC4OrdinanceSimulator();
            object->AddRef();
            return object;
        }
        case 0xa9c05c85: {  // cSC4FloraOccupant
            auto* object = new cSC4FloraOccupant();
            object->AddRef();
            return object;
        }
        case 0xe990be01: {  // cSC4BudgetSimulator
            auto* object = new cSC4BudgetSimulator();
            object->AddRef();
            return object;
        }
        case 0xe990bffc: {  // cSC4DepartmentBudget
            auto* object = new cSC4DepartmentBudget();
            object->AddRef();
            return object;
        }
        case 0xaa313c9f: {  // cSC4LineItem (owns its department reference; starts with one reference already)
            auto* department = new cSC4DepartmentBudget();
            auto* object = new cSC4LineItem(department, 0, false);
            return object;
        }
        case 0xca14ca03: {  // cSC4AutomataControllerManager
            auto* object = new cSC4AutomataControllerManager();
            object->AddRef();
            return object;
        }
        case 0x4a1b664e: {  // cSC4AutomataScriptSystem
            auto* object = new cSC4AutomataScriptSystem();
            object->AddRef();
            return object;
        }

        // ---- COM-compatible, host-driven -----------------------------------------------------------
        case 0x89c49986: {  // cSC4WealthRequester(IWealthHost*, cRZRandom*)
            auto* object = new cSC4WealthRequester(CityProbeWealthHost(), new cRZRandom(1));
            object->AddRef();
            return object;
        }
        case 0xc990c0a4: {  // cSC4NeighborsSimulator(INeighborsHost*)
            auto* object = new cSC4NeighborsSimulator(CityProbeNeighborsHost());
            object->AddRef();
            return object;
        }
        case 0x2990c1bc: {  // cSC4WeatherSimulator(IWeatherHost*)
            // the application's installed host (the game's NativeWeather.cpp) when there is one, else the probe binding
            auto* object = new cSC4WeatherSimulator(WeatherHost() ? WeatherHost() : CityProbeWeatherHost());
            object->AddRef();
            return object;
        }
        case 0x8990c080: {  // cSC4CommercialSimulator(ICommercialHost*)
            auto* object = new cSC4CommercialSimulator(CityProbeCommercialHost());
            object->AddRef();
            return object;
        }
        case 0xe990c18e: {  // cSC4LandValueSimulator(ILandValueHost*)
            auto* object = new cSC4LandValueSimulator(CityProbeLandValueHost());
            object->AddRef();
            return object;
        }

        // ---- cSC4SimGrid<T> (the city file's tract grids) ---------------------------------------------
        case 0x49b9e602: { auto* object = new cSC4SimGrid<std::uint8_t>(); object->AddRef(); return object; }
        case 0x49b9e603: { auto* object = new cSC4SimGrid<std::int8_t>(); object->AddRef(); return object; }
        case 0x49b9e604: { auto* object = new cSC4SimGrid<std::uint16_t>(); object->AddRef(); return object; }
        case 0x49b9e605: { auto* object = new cSC4SimGrid<std::int16_t>(); object->AddRef(); return object; }
        case 0x49b9e606: { auto* object = new cSC4SimGrid<std::uint32_t>(); object->AddRef(); return object; }
        case 0x49b9e60a: { auto* object = new cSC4SimGrid<float>(); object->AddRef(); return object; }

        // ---- plain serializables (recovered Write/Read/GetGZCLSID, no COM base) ----------------------
        case 0x0990c38a:   // cSC4Demolition(IDemolitionHost*)
            return WrapPlain(new cSC4Demolition(CityProbeDemolitionHost()));
        case 0x8990c09a:   // cSC4CrimeSimulator(ICrimeHost*, CrimeTypeRegistry*)
            return WrapPlain(new cSC4CrimeSimulator(CityProbeCrimeHost(), new CrimeTypeRegistry()));
        case 0x8990be7a:   // cSC4BuildingDevelopmentSimulator
            return WrapPlain(new cSC4BuildingDevelopmentSimulator());
        case 0xa990bd10:   // cSC4PropManager
            return WrapPlain(new cSC4PropManager());
        case 0x299062c0:   // cSC4LotConfigurationManager (separate TU: header constant collision)
            return CreateCityProbeLotConfigurationManager();
        case 0xc9bd5d4a:   // cSC4Lot (a plain class: its GZCOM creator returns the raw object)
            return WrapPlain(new cSC4Lot());
        case 0x8990c12d:   // cSC4Demand(IDemandHost*)
            return WrapPlain(new cSC4Demand(CityProbeDemandHost()));
        case 0xa7e7f929:   // cSC424HourClock(IClockHost*)
            return WrapPlain(new cSC424HourClock(CityProbeClockHost()));
        case 0x4a1628c5:   // cSC4AutomataGenerator(IAutomataGeneratorHost*)
            return WrapPlain(new cSC4AutomataGenerator(&g_automataGeneratorHost));

        default:
            return nullptr;
    }
}

#include "sc4/SC4Utilities.h"
#include <filesystem>
// Loading a city file in the native application (see NativeCity.h). The factory and city host are the ones the city probe
// (src/tools/city_probe.cpp) proved against the retail files.
#include "app/NativeCitySystems.h"
#include "app/NativeRuntimeWire.h"
#include "app/NativeCity.h"
#include "app/NativeBuildingDevelopment.h"
#include "app/NativeGrowthDevelopers.h"

#include <cstdio>
#include <set>

#include "gz/GZCOM.h"
#include "sc4/App.h"
#include "sc4/Exemplar.h"
#include "sc4/ServiceLocator.h"
#include "sc4/AutoHelpers.h"
#include "sc4/DBSegment.h"
#include "sc4/ISC4City.h"
#include "sc4/SaveGame.h"
#include "sc4/cSC4City.h"
#include "sc4/Simulator.h"

cIGZSerializable* CreateCityProbeSerializable(std::uint32_t classId);   // city_probe_creators.cpp
cIGZSerializable* CreateCityProbeSerializable2(std::uint32_t classId);
cIGZSerializable* CreateCityProbeSerializable3(std::uint32_t classId);
cIGZSerializable* CreateCityProbeSerializable4(std::uint32_t classId);
cIGZSerializable* CreateCityProbeSerializable5(std::uint32_t classId);
cIGZSerializable* CreateCityProbeSerializable6(std::uint32_t classId);
cIGZSerializable* CreateCityProbeSerializable7(std::uint32_t classId);
cIGZSerializable* CreateCityProbeSerializable8(std::uint32_t classId);
void InstallCityProbeBindings();                                          // city_probe_bindings.cpp

namespace native {
void SetTerrainHostCity(cISC4City* city);   // NativeTerrainHost.cpp
static cISC4City* g_hostCity = nullptr;
static std::set<std::uint32_t>* g_missingClasses = nullptr;   // the factory's list of classes it could not make
static sc4::ISerializableFactory* g_serFactory = nullptr;      // the city the host creates (OnCityInitialized binds its services)
namespace {

// The city's host services. The game implements this with the framework, the message server and GZCOM; the probe only
// needs the serial number and the language, and everything optional stays a no-op so the load path itself runs.
struct CityHost final : sc4::ICityHost {
    std::uint32_t CreateCitySerialNumber() override { return sc4::SC4CreateCitySerialNumber(); }
    void GetCurrentLanguageAndCountry(int& language, int& country) override { language = 0; country = 0; }
    // A component the city file did not carry: the class's GZCOM creator, asked for the interface the city keeps it as
    // (cSC4City::CreateCityComponents -> GZCOM()->CreateInstance(classId, iid)).
    cIGZUnknown* CreateComponent(std::uint32_t classId, std::uint32_t iid) override;
    void PostMessage(cIGZMessage2* message) override { if (SL::spMS2) SL::spMS2->MessagePost(message, false); }   // SL::spMS2 slot 0x20
    void SendMessage(cIGZMessage2* message) override { if (SL::spMS2) SL::spMS2->MessageSend(message); }          // slot 0x18
    // slot 0x18 / 0x20 of each component (NativeCityComponents.cpp)
    void InitComponent(sc4::CityComponent component, cIGZUnknown* object) override { RunCityComponentLifecycle(component, object, true); }
    void ShutdownComponent(sc4::CityComponent component, cIGZUnknown* object) override { RunCityComponentLifecycle(component, object, false); }
    // cSC4City::Init has created and initialised the components: the locator's OnCityCreate / OnCityInit.
    void OnCityInitialized() override { BindCityServices(g_hostCity); ReportCityWiring(); }
    // SetEstablished: the simulator's SetCityEstablished (vslot 0x108) switches the agents of an established city on.
    void OnEstablishedChanged(bool established) override {
        sc4::cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Simulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        if (serializable->GetGZCLSID() == sc4::kGZCLSID_cSC4Simulator) static_cast<sc4::cSC4Simulator*>(serializable)->SetCityEstablished(established);
        serializable->Release();
    }
};

// Every serialized object of a city file is created from its class id, as the game's serializer does through GZCOM
// (cSC4COMSerializer::OnGetGZSerializable -> GZCOM()->CreateInstance(classId, cIGZSerializable)). The probe goes
// through the generated GZCOM creator table (gz/GZCOM.h); classes whose creator is not linked or not recovered come
// back null and are listed, so the output names the next class LoadCity needs.
struct Factory final : sc4::ISerializableFactory {
    cIGZSerializable* CreateSerializable(std::uint32_t classId) override {
        if (classId == 0x8990c372) {                        // cSC4City: the probe's own host
            auto* city = new sc4::cSC4City(&host);
            city->AddRef();
            g_hostCity = city;
            SetTerrainHostCity(city);
            return city;
        }
        // Classes the decompilation kept as plain C++ (no cIGZUnknown base) have a GZCOM creator that returns a raw
        // object, which is not a COM pointer; the probe's wrapper takes priority for those.
        if (cIGZSerializable* wrapped = CreateCityProbeSerializable(classId)) return wrapped;
        for (auto create : {CreateCityProbeSerializable2, CreateCityProbeSerializable3, CreateCityProbeSerializable4, CreateCityProbeSerializable5, CreateCityProbeSerializable6, CreateCityProbeSerializable7, CreateCityProbeSerializable8})
            if (cIGZSerializable* wrapped = create(classId)) return wrapped;
        const gzcom::ClassFactoryInfo* info = gzcom::FindClassFactory(classId);
        void* object = info && info->create ? info->create() : nullptr;
        cIGZSerializable* serializable = nullptr;
        if (object) {
            auto* unknown = static_cast<cIGZUnknown*>(object);   // the creators hand out the primary (cIGZUnknown) pointer
            unknown->AddRef();
            if (!unknown->QueryInterface(sc4::kGZIID_cIGZSerializable, reinterpret_cast<void**>(&serializable))) serializable = nullptr;
            unknown->Release();
        }
        if (!serializable && missing.insert(classId).second)
            std::printf("  class %08x %s: %s\n", classId, info ? info->className : "(not in the GZCOM table)",
                        !info ? "unknown id" : object ? "no cIGZSerializable" : "creator returns null");
        return serializable;
    }
    CityHost host;
    std::set<std::uint32_t> missing;
};

cIGZUnknown* CityHost::CreateComponent(std::uint32_t classId, std::uint32_t iid) {
    // The serialization factory knows every class the city can hold, including the wrappers for the classes that are plain C++.
    if (classId == 0 || !g_serFactory) return nullptr;
    cIGZSerializable* serializable = g_serFactory->CreateSerializable(classId);
    cIGZUnknown* result = nullptr;
    if (serializable) {
        void* raw = nullptr;
        if (serializable->QueryInterface(iid, &raw) && raw) {
            result = static_cast<cIGZUnknown*>(raw);   // one reference
        } else {
            // The classes the decompilation kept as plain C++ answer only their serializable view; the loaded components of the city are
            // held the same way (every user asks the component for iid 0xe4fda3d4 and checks the class id).
            serializable->AddRef();
            result = static_cast<cIGZUnknown*>(static_cast<cIGZSerializable*>(serializable));
        }
        serializable->Release();
    }
    if (!result) {   // not a serializable class: the raw GZCOM creator, which hands out the primary cIGZUnknown pointer
        const gzcom::ClassFactoryInfo* info = gzcom::FindClassFactory(classId);
        void* object = info && info->create ? info->create() : nullptr;
        if (object) {
            auto* unknown = static_cast<cIGZUnknown*>(object);
            unknown->AddRef();
            void* raw = nullptr;
            if (unknown->QueryInterface(iid, &raw) && raw) result = static_cast<cIGZUnknown*>(raw);
            unknown->Release();
        }
    }
    // A component the serialization factory could not wrap (its class answers only its own interface, as the original's do) but the raw
    // creator made is not missing.
    if (result && g_missingClasses) g_missingClasses->erase(classId);
    std::printf("city: create component class %08x iid %08x: %s\n", classId, iid, result ? "ok" : "no creator");
    return result;
}

}  // namespace

namespace {
CityScreenCallbacks g_screens;
}

void SetCityScreenCallbacks(CityScreenCallbacks callbacks) { g_screens = std::move(callbacks); }

// Exemplars (type 0x6534284a) and cohorts: the resource manager needs the recovered factories to build them from the packages.
// The game registers them at start-up; the region screen's dialogs read tuning exemplars before any city exists. Once only.
void RegisterExemplarFactories() {
    static bool registered = false;
    if (registered || !SL::spRM) return;
    registered = true;
    static sc4::cSCResExemplarFactory exemplarFactory;
    static sc4::cSCResExemplarCohortFactory cohortFactory;
    exemplarFactory.AddRef();
    cohortFactory.AddRef();
    SL::spRM->RegisterObjectFactory(0x453429b3, sc4::kResourceTypeExemplar, &exemplarFactory);
    SL::spRM->RegisterObjectFactory(0x053429c8, sc4::kResourceTypeCohort, &cohortFactory);
}

bool LoadCityFile(cSC4App& app, const std::string& path) {
    static CityHost cityHost;
    static Factory factory;
    static bool installed = false;
    if (!installed) {
        installed = true;
        sc4::SetSerializableFactory(&factory);
        g_serFactory = &factory;
        g_missingClasses = &factory.missing;
        auto& hooks = app.CityHooks();
        hooks.createCity = []() -> cISC4City* { auto* city = new sc4::cSC4City(&cityHost); city->AddRef(); g_hostCity = city; SetTerrainHostCity(city); return city; };
        hooks.createCitySegment = []() -> cIGZPersistDBSegment* {
            auto* segment = new sc4::cSC4DBSegment();
            segment->AddRef();
            return segment->AsIGZPersistDBSegment();
        };
        hooks.readLoadSaveVersion = [](cIGZPersistDBSegment* segment) { return sc4::SC4ReadAppLoadSaveVersion(segment); };
        // SC4CreateSavePathForCity (0x1001e98a4 -> 0x1001e9aa9): "City - <name>.sc4", sanitised, resolved in the current region's
        // directory (natively the directory the open city was read from), " (n)" appended while another city's file holds the name.
        hooks.createSavePathForCity = [](const cRZString& cityName, std::uint32_t, cRZString& out) {
            out = cRZString("");
            cRZString current;
            if (g_hostCity) g_hostCity->GetCitySaveFilePath(current);
            const std::string currentPath = current.ToChar();
            const std::size_t slash = currentPath.rfind('/');
            if (slash == std::string::npos) { out = current; return; }   // no directory known: the file it came from
            const std::string directory = currentPath.substr(0, slash + 1);
            for (unsigned copy = 1;; ++copy) {
                std::string name = std::string("City - ") + cityName.ToChar();
                if (copy > 1) name += " (" + std::to_string(copy) + ")";
                name += ".sc4";
                cRZString sanitized;
                sc4::SC4SanitizeFileName(cRZString(name.c_str()), sanitized);
                const std::string candidate = directory + sanitized.ToChar();
                if (candidate == currentPath || !std::filesystem::exists(candidate)) { out = cRZString(candidate.c_str()); return; }
            }
        };
        hooks.notify = [](const std::string& title, const std::string& text) { std::printf("notify: %s - %s\n", title.c_str(), text.c_str()); };
        hooks.loadText = [](std::uint32_t, std::uint32_t) { return std::string(); };
        hooks.busy = [](bool) {};
        hooks.flushGraphics = [] {};
        hooks.destroyCurrentScreen = [] { if (g_screens.destroyCurrentScreen) g_screens.destroyCurrentScreen(); };
        hooks.createCityScreen = [] { if (g_screens.createCityScreen) g_screens.createCityScreen(); };
        hooks.createRegionScreen = [] { if (g_screens.createRegionScreen) g_screens.createRegionScreen(); };
        RegisterExemplarFactories();
        InstallStubHosts();
        InstallCityProbeBindings();
        InstallBuildingDevelopmentHost();   // the real hosts replace the generated stubs before the components Init
        InstallGrowthDeveloperHost();
        InstallTractDeveloperHost();
        InstallDemandSimulatorHost();
        InstallLotConfigurationHosts();
    }
    const bool loaded = app.LoadCity(cRZString(path.c_str()), nullptr);
    std::printf("city: %s %s (%zu classes missing)\n", path.c_str(), loaded ? "loaded" : "FAILED", factory.missing.size());
    return loaded;
}

// The city being loaded or played (the one cSC4City::Init runs on, before the application publishes it).
sc4::cSC4City* HostCity() { return dynamic_cast<sc4::cSC4City*>(g_hostCity); }

}  // namespace native

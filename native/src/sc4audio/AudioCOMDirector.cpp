// The game's audio COM directors: see AudioCOMDirector.h for the overview and the addresses.
#include "sc4audio/AudioCOMDirector.h"

#include <cstdio>
#include <cstdlib>

#include "audio/FreshnessDirector.h"
#include "audio/GZSnd.h"
#include "audio/GZSnd3D.h"
#include "audio/GZSndSysMac.h"
#include "audio/HitChannel.h"
#include "audio/HitDebugInfo.h"
#include "audio/HitList.h"
#include "audio/HitMan.h"
#include "audio/HitResourceFactory.h"
#include "audio/HitSegmentCreators.h"
#include "audio/HitTrackPlayer.h"
#include "gz/GZCOMDirector.h"
#include "gz/cIGZFile.h"
#include "gz/cIGZFrameWork.h"
#include "gz/cIGZInitShutdown.h"
#include "rz/COMDirectors.h"
#include "rz/DiskRadioStation.h"
#include "rz/Radio.h"
#include "rz/cRZSysServPtr.h"
#include "sc4/AmbienceCellFilters.h"
#include "sc4/AmbienceOccupantFilters.h"
#include "sc4/AmbienceStateFilters.h"
#include "sc4/ResourceAsFile.h"
#include "sc4/ServiceLocator.h"
#include "sc4audio/AudioAmbienceManager.h"
#include "sc4audio/AudioEventHandler.h"
#include "sc4audio/AudioScapeManager.h"
#include "sc4audio/Listener.h"
#include "sc4audio/SemManager.h"
#include "sc4audio/SoundManager.h"

namespace gz {
void RegisterGZSoundClasses();   // audio/GZSoundDirector.cpp
}

namespace sc4audio {
namespace {
IHitIniHost* g_iniHost = nullptr;
}
void SetHitIniHost(IHitIniHost* host) { g_iniHost = host; }
IHitIniHost* HitIniHost() { return g_iniHost; }
}  // namespace sc4audio

namespace {

constexpr std::uint32_t kGZSID_cHitMan = 0x1b2ce671;

// SC4_AUDIODEBUG: one line per start-up step that failed.
bool Step(const char* name, bool ok) {
    if (!ok && std::getenv("SC4_AUDIODEBUG")) std::fprintf(stderr, "audio director: %s FAILED\n", name);
    return ok;
}

// A class's Create function as the class table's maker.
#define AUDIO_CLASS(id, expression) AddCls(id, +[]() -> cIGZUnknown* { return expression; })

}  // namespace

// ---------------------------------------------------------------------------------------------
// GZSndSysMacCOMDirector
// ---------------------------------------------------------------------------------------------

GZSndSysMacCOMDirector::GZSndSysMacCOMDirector() {
    AUDIO_CLASS(0xfded7f31, static_cast<cIGZSnd*>(cGZSnd::Create()));
    AUDIO_CLASS(0xfded7f34, static_cast<cIGZSnd*>(cGZSnd3D::Create()));
    AUDIO_CLASS(0xe9d28415, static_cast<cIGZSystemService*>(cGZSndSysMac::Create()));
}

bool GZSndSysMacCOMDirector::OnStart(cIGZCOM*) {
    cRZAutoRefCount<cIGZSystemService> renderer;
    GZCOM()->CreateInstance(0xe9d28415, kGZIID_cIGZSystemService, renderer.AsOutParam());
    if (renderer) RZGetFramework()->AddSystemService(renderer.get());
    return true;
}

cRZCOMDllDirector* GetGZSndSysMacCOMDirector() {
    static GZSndSysMacCOMDirector director;
    return &director;
}

// ---------------------------------------------------------------------------------------------
// cHitManCOMDirector
// ---------------------------------------------------------------------------------------------

// The original registers ten classes. Two are left out here: cHitIniFile (0x3d802d67) and
// cPropertySetUint32XmlReader (0xaba06ef4) have no cIGZUnknown view in this build (the director's own PostAppInit
// builds the reader directly, and nothing creates either by class id).
cHitManCOMDllDirector::cHitManCOMDllDirector() : hooks_(this) {
    AUDIO_CLASS(HitIds::kClsidHitMan, static_cast<cIHitMan*>(new cHitMan()));
    AUDIO_CLASS(HitIds::kClsidHitDebugInfo, static_cast<cIHitDebugInfo*>(new cHitDebugInfo()));
    AUDIO_CLASS(HitIds::kClsidHitResourceFactory, static_cast<cIGZPersistResourceFactory*>(new sc4audio::cHitResourceFactory()));
    AUDIO_CLASS(HitIds::kClsidHitSegmentTrackCreator, static_cast<cIGZSoundCreator*>(new cHitSegmentTrackCreator()));
    AUDIO_CLASS(HitIds::kClsidHitSegmentDataSourceChannelCreator, static_cast<cIGZSoundCreator*>(new cHitSegmentDataSourceChannelCreator()));
    AUDIO_CLASS(HitIds::kClsidHitTrackPlayer, static_cast<cIHitTrackPlayer*>(new cHitTrackPlayer()));
    AUDIO_CLASS(HitIds::kClsidHitChannel, static_cast<cIGZSnd*>(new cHitChannel()));
    AUDIO_CLASS(HitIds::kClsidHitList, static_cast<cIHitList*>(new cHitList()));
}

bool cHitManCOMDllDirector::OnStart(cIGZCOM*) { return hooks_.OnStart(nullptr); }

cIGZPersistResourceManager* cHitManCOMDllDirector::ResourceManager() { return SL::spRM; }
cIGZSndMgr* cHitManCOMDllDirector::SoundManager() {
    cRZSysServPtr<cIGZSndMgr, kGZSID_cGZSndMgr, kGZIID_cIGZSndMgr> manager;
    return manager.get();   // borrowed: the framework keeps the service
}
cIHitMan* cHitManCOMDllDirector::HitManager() {
    cRZSysServPtr<cIHitMan, kGZSID_cHitMan, kGZIID_cIHitMan> manager;
    return manager.get();
}
cIHitDebugInfo* cHitManCOMDllDirector::DebugInfo() {
    cRZSysServPtr<cIHitDebugInfo, kGZSID_cHitDebugInfo, kGZIID_cIHitDebugInfo> info;
    return info.get();
}
bool cHitManCOMDllDirector::CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) {
    return GZCOM()->CreateInstance(classId, iid, out);
}
void cHitManCOMDllDirector::RegisterSystemService(cIGZUnknown* service) {
    cRZAutoRefCount<cIGZSystemService> asService;
    if (service && service->QueryInterface(kGZIID_cIGZSystemService, asService.AsOutParam())) RZGetFramework()->AddSystemService(asService.get());
}
void cHitManCOMDllDirector::AddSystemServiceDirector(cIGZUnknown*) {
    RZGetFramework()->AddHook(static_cast<cIGZFrameWorkHooks*>(this));
}

cRZCOMDllDirector* GetHitManCOMDirector() {
    static cHitManCOMDllDirector director;
    return &director;
}

// ---------------------------------------------------------------------------------------------
// cFreshnessCOMDirector
// ---------------------------------------------------------------------------------------------

cFreshnessCOMDirector::cFreshnessCOMDirector() {
    for (const Fresh::ClassFactory& factory : Fresh::kClassFactories) AddCls(factory.classId, factory.create);
}
std::uint32_t cFreshnessCOMDirector::GetDirectorID() const { return Fresh::kDirectorId; }

cRZCOMDllDirector* GetFreshnessCOMDirector() {
    static cFreshnessCOMDirector director;
    return &director;
}

// ---------------------------------------------------------------------------------------------
// cSC4AudioCOMDirector
// ---------------------------------------------------------------------------------------------

cSC4AudioCOMDirector::cSC4AudioCOMDirector() {
    using namespace sc4;
    gz::RegisterGZSoundClasses();
    // The nine services, then the ambience layers' 83 filters, in the original's order.
    AUDIO_CLASS(0xaa355871, cSC4AudioSemManager::Create());
    AUDIO_CLASS(0x29e2797f, cSC4AudioScapeManager::Create());
    AUDIO_CLASS(0xbdb6171e, static_cast<cIGZSndEventHandler*>(cSC4AudioEventHandler::Create()));
    AUDIO_CLASS(0x69e500bc, cSC4AudioEventHandlerDisasterFire::Create());
    AUDIO_CLASS(0x6a12f51c, static_cast<cIGZSystemService*>(SC4Audio::cSC4AudioAmbienceManager::Create()));
    AUDIO_CLASS(0x4a47dc0d, static_cast<cIGZSndEventHandler*>(cSC4AudioSoundManager::Create()));
    AUDIO_CLASS(0xca4a66b9, cSC4AudioListener::Create());
    AUDIO_CLASS(0x5cfbdbbf, CreatecRZRadio());
    AUDIO_CLASS(0x5cfbdbd5, CreatecRZDiskRadioStation());
    AUDIO_CLASS(0xca426982, cAmbienceFilterIsDay::Create());
    AUDIO_CLASS(0x8a401e0f, cAmbienceFilterIsNight::Create());
    AUDIO_CLASS(0x2a426986, cAmbienceFilterIsZoom5::Create());
    AUDIO_CLASS(0x8a42698a, cAmbienceFilterIsZoom4::Create());
    AUDIO_CLASS(0x6a42698e, cAmbienceFilterIsZoom3::Create());
    AUDIO_CLASS(0x0a426991, cAmbienceFilterIsZoom2::Create());
    AUDIO_CLASS(0x6a426995, cAmbienceFilterIsZoom1::Create());
    AUDIO_CLASS(0x0a426998, cAmbienceFilterIsAlways::Create());
    AUDIO_CLASS(0xaa54e710, cAmbienceFilterIsPipeView::Create());
    AUDIO_CLASS(0xaa54e715, cAmbienceFilterIsSubwayView::Create());
    AUDIO_CLASS(0x0a54e719, cAmbienceFilterIsSurfaceView::Create());
    AUDIO_CLASS(0x0b817c41, cAmbienceFilterIsSimSpeedPaused::Create());
    AUDIO_CLASS(0xab817c0b, cAmbienceFilterIsSimSpeedSlow::Create());
    AUDIO_CLASS(0xcb817c1b, cAmbienceFilterIsSimSpeedMedium::Create());
    AUDIO_CLASS(0x2b817c27, cAmbienceFilterIsSimSpeedFast::Create());
    AUDIO_CLASS(0x8b817c32, cAmbienceFilterIsDrivingMode::Create());
    AUDIO_CLASS(0x0a401ba0, cAmbienceOccupantFilterIsDistressed::Create());
    AUDIO_CLASS(0x4a401ba7, cAmbienceOccupantFilterIsIndustrialDirty::Create());
    AUDIO_CLASS(0xca401bae, cAmbienceOccupantFilterIsIndustrialClean::Create());
    AUDIO_CLASS(0x6a401bb3, cAmbienceOccupantFilterIsNature::Create());
    AUDIO_CLASS(0x6a401bba, cAmbienceOccupantFilterIsTraffic::Create());
    AUDIO_CLASS(0x0a42b520, cAmbienceOccupantFilterIsLandfill::Create());
    AUDIO_CLASS(0x6a43c75d, cAmbienceOccupantFilterIsBuilding::Create());
    AUDIO_CLASS(0xca5a6a5f, cAmbienceOccupantFilterIsVehicle::Create());
    AUDIO_CLASS(0x0a5a6a64, cAmbienceOccupantFilterIsEmergencyVehicle::Create());
    AUDIO_CLASS(0x2a5a6a68, cAmbienceOccupantFilterIsFarmVehicle::Create());
    AUDIO_CLASS(0x6a5a6a6b, cAmbienceOccupantFilterIsCivillianVehicle::Create());
    AUDIO_CLASS(0x4a5a6a6f, cAmbienceOccupantFilterIsTaxi::Create());
    AUDIO_CLASS(0x0a5a6a72, cAmbienceOccupantFilterIsRichVehicle::Create());
    AUDIO_CLASS(0x0a5a6a76, cAmbienceOccupantFilterIsCheapVehicle::Create());
    AUDIO_CLASS(0xaa5a6a7a, cAmbienceOccupantFilterIsSoccerMom::Create());
    AUDIO_CLASS(0xaa5a6a7d, cAmbienceOccupantFilterIsTvReporter::Create());
    AUDIO_CLASS(0xca5a6a81, cAmbienceOccupantFilterIsTrain::Create());
    AUDIO_CLASS(0x2a5a6a84, cAmbienceOccupantFilterIsBus::Create());
    AUDIO_CLASS(0x4a5a6a88, cAmbienceOccupantFilterIsMovingVan::Create());
    AUDIO_CLASS(0xea5a6a8b, cAmbienceOccupantFilterIsGarbageTruck::Create());
    AUDIO_CLASS(0x6a5a6a8f, cAmbienceOccupantFilterIsMySim::Create());
    AUDIO_CLASS(0x4a5d0eb0, cAmbienceOccupantFilterIsResidential::Create());
    AUDIO_CLASS(0xaa5d0eb5, cAmbienceOccupantFilterIsCommercial::Create());
    AUDIO_CLASS(0xea5f4954, cAmbienceOccupantFilterIsAgriculture::Create());
    AUDIO_CLASS(0x8a5f603e, cAmbienceOccupantFilterIsOnFire::Create());
    AUDIO_CLASS(0x8a60f033, cAmbienceOccupantFilterIsAbandoned::Create());
    AUDIO_CLASS(0x2a872395, cAmbienceOccupantFilterIsConstructionVehicle::Create());
    AUDIO_CLASS(0xca9d68a1, cAmbienceOccupantFilterIsLowWealth::Create());
    AUDIO_CLASS(0xca9d68a5, cAmbienceOccupantFilterIsMediumWealth::Create());
    AUDIO_CLASS(0xaa9d68a9, cAmbienceOccupantFilterIsHighWealth::Create());
    AUDIO_CLASS(0x2aa17957, cAmbienceOccupantFilterIsPowered::Create());
    AUDIO_CLASS(0x8a427f7c, cAmbienceCellFilterIsEmpty::Create());
    AUDIO_CLASS(0xea4298fc, cAmbienceCellFilterIsRoad::Create());
    AUDIO_CLASS(0x2a429900, cAmbienceCellFilterIsRail::Create());
    AUDIO_CLASS(0x4a429903, cAmbienceCellFilterIsSubway::Create());
    AUDIO_CLASS(0x2a429907, cAmbienceCellFilterIsPipe::Create());
    AUDIO_CLASS(0xca42990a, cAmbienceCellFilterIsWater::Create());
    AUDIO_CLASS(0x8a53e23e, cAmbienceCellFilterIsResidential::Create());
    AUDIO_CLASS(0xca53e243, cAmbienceCellFilterIsCommercial::Create());
    AUDIO_CLASS(0x0a53e247, cAmbienceCellFilterIsIndustrial::Create());
    AUDIO_CLASS(0x4a53e24b, cAmbienceCellFilterIsAirport::Create());
    AUDIO_CLASS(0x2a53e24f, cAmbienceCellFilterIsSeaport::Create());
    AUDIO_CLASS(0xca53e252, cAmbienceCellFilterIsSpaceport::Create());
    AUDIO_CLASS(0x0a53e290, cAmbienceCellFilterIsLandfill::Create());
    AUDIO_CLASS(0x2a53f0c3, cAmbienceCellFilterIsContruction::Create());
    AUDIO_CLASS(0x8a53f0e4, cAmbienceCellFilterIsLowGrowthStage::Create());
    AUDIO_CLASS(0x4a53f0e8, cAmbienceCellFilterIsMediumGrowthStage::Create());
    AUDIO_CLASS(0x8a53f584, cAmbienceCellFilterIsHighGrowthStage::Create());
    AUDIO_CLASS(0xea5a6fce, cAmbienceCellFilterIsHighCrime::Create());
    AUDIO_CLASS(0x8a5a6fd1, cAmbienceCellFilterIsMediumCrime::Create());
    AUDIO_CLASS(0xea5a6fd4, cAmbienceCellFilterIsLowCrime::Create());
    AUDIO_CLASS(0x6a5d0682, cAmbienceCellFilterIsLowDesirability::Create());
    AUDIO_CLASS(0x0a5d068d, cAmbienceCellFilterIsMediumDesirability::Create());
    AUDIO_CLASS(0x2a5d0691, cAmbienceCellFilterIsHighDesirability::Create());
    AUDIO_CLASS(0xea5f495c, cAmbienceCellFilterIsUnzoned::Create());
    AUDIO_CLASS(0x4a60d870, cAmbienceCellFilterIsStreet::Create());
    AUDIO_CLASS(0x8a60d873, cAmbienceCellFilterIsHighway::Create());
    AUDIO_CLASS(0x6a60f166, cAmbienceCellFilterIsAgriculture::Create());
    AUDIO_CLASS(0xea80835e, cAmbienceCellFilterIsDistressed::Create());
    AUDIO_CLASS(0x0a808367, cAmbienceCellFilterIsIndustrialDirty::Create());
    AUDIO_CLASS(0xca80836c, cAmbienceCellFilterIsIndustrialClean::Create());
    AUDIO_CLASS(0x2a808371, cAmbienceCellFilterIsBuilding::Create());
    AUDIO_CLASS(0x4a808375, cAmbienceCellFilterIsAbandoned::Create());
    AUDIO_CLASS(0x4a8084f4, cAmbienceCellFilterIsZoo::Create());
    AUDIO_CLASS(0x2a9d6690, cAmbienceCellFilterIsLowWealth::Create());
    AUDIO_CLASS(0xaa9d6694, cAmbienceCellFilterIsMediumWealth::Create());
    AUDIO_CLASS(0x2a9d6697, cAmbienceCellFilterIsHighWealth::Create());
}

// The library directors join the process director and start, in this order; no result is checked.
bool cSC4AudioCOMDirector::OnStart(cIGZCOM* com) {
    cRZCOMDllDirector* process = RZGetCOMDllDirector();
    RZGetFramework()->AddHook(static_cast<cIGZFrameWorkHooks*>(this));
    for (cRZCOMDllDirector* child : {gz::GetGZSoundCOMDirector(), GetGZSndSysMacCOMDirector(), GetHitManCOMDirector(), GetFreshnessCOMDirector()}) {
        process->AddDirector(child);
        child->OnStart(com);
    }
    return true;
}

bool cSC4AudioCOMDirector::PostAppInit() {
    cRZAutoRefCount<cIGZFile> file;
    const cGZPersistResourceKey key{0, 0x8a5971c5, 0xea8a1115};
    cIGZFile* opened = nullptr;
    if (!Step("the sound configuration record", sc4::SC4GetResourceAsFile(key, &opened, false))) return false;
    file = opened;
    if (opened) opened->Release();   // the holder keeps the one reference SC4GetResourceAsFile handed out

    bool result = false;
    sc4audio::cHitIniFile ini(sc4audio::HitIniHost(), file.get());
    if (Step("the sound configuration", ini.EnumDefaultEntries())) {
        cIGZCOM* com = GZCOM();
        cRZAutoRefCount<cIGZSystemService> radio;
        result = Step("radio", com->CreateInstance(0x5cfbdbbf, kGZIID_cIGZSystemService, radio.AsOutParam()) && radio->Init() &&
                                   RZGetFramework()->AddSystemService(radio.get()));
        if (!Step("emanation model manager", com->CreateInstance(0xaa355871, kGZIID_cSC4AudioSemManager, semManager_.AsOutParam()))) result = false;
        if (!Step("soundscape manager", com->CreateInstance(0x29e2797f, kGZIID_cISC4AudioScapeManager, scapeManager_.AsOutParam()))) result = false;
        if (!Step("event handler", com->CreateInstance(0xbdb6171e, sc4audio::kGZIID_cSC4AudioEventHandler, eventHandler_.AsOutParam()))) result = false;
        if (!Step("disaster fire handler", com->CreateInstance(0x69e500bc, sc4audio::kGZIID_cSC4AudioEventHandler, fireHandler_.AsOutParam()))) result = false;
        if (!Step("ambience manager", com->CreateInstance(0x6a12f51c, SC4Audio::kGZIID_cISC4AudioAmbienceManager, ambienceManager_.AsOutParam()))) result = false;
        if (!Step("sound manager", com->CreateInstance(0x4a47dc0d, 0x4a47dc0d, soundManager_.AsOutParam()))) result = false;
        if (!Step("listener", com->CreateInstance(0xca4a66b9, 0xca4a66b9, listener_.AsOutParam()))) result = false;
        // SFX::SoundManager() reads the holder above: the emanation models ask it for the master volume from their Init on.
        SC4Audio::SetSoundManager(soundManager_ ? soundManager_->AsMasterVolume() : nullptr);

        if (!Step("soundscape manager Init", scapeManager_ && scapeManager_->Init())) result = false;
        if (!Step("ambience manager Init", ambienceManager_ && ambienceManager_->Init())) result = false;
        if (!Step("sound manager Init", soundManager_ && soundManager_->Init())) result = false;
        if (!Step("listener Init", listener_ && listener_->Init())) result = false;
        {
            cRZSysServPtr<cIHitMan, kGZSID_cHitMan, kGZIID_cIHitMan> hitMan;   // the manager's own lookup in the original
            if (!Step("emanation model manager Init", semManager_ && semManager_->Init(hitMan.get()))) result = false;
        }
        // The two handlers start through their cIGZInitShutdown view; one that does not answer it is skipped.
        for (cIGZSndEventHandler* handler : {eventHandler_.get(), fireHandler_.get()}) {
            cRZAutoRefCount<cIGZInitShutdown> start;
            if (handler && handler->QueryInterface(kGZIID_cIGZInitShutdown, start.AsOutParam()) &&
                !Step(handler == eventHandler_.get() ? "event handler Init" : "disaster fire handler Init", start->Init()))
                result = false;
        }
    }
    return result;
}

bool cSC4AudioCOMDirector::PreAppShutdown() {
    for (cRZAutoRefCount<cIGZSndEventHandler>* holder : {&fireHandler_, &eventHandler_}) {
        if (!*holder) continue;
        cRZAutoRefCount<cIGZInitShutdown> stop;
        if ((*holder)->QueryInterface(kGZIID_cIGZInitShutdown, stop.AsOutParam())) stop->Shutdown();
        *holder = nullptr;
    }
    if (semManager_) { semManager_->Shutdown(); semManager_ = nullptr; }
    if (listener_) { listener_->Shutdown(); listener_ = nullptr; }
    if (soundManager_) {
        soundManager_->Shutdown();
        SC4Audio::SetSoundManager(nullptr);
        soundManager_ = nullptr;
    }
    if (ambienceManager_) { ambienceManager_->Shutdown(); ambienceManager_ = nullptr; }
    if (scapeManager_) { scapeManager_->Shutdown(); scapeManager_ = nullptr; }
    return true;
}

// The holder keeps the manager by its primary (system service) view, which is the object itself.
SC4Audio::cSC4AudioAmbienceManager* cSC4AudioCOMDirector::AmbienceManager() const {
    return static_cast<SC4Audio::cSC4AudioAmbienceManager*>(ambienceManager_.get());
}

cSC4AudioCOMDirector* GetSC4AudioCOMDirector() {
    static cSC4AudioCOMDirector director;
    return &director;
}

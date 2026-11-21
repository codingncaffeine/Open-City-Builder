// Installs the generated neutral stub hosts (src/app/hosts, scripts/gen_stub_hosts.py).
#include "app/NativeObliterate.h"
#include "app/NativeSignLabel.h"
#include "app/NativeTips.h"
#include "app/NativeSnapshot.h"
#include "app/NativeTutorial.h"
#include "app/NativeUnderground.h"
#include "app/NativeCity.h"

namespace native {
void InstallAdviceStub();
void InstallAdviceSubjectStub();
void InstallAdvisorStub();
void InstallAdvisorConcreteStub();
void InstallAdvisorPopupTrackerStub();
void InstallAdvisorSystemStub();
void InstallAircraftHosts();   // app/NativeAircraft.cpp: the aircraft, helicopter and aircraft manager hosts
void InstallAmbienceHosts();    // app/NativeAudioAmbience.cpp: the ambience system's real hosts
void InstallSoundBankHost();    // app/NativeAudioDirector.cpp
void InstallAuraStub();
void InstallAuraHost();   // app/NativeAura.cpp
void InstallPropDeveloperHost();   // app/NativePropDeveloper.cpp
void InstallAutomataGroupHosts();
void InstallAutomataControllerStub();
void InstallAutomataControllerHost();   // app/NativeAutomataControllerHost.cpp
void InstallControllerManagerStub();
void InstallAutomataManagerStub();
void InstallAutomataManagerHost();   // app/NativeAutomataManagerHost.cpp
void InstallAutomataScriptHost();    // app/NativeAutomataScripts.cpp
void InstallAutomataHost();
void InstallBudgetStub();
void InstallBudgetDialogStub();
void InstallBudgetUIWinProcStub();
void InstallBuildingDevelopmentSimStub();
void InstallFeatureStub();
void InstallCameraControlStub();
void InstallCitySituationAdviceStub();
void InstallCivicBuildingHost();
void InstallDemandSimulatorStub();
void InstallDisasterStub();
void InstallDisasterHosts();   // NativeDisasters.cpp
void InstallDisasterTools();   // NativeDisasterTools.cpp
void InstallEffectsStub();
void InstallEffectsManagerHost();   // NativeEffects.cpp
void InstallCityPanelHosts();
void InstallEstablishCityHost();
void InstallBudgetSimHosts();
void InstallTerrainTools();
void InstallNativeFlora();
void InstallGraphsViewStub();
void InstallGraphsViewHost();   // app/NativeGraphsView.cpp
void InstallGrowthDeveloperStub();
void InstallLandfillDeveloperStub();
void InstallLandfillDeveloperHost();   // NativeLandfill.cpp
void InstallLotBaseTextureHost();      // NativeModelMaker.cpp
void InstallLightingManagerStub();
void InstallLocalGridHost();
void InstallLotStub();
void InstallLotBaseTextureStub();
void InstallLotConfigurationStub();
void InstallLotConfigurationManagerStub();
void InstallLotRetainingWallModelStub();
void InstallLotRetainingWallStub();
void InstallLotRetainingWallHosts();   // NativeLotWalls.cpp
void InstallModelMakerStub();
void InstallMySimStub();
void InstallMySimAdviceStub();
void InstallMySimDispatchStub();
void InstallMySimFilterStub();
void InstallNeighborConnectionStub();
void InstallNeighborDealStub();
void InstallNetworkLotManagerStub();
void InstallNetworkManagerHost();   // NativeNetworkManager.cpp
void InstallNetworkOccupantStub();
void InstallNetworkLotHosts();   // NativeNetworkLots.cpp
void InstallPreBuiltModelOccupantStub();
void InstallNetworkToolStub();
void InstallOrdinanceStub();
void InstallOrdinanceHost();   // app/NativeOrdinance.cpp
void InstallOverlayStub();
void InstallPlaceableStub();
void InstallPortDeveloperStub();
void InstallPortDeveloperHosts();   // NativePorts.cpp
void InstallPowerLineToolHost();   // NativePowerLineTool.cpp
void InstallPropDeveloperStub();
void InstallPropManagerStub();
void InstallPropStub();
void InstallPropControllerStub();
void InstallPropertyViewStub();
void InstallPylonOccupantStub();
void InstallRender3DStub();
void InstallRewardAdviceStub();
void InstallSignpostStub();
void InstallSimulatorStub();
void InstallTextDecalStub();
void InstallToxicStub();
void InstallToxicHost();   // NativeToxic.cpp
void InstallTractDeveloperStub();
void InstallTutorialSystemStub();
void InstallTutorialTaskStub();
void InstallVertexHtConstraintHost();   // NativeNetworkTool.cpp
void InstallView3DWinStub();
void InstallViewInputStub();
void InstallEffectToolStub();
void InstallPlaceLotStub();
void InstallQueryStub();
void InstallQueryHost();   // app/NativeQuery.cpp
void InstallSignToolStub();
void InstallWeatherStub();
void InstallNativeWeather();
void InstallWinAdviceListStub();
void InstallWinHtmlHosts();   // app/NativeWinHtml.cpp
void InstallWinAnimationRecorderStub();
void InstallWinBMPViewStub();
void InstallCatalogViewStub();
void InstallLoadHeightFieldStub();
void InstallWinLotConfigurationEditorStub();
void InstallWinMapViewStub();
void InstallWinMapViewHost();   // app/NativeWinMapView.cpp
void InstallMiniMapStub();
void InstallResizableOutlineStub();
void InstallWinTertiaryMenuFrameStub();
void InstallZoneDeveloperStub();
void InstallTerrainStub();

void InstallStubHosts() {
    InstallAdviceStub();
    InstallAdviceSubjectStub();
    InstallAdvisorStub();
    InstallAdvisorConcreteStub();
    InstallAdvisorPopupTrackerStub();
    InstallAdvisorSystemStub();
    InstallAircraftHosts();               // NativeAircraft.cpp: the aircraft, helicopter and aircraft manager hosts
    InstallAmbienceHosts();
    InstallSoundBankHost();
    InstallAuraStub();
    InstallAuraHost();                    // the mayor rating simulator's real host replaces the stub
    InstallAutomataGroupHosts();          // NativeAutomataGroups.cpp: occupant group rolls, attractor states
    InstallAutomataControllerStub();
    InstallAutomataControllerHost();   // the native host replaces the stub
    InstallControllerManagerStub();
    InstallAutomataManagerStub();
    InstallAutomataManagerHost();   // the native host replaces the stub
    InstallAutomataScriptHost();    // the automata scripts run on the real Lua host
    InstallAutomataHost();   // NativeAutomata.cpp: the models, the city sizes, the terrain
    InstallBudgetStub();
    InstallBudgetDialogStub();
    InstallBudgetUIWinProcStub();
    InstallBuildingDevelopmentSimStub();
    InstallFeatureStub();
    InstallCameraControlStub();
    InstallCitySituationAdviceStub();
    InstallCivicBuildingHost();           // NativeCivic.cpp: the civic building simulator's services
    InstallDemandSimulatorStub();
    InstallDisasterStub();
    InstallDisasterHosts();               // the real disaster, fire and flammability hosts replace the stub
    InstallEffectsStub();
    InstallEffectsManagerHost();         // the effects manager's real host (a city's manager keeps the host it was made with)
    InstallCityPanelHosts();              // the RCI and opinion-poll gauges' services (NativeCityPanel.cpp)
    InstallEstablishCityHost();
    InstallBudgetSimHosts();
    InstallTerrainTools();
    InstallNativeFlora();                 // NativeFlora.cpp: the flora simulator, flora occupant and tree tool services
    InstallGraphsViewStub();
    InstallGraphsViewHost();              // the real host replaces the stub above
    InstallGrowthDeveloperStub();
    InstallLandfillDeveloperStub();
    InstallLandfillDeveloperHost();       // the landfill developer's real host replaces the stub above
    InstallLightingManagerStub();
    InstallLocalGridHost();               // NativeLocalGrid.cpp: the cursor grid's effect and cell size
    InstallLotStub();
    InstallLotBaseTextureStub();
    InstallLotBaseTextureHost();          // the real host replaces the stub above (the lot developer's Init refits base textures)
    InstallLotConfigurationStub();
    InstallLotConfigurationManagerStub();
    InstallLotRetainingWallModelStub();
    InstallLotRetainingWallStub();
    InstallLotRetainingWallHosts();       // the walls' and their models' real hosts replace the two stubs above
    InstallModelMakerStub();
    InstallMySimStub();
    InstallMySimAdviceStub();
    InstallMySimDispatchStub();
    InstallMySimFilterStub();
    InstallNeighborConnectionStub();
    InstallNeighborDealStub();
    InstallNetworkLotManagerStub();
    InstallNetworkManagerHost();          // the network manager's real host (before the city's manager is made)
    InstallNetworkOccupantStub();
    InstallNetworkLotHosts();             // the lot manager's and the pieces' real hosts replace the two stubs
    InstallPreBuiltModelOccupantStub();
    InstallNetworkToolStub();
    InstallOrdinanceStub();
    InstallOrdinanceHost();               // the real ordinance host replaces the stub above
    InstallOverlayStub();
    InstallPlaceableStub();
    InstallPortDeveloperStub();
    InstallPortDeveloperHosts();          // the airport and seaport developers' real hosts replace the stub above
    InstallPowerLineToolHost();           // the power line tool and the power poles
    InstallPropDeveloperStub();
    InstallPropDeveloperHost();   // the prop developer's real host replaces the stub
    InstallPropManagerStub();
    InstallPropStub();
    InstallPropControllerStub();
    InstallPropertyViewStub();
    InstallPylonOccupantStub();
    InstallRender3DStub();
    InstallRewardAdviceStub();
    InstallSignpostStub();
    InstallSimulatorStub();
    InstallTextDecalStub();
    InstallToxicStub();
    InstallToxicHost();                   // the toxic waste occupants' real host replaces the stub above
    InstallTractDeveloperStub();
    InstallTutorialSystemStub();
    InstallTutorialTaskStub();
    InstallTutorialHosts();               // NativeTutorial.cpp: the tutorial system's, its tasks' and its window's real hosts
    InstallUndergroundHosts();   // NativeUnderground.cpp
    InstallVertexHtConstraintHost();      // the height solver's terrain (NativeNetworkTool.cpp)
    InstallView3DWinStub();
    InstallViewInputStub();
    InstallEffectToolStub();
    InstallDisasterTools();               // the disaster menu's tools; the effect tools' cursor effects replace the stub
    InstallPlaceLotStub();
    InstallQueryStub();
    InstallQueryHost();                   // the real query host replaces the stub above
    InstallSignToolStub();
    InstallSignToolHost();                // NativeSignLabel.cpp: the sign tool and signposts' real hosts replace the stubs
    InstallObliterateCityHost();          // NativeObliterate.cpp: the obliterate scene's services
    InstallWeatherStub();
    InstallNativeWeather();               // the real weather host replaces the stub above
    InstallWinAdviceListStub();
    InstallWinHtmlHosts();                // the real advice-list host replaces the stub above
    InstallWinAnimationRecorderStub();
    InstallWinBMPViewStub();
    InstallSnapshotHost();
    InstallTipOfTheDayHost();             // NativeTips.cpp: the tip of the day dialog's host                // NativeSnapshot.cpp: the snapshot frame and album; the picture view's real host replaces the stub above
    InstallCatalogViewStub();
    InstallLoadHeightFieldStub();
    InstallWinLotConfigurationEditorStub();
    InstallWinMapViewStub();
    InstallWinMapViewHost();              // the data-view map window's real host replaces the stub above
    InstallMiniMapStub();
    InstallResizableOutlineStub();
    InstallWinTertiaryMenuFrameStub();
    InstallZoneDeveloperStub();
    InstallTerrainStub();
}

}  // namespace native

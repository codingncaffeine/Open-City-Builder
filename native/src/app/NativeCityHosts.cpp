// Installs the generated neutral stub hosts (src/app/hosts, scripts/gen_stub_hosts.py).
#include "app/NativeCity.h"

namespace native {
void InstallAdviceStub();
void InstallAdviceSubjectStub();
void InstallAdvisorStub();
void InstallAdvisorConcreteStub();
void InstallAdvisorPopupTrackerStub();
void InstallAdvisorSystemStub();
void InstallAircraftStub();
void InstallAmbienceStateStub();
void InstallSoundBankStub();
void InstallAuraStub();
void InstallAutomataAttractorStub();
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
void InstallCivicBuildingStub();
void InstallDemandSimulatorStub();
void InstallDisasterStub();
void InstallDisasterHosts();   // NativeDisasters.cpp
void InstallDisasterTools();   // NativeDisasterTools.cpp
void InstallEffectsStub();
void InstallEffectsManagerStub();
void InstallCityPanelHosts();
void InstallEstablishCityHost();
void InstallBudgetSimHosts();
void InstallTerrainTools();
void InstallFloraStub();
void InstallFloraSimulatorStub();
void InstallNativeFlora();
void InstallGraphsViewStub();
void InstallGrowthDeveloperStub();
void InstallHelicopterStub();
void InstallLandfillDeveloperStub();
void InstallLightingManagerStub();
void InstallLocalGridStub();
void InstallLotStub();
void InstallLotBaseTextureStub();
void InstallLotConfigurationStub();
void InstallLotConfigurationManagerStub();
void InstallLotRetainingWallModelStub();
void InstallLotRetainingWallStub();
void InstallModelMakerStub();
void InstallMySimStub();
void InstallMySimAdviceStub();
void InstallMySimDispatchStub();
void InstallMySimFilterStub();
void InstallNeighborConnectionStub();
void InstallNeighborDealStub();
void InstallNetworkLotManagerStub();
void InstallNetworkManagerStub();
void InstallNetworkOccupantStub();
void InstallPreBuiltModelOccupantStub();
void InstallNetworkToolStub();
void InstallOccupantGroupStub();
void InstallOrdinanceStub();
void InstallOrdinanceHost();   // app/NativeOrdinance.cpp
void InstallOrdinanceSimStub();
void InstallOverlayStub();
void InstallPipeBurstStub();
void InstallPlaceableStub();
void InstallPortDeveloperStub();
void InstallPowerLineToolStub();
void InstallPropDeveloperStub();
void InstallPropManagerStub();
void InstallPropStub();
void InstallPropControllerStub();
void InstallPropVisualEffectStub();
void InstallPropertyViewStub();
void InstallPylonOccupantStub();
void InstallRender3DStub();
void InstallRewardAdviceStub();
void InstallSignpostStub();
void InstallSimulatorStub();
void InstallTextDecalStub();
void InstallToxicStub();
void InstallTractDeveloperStub();
void InstallTutorialSystemStub();
void InstallTutorialTaskStub();
void InstallUndergroundNetworkToolStub();
void InstallUndergroundOccupantStub();
void InstallVertexHtConstraintStub();
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
    InstallAircraftStub();
    InstallAmbienceStateStub();
    InstallSoundBankStub();
    InstallAuraStub();
    InstallAutomataAttractorStub();
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
    InstallCivicBuildingStub();
    InstallDemandSimulatorStub();
    InstallDisasterStub();
    InstallDisasterHosts();               // the real disaster, fire and flammability hosts replace the stub
    InstallEffectsStub();
    InstallEffectsManagerStub();
    InstallCityPanelHosts();              // the RCI and opinion-poll gauges' services (NativeCityPanel.cpp)
    InstallEstablishCityHost();
    InstallBudgetSimHosts();
    InstallTerrainTools();
    InstallFloraStub();
    InstallFloraSimulatorStub();
    InstallNativeFlora();                 // the real flora hosts replace the two stubs above
    InstallGraphsViewStub();
    InstallGrowthDeveloperStub();
    InstallHelicopterStub();
    InstallLandfillDeveloperStub();
    InstallLightingManagerStub();
    InstallLocalGridStub();
    InstallLotStub();
    InstallLotBaseTextureStub();
    InstallLotConfigurationStub();
    InstallLotConfigurationManagerStub();
    InstallLotRetainingWallModelStub();
    InstallLotRetainingWallStub();
    InstallModelMakerStub();
    InstallMySimStub();
    InstallMySimAdviceStub();
    InstallMySimDispatchStub();
    InstallMySimFilterStub();
    InstallNeighborConnectionStub();
    InstallNeighborDealStub();
    InstallNetworkLotManagerStub();
    InstallNetworkManagerStub();
    InstallNetworkOccupantStub();
    InstallPreBuiltModelOccupantStub();
    InstallNetworkToolStub();
    InstallOccupantGroupStub();
    InstallOrdinanceStub();
    InstallOrdinanceHost();               // the real ordinance host replaces the stub above
    InstallOrdinanceSimStub();
    InstallOverlayStub();
    InstallPipeBurstStub();
    InstallPlaceableStub();
    InstallPortDeveloperStub();
    InstallPowerLineToolStub();
    InstallPropDeveloperStub();
    InstallPropManagerStub();
    InstallPropStub();
    InstallPropControllerStub();
    InstallPropVisualEffectStub();
    InstallPropertyViewStub();
    InstallPylonOccupantStub();
    InstallRender3DStub();
    InstallRewardAdviceStub();
    InstallSignpostStub();
    InstallSimulatorStub();
    InstallTextDecalStub();
    InstallToxicStub();
    InstallTractDeveloperStub();
    InstallTutorialSystemStub();
    InstallTutorialTaskStub();
    InstallUndergroundNetworkToolStub();
    InstallUndergroundOccupantStub();
    InstallVertexHtConstraintStub();
    InstallView3DWinStub();
    InstallViewInputStub();
    InstallEffectToolStub();
    InstallDisasterTools();               // the disaster menu's tools; the effect tools' cursor effects replace the stub
    InstallPlaceLotStub();
    InstallQueryStub();
    InstallQueryHost();                   // the real query host replaces the stub above
    InstallSignToolStub();
    InstallWeatherStub();
    InstallNativeWeather();               // the real weather host replaces the stub above
    InstallWinAdviceListStub();
    InstallWinHtmlHosts();                // the real advice-list host replaces the stub above
    InstallWinAnimationRecorderStub();
    InstallWinBMPViewStub();
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

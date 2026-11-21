# Seams: how the recovered pieces get joined

Recovered classes never call the engine's globals (`SL::sp*`) directly. Where the original reached an unrecovered neighbour, the
code talks to a small interface (a "seam": `I*Host`, `I*Services`, ...). A test supplies a fake; the real game supplies the recovered
class. When a neighbour is finished, write the adapter, replace the fake in the app, and tick the entry below.

Rules every agent follows: (1) a seam method mirrors one original call (note its vtable slot / address in a comment);
(2) a function that depends on a seam it cannot satisfy is `recovered-partial` in `notes/ledger.csv`, never `recovered`;
(3) after adding or removing a seam run `python3 scripts/list_seams.py` and commit `docs/SEAMS.md`.

## cSTETerrainView3D (src/sc4/cSTETerrainView3D*)

The view is complete except for the neighbours below. Everything it needs from them goes through `ITerrainViewHost` and
`ITerrainDrawContext` (both in `cSTETerrainView3D.h`); `ITerrainVertBufExtn` is the optional GL vertex-buffer fast path.

| Seam method(s) | Original it stands for | Real class to hook up | Status |
|---|---|---|---|
| `ITerrainDrawContext` (all) | `SC4DrawContext` (`SetRenderState`, `SetTexture`, `SetVertexBuffer`, `DrawPrimsIndexed`, ...) | `SC4DrawContext` (58 functions left) over `cMetalGDriver` | adapter prototype in `scripts/terrain_shot.mm` |
| `DrawFrustum()` / `ViewFrustum()` / `GetVisibleEdges()` | `SC4DrawContext +0x28` = `SC4ViewUtilities` (offsets in the header comments) | `SC4ViewUtilities` | not started |
| `Pass()`, `DrawEnabled()`, `GridSuppressed()`, `RenderFlag738()` | draw context fields `+0x80`, render properties `+0x3f8` / `+0x738` | `SC4DrawContext` / `cSC4RenderProperties` | not started |
| `GetNormalLight`, `GetPointLight`, `HasLighting`, `ApplyTerrainLighting` | `SL::spLightingManager` slots `0xa0`/`0xc0`, `cSTETerrain::DoLighting` | lighting manager; `cSTETerrain::DoLighting` (see below) | not started |
| `LoadTerrainTexture`, `LoadWaterTexture`, `LoadGridTexture`, `LoadLotTexture`, `TexturesUsedInTerrainType` | `spTextureBindingFactory` (key type `0x7ab50e44`, group `0x891b0e1a`, instance = texture id + zoom * 0x100), `cSTETerrain::sTexturesUsedInTerrainType` | texture binding factory + `cSTETerrain::ReadTextureInfos` | not started |
| `GetBuffer` | `spGimexFactory` slot `0x50` (gradients `0xc9efb727`, `0x69efb6f8`, glare mask `0x69efb7da`, displacement maps `0xca5cc9c0/1`) | Gimex image factory | not started |
| `ReadTuning*`, `ReadPlumbingColor`, `HasTuningExemplar` | tuning exemplar (type `0x6534284a`, group `0x88cd66e9`, id `0xa52160f5`) read with `cSCPropertyHelp::GetPropertyValue` | resource manager + property holders | not started |
| `AddRenderLayer` / `RemoveRenderLayer` | `SL::spRenderer` slots `0x100` / `0x108` (layers 0, 2, 5; priorities 0, 1000, 0x44c) | city renderer | not started |
| `RegisterMessage` / `UnregisterMessage` | `SL::spMS2` slots `0x28` / `0x30`; ids `0x277a9364 0x278e6003 0x678e6e3a 0xa6b79601 0xa6b79611 0xc9da96ea` | message server | not started |
| `CreateSelection` -> `ITerrainSelection` | `cSTESelectedTerrainArea` | `cSTESelectedTerrainArea` | not started |
| (missing) overlay managers, `cSTEWaterRipple`, `cSTEWaterWave` | created in `Init` (4x `cSTEOverlayManager`, ripple, wave); `GetOverlayManager` is **recovered-partial** | those classes | not started: add `ITerrainViewHost::CreateOverlayManager` etc. when they exist |
| `PlumbingCellFlags`, `PlumbingQuery58/40`, `NetworkHiddenTriangles`, `CellHasOccupantType`, `GetBoolPlumbingDisplay` | `spPlumbingSim` slots `0x38/0x58/0x40`, `NetworkManager` slots `0x50/0x58`, occupant manager slot `0x148`, `cSTETerrain::sbDisplayPlumbingInfo` | plumbing simulator, network manager, occupant manager | not started |
| `WaterSurfaceDisplayState`, `RedisplayTerrain`, `PreferenceDisplayGrid`, `StorePreferenceDisplayGrid` | `cSTETerrain::GetWaterSurfaceDisplayState` / `RedisplayTerrain`, `SL::spPreferences` | `cSTETerrain` (see below), preferences | not started |
| `ColorToLuminance` | `nS3D::kColorToLuminance` (**assumed 0.299/0.587/0.114**, the real constants are computed at start-up and were not readable) | S3D static init | verify against the binary |

### What `cSTETerrain` must grow (it holds the data this view reads)
`Init`, `Shutdown`, `DoLighting(rect)`, `RedisplayTerrain(...)`, `GetView()`, `GetWaterSurfaceDisplayState` / `SetWaterSurfaceDisplayState`,
`ReadTextureInfos` + `sTexturesUsedInTerrainType`, `AssignSurfaceTexturesAndColors` (fills the three texture layers the view reads: u16 key at
vertex `+0x16/0x1a/0x1e`, alpha at `+0x18/0x1c/0x20`, colours at `+0x10` and `+0x13`). Until then the view works on a terrain whose surface bytes
the caller fills (see `scripts/terrain_shot.mm`).

### Known quirks kept on purpose (do not "fix")
* `StaticInit` tests `sWaterColorDepthGradientHt != 0` instead of the gradient pointer, so the height stays 0 and the water colour is always
  gradient row 0 (confirmed in the disassembly at `0x10074d666`).
* `DrawWaterBox` takes the ray origin's z from the second box point; `DrawMixedMeshSubsetInViewFrustum` uses the draw frustum's column table;
  `DrawTerrainMeshSubsetInViewFrustum` bounds rows with the whole frustum rows, the draw variant with the terrain rows.

### Hook-up checklist (when the neighbours exist)
1. Write `SC4DrawContext` over `cMetalGDriver`; make it implement `ITerrainDrawContext` (start from the adapter in `scripts/terrain_shot.mm`).
2. Write the `ITerrainViewHost` adapter once (it is one class) and pass it to `cSTETerrainView3D::StaticInit`.
3. Implement the missing `cSTETerrain` functions above, then delete the caller-filled surface bytes in the demo.
4. Run `scripts/run_terrain_shot.sh` (expect textured land) and `zsh scripts/runone.sh sc4_terrainview3d`.
5. Flip the ledger entry `GetOverlayManager` to `recovered` and regenerate this file.

<!-- generated:start -->
## Seam registry (generated by scripts/list_seams.py)

392 seams, 7 with no implementation at all, 40 with only a test fake.

| Seam | Declared in | Used by | Status | Implemented by |
|---|---|---|---|---|
| `IAdviceDetokenizerHost` | `src/sc4/AdviceDetokenizer.h` | src/sc4/AdviceDetokenizer.cpp | implemented | scripts/tests/sc4_advicedetokenizer_test.cpp, src/app/NativeAdvisors.cpp |
| `IAdviceHost` | `src/sc4/Advice.h` | src/sc4/Advice.cpp, src/sc4/RewardAdvice.cpp | implemented | scripts/tests/sc4_advice_subclasses_test.cpp, scripts/tests/sc4_advice_test.cpp |
| `IAdviceSubjectHost` | `src/sc4/AdviceSubject.h` | src/sc4/AdviceSubject.cpp | implemented | scripts/tests/sc4_advice_subclasses_test.cpp, scripts/tests/sc4_advicesubject_test.cpp |
| `IAdvisorConcreteHost` | `src/sc4/AdvisorConcrete.h` | src/app/NativeAdvisorStatistics.cpp, src/sc4/AdvisorConcrete.cpp | implemented | scripts/tests/sc4_advisorconcrete_test.cpp, src/app/NativeAdvisors.cpp |
| `IAdvisorHost` | `src/sc4/Advisor.h` | src/sc4/Advisor.cpp, src/sc4/AdvisorUIAdvisorWinProc.cpp, src/sc4/AdvisorUIAdvisorWinProc.h | implemented | scripts/tests/sc4_advisor_test.cpp, scripts/tests/sc4_advisorconcrete_test.cpp |
| `IAdvisorPopupTrackerHost` | `src/sc4/AdvisorPopupTracker.h` | src/sc4/AdvisorPopupTracker.cpp | implemented | scripts/tests/sc4_advisorpopuptracker_test.cpp, src/app/NativeView3DWinHost.cpp |
| `IAdvisorSystemHost` | `src/sc4/AdvisorSystem.h` | src/sc4/AdvisorSystem.cpp, src/sc4/AdvisorUIAdvisorWinProc.h | implemented | scripts/tests/sc4_advisorsystem_test.cpp, src/app/NativeAdvisors.cpp |
| `IAdvisorUIAdvisorHost` | `src/sc4/AdvisorUIAdvisorWinProc.h` | src/sc4/AdvisorUIAdvisorWinProc.cpp | implemented | scripts/tests/sc4_advisoruiadvisorwinproc_test.cpp, src/app/NativeAdvisors.cpp |
| `IAdvisorUIImageHost` | `src/sc4/AdvisorUIImageWinProc.h` | src/sc4/AdvisorUIImageWinProc.cpp | implemented | scripts/tests/sc4_advisoruiadvisorwinproc_test.cpp, scripts/tests/sc4_advisoruiimagewinproc_test.cpp |
| `IAemsEngineHost` | `src/sc4audio/AudioAEMSEventSystem.h` | src/sc4audio/AudioAEMSEventSystem.cpp | test fake only | scripts/tests/sc4_audioaemseventsystem_test.cpp |
| `IAircraftHost` | `src/sc4/Aircraft.h` | src/sc4/Aircraft.cpp, src/sc4/Airplane.cpp, src/sc4/Airplane.h ... | implemented | scripts/machine_oracle/airplane_oracle.cpp, scripts/tests/sc4_airplane_test.cpp |
| `IAirportDeveloperHost` | `src/sc4/AirportDeveloper.h` | src/sc4/AirportDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `IAmbienceLayerHost` | `src/sc4audio/AmbienceLayer.h` | src/sc4audio/AmbienceLayer.cpp | implemented | scripts/tests/sc4_audioambiencemanager_test.cpp, scripts/tests/sc4audio_ambiencelayer_test.cpp |
| `IAmbienceManagerHost` | `src/sc4audio/AudioAmbienceManager.h` | src/sc4audio/AudioAmbienceManager.cpp | implemented | scripts/tests/sc4_audioambiencemanager_test.cpp, src/app/NativeAudioAmbience.cpp |
| `IAmbienceStateHost` | `src/sc4/AmbienceStateFilters.h` | src/sc4/AmbienceStateFilters.cpp | implemented | scripts/tests/sc4_ambiencestatefilters_test.cpp, src/app/NativeAudioAmbience.cpp |
| `IAnimatedModelHost` | `src/s3d/AnimatedInstance.h` |  | implemented | scripts/tests/s3d_animatedinstance_test.cpp, src/s3d/ModelInstanceAnimatedMesh.h |
| `IAnimationCoordinatorHost` | `src/sc4/AnimationCoordinator.h` |  | implemented | scripts/tests/sc4_animationcoordinator_test.cpp, src/app/NativeAnimationCoordinator.cpp |
| `IAttractorHost` | `src/sc4/AttractorEffect.h` | src/sc4/AttractorEffect.cpp | implemented | scripts/tests/sc4_cityeffects_test.cpp, src/app/NativeEffectComponents.cpp |
| `IAudioEventHandlerHost` | `src/sc4audio/AudioEventHandler.h` |  | implemented | scripts/tests/sc4_audioeventhandler_test.cpp, src/app/NativeAudioDirector.cpp |
| `IAudioQueueApi` | `src/audio/DirectSoundMac.h` | src/audio/DirectSoundMac.cpp | test fake only | scripts/tests/audio_directsoundmac_test.cpp |
| `IAuraBudgetApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraCityApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraDisasterApi` | `src/sc4/AuraSimulator.h` | src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraGridApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraHistoryApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraHost` | `src/sc4/AuraSimulator.h` | src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraLandValueApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraLotManagerApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraOrdinanceApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraPollutionApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraResidentialApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAuraService` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp | implemented | src/app/NativeAura.cpp |
| `IAuraStrikeApi` | `src/sc4/AuraSimulator.h` | src/app/hosts/StubAura.cpp, src/sc4/AuraSimulator.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, src/app/NativeAura.cpp |
| `IAutomataAttractorHost` | `src/sc4/AutomataAttractor.h` | src/sc4/AutomataAttractor.cpp | implemented | scripts/tests/sc4_automataattractor_test.cpp, src/app/NativeAutomataGroups.cpp |
| `IAutomataControllerHost` | `src/sc4/AutomataController.h` | src/sc4/AutomataController.cpp, src/sc4/AutomataControllerGrid.cpp | implemented | scripts/tests/sc4_automataattractor_test.cpp, scripts/tests/sc4_automatacontroller_test.cpp |
| `IAutomataGeneratorHost` | `src/sc4/AutomataGenerator.h` | src/sc4/AutomataGenerator.cpp | implemented | scripts/tests/sc4_automatagenerator_test.cpp, src/app/NativeAutomataGenerator.cpp |
| `IAutomataManagerHost` | `src/sc4/AutomataManager.h` | src/app/NativeAutomataManagerHost.cpp, src/sc4/AutomataManager.cpp, src/sc4/TrafficAutomataManager.cpp | implemented | scripts/tests/sc4_automatamanager_test.cpp, src/app/hosts/StubAutomataManager.cpp |
| `IAutomataScriptHost` | `src/sc4/AutomataScriptSystem.h` |  | implemented | scripts/tests/sc4_automatascriptsystem_test.cpp, src/app/NativeAutomataScripts.cpp |
| `IAutomatonHost` | `src/sc4/Automaton.h` | src/app/NativeAutomata.h, src/app/NativeAutomataControllerHost.cpp, src/app/NativeAutomataManagerHost.cpp ... | implemented | scripts/machine_oracle/vehicle_oracle.cpp, scripts/tests/sc4_airplane_test.cpp |
| `IBaseLotDeveloperHost` | `src/sc4/LotDeveloper.h` | src/gz/SimHostRegistry.h | implemented | scripts/tests/sc4_businessunit_test.cpp, scripts/tests/sc4_lotdeveloper_test.cpp |
| `IBudgetDialogHost` | `src/sc4/BudgetDialogs.h` | src/sc4/BudgetDialogs.cpp | implemented | scripts/tests/sc4_budgetdialogs_test.cpp, src/app/hosts/StubBudgetDialog.cpp |
| `IBudgetHost` | `src/sc4/Budget.h` | src/sc4/Budget.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_budget_test.cpp |
| `IBudgetSimServices` | `src/sc4/BudgetSimulator.h` | src/sc4/BudgetSimulator.cpp | implemented | scripts/tests/sc4_budgetsimulator2_test.cpp, src/app/NativeBudgetSim.cpp |
| `IBudgetUIWinProcHost` | `src/sc4/BudgetUIWinProc.h` | src/sc4/BudgetUIWinProc.cpp | implemented | scripts/tests/sc4_budgetuiwinproc_test.cpp, src/app/NativeBudgetUi.cpp |
| `IBufferHost` | `src/gz/Buffer.h` | src/gz/Buffer.cpp | implemented | scripts/tests/gz_persistbufferresource_test.cpp, scripts/tests/gz_winfolders_test.cpp |
| `IBuildingDevelopmentHost` | `src/sc4/BuildingOccupant.h` | src/app/NativeBudgetSim.cpp, src/app/NativeQuery.cpp, src/sc4/BuildingOccupant.cpp | implemented | scripts/tests/sc4_ambiencefilters_test.cpp, src/app/NativeBuildingDevelopment.cpp |
| `IBuildingDevelopmentSimHost` | `src/sc4/BuildingDevelopmentSimulator.h` | src/sc4/BuildingDevelopmentSimulator.cpp | implemented | scripts/tests/sc4_buildingdevelopment_test.cpp, src/app/NativeBuildingDevelopment.cpp |
| `IBuildingPlacementServices` | `src/sc4/BuildingDevelopmentSimulator.h` |  | implemented | scripts/tests/sc4_buildingdevelopment_test.cpp, src/app/NativeBuildingDevelopment.cpp |
| `ICameraControlHost` | `src/sc4/CameraControl.h` | src/sc4/CameraControl.cpp | implemented | scripts/tests/sc4_cameracontrol_test.cpp, src/app/NativeCityView.cpp |
| `ICatalogViewHost` | `src/sc4/WinCatalogView.h` | src/sc4/WinCatalogView.cpp | implemented | scripts/tests/sc4_wincatalogview_test.cpp, src/app/NativeCatalogUi.cpp |
| `ICellTextureModelHost` | `src/sc4/CellTextureModel.h` | src/sc4/CellTextureModel.cpp | implemented | src/app/NativeModelMaker.cpp |
| `ICheatCodeEntryDialogHost` | `src/sc4/CheatCodeEntryDialog.h` | src/sc4/CheatCodeEntryDialog.cpp | implemented | scripts/tests/sc4_cheatcodeentrydialog_test.cpp, src/app/NativeCheats.cpp |
| `ICityDetailHost` | `src/sc4/CityDetailManager.h` | src/sc4/CityDetailManager.cpp | NOT IMPLEMENTED |  |
| `ICityHost` | `src/sc4/cSC4City.h` | src/gz/GZCOM.cpp, src/gz/GZCOM.h, src/gz/GZCOMCreators7.cpp ... | implemented | scripts/tests/gzcomcreators_core_part2.cpp, scripts/tests/sc4_city_test.cpp |
| `ICityScrollHost` | `src/sc4/ViewInputControlCityScroll.h` | src/sc4/ViewInputControlCityScroll.cpp | implemented | src/app/NativeCityView.cpp |
| `ICitySituationAdviceHost` | `src/sc4/CitySituationAdvice.h` | src/sc4/CitySituationAdvice.cpp | implemented | scripts/tests/sc4_citysituationadvice_test.cpp, src/app/hosts/StubCitySituationAdvice.cpp |
| `ICitySituationManagerHost` | `src/sc4/CitySituationManager.h` | src/sc4/CitySituationManager.cpp | test fake only | scripts/tests/sc4_citysituation_test.cpp |
| `ICivicHost` | `src/sc4/CivicBuildingSimulator.h` | src/sc4/CivicBuildingSimulator.cpp | implemented | scripts/tests/sc4_civicbuildingsimulator_test.cpp, src/app/NativeCivic.cpp |
| `IClockHost` | `src/sc4/cSC424HourClock.h` | src/sc4/cSC424HourClock.cpp, src/tools/city_probe_creators.cpp, src/tools/city_probe_hosts.h | implemented | scripts/tests/sc4_clock_test.cpp, src/app/NativeLighting.cpp |
| `ICommercialHost` | `src/sc4/CommercialSimulator.h` | src/gz/SimHostRegistry.h, src/tools/city_probe_creators.cpp, src/tools/city_probe_hosts.h | implemented | scripts/tests/sc4_commercialsimulator_test.cpp, src/app/NativeSimHosts.cpp |
| `ICompassHost` | `src/sc4/WinProcs.h` |  | test fake only | scripts/tests/sc4_winprocs_test.cpp |
| `IComplexViewFilterHost` | `src/sc4/ComplexViewOccupantFilter.h` | src/sc4/ComplexViewOccupantFilter.cpp | implemented | scripts/tests/sc4_undergroundview_test.cpp, src/app/NativeUnderground.cpp |
| `IConstructionOccupantHost` | `src/sc4/ConstructionOccupant.h` | src/sc4/ConstructionOccupant.cpp | implemented | scripts/tests/sc4_constructionoccupant_test.cpp, src/app/NativeConstruction.cpp |
| `IConstructionSuitabilityHost` | `src/sc4/BuildingDevelopmentSimulator.h` | src/sc4/BuildingDevelopmentSimulator.cpp | test fake only | scripts/tests/sc4_buildingdevelopment_test.cpp |
| `IControllerManagerHost` | `src/sc4/AutomataControllerManager.h` | src/sc4/AutomataControllerManager.cpp | implemented | scripts/tests/sc4_automatacontrollermanager_test.cpp, src/app/NativeControllerManagerHost.cpp |
| `ICookieHTTPHost` | `src/rz/CookieManager.h` | src/rz/CookieManager.cpp | test fake only | scripts/tests/rz_cookiemanager_test.cpp |
| `ICrimeHost` | `src/sc4/CrimeSimulator.h` | src/sc4/CrimeSimulator.cpp, src/tools/city_probe_creators.cpp, src/tools/city_probe_hosts.h | implemented | scripts/tests/sc4_crimesimulator_test.cpp, src/app/NativeCrime.cpp |
| `IDX7Host` | `src/render/DirectDraw7.h` | src/render/DX7Host.h, src/render/DirectDraw7.cpp | implemented | scripts/tests/render_directdraw7_test.cpp, src/render/DX7Host.cpp |
| `IDecalEffectHost` | `src/sc4/DecalEffect.h` | src/sc4/DecalEffect.cpp | implemented | scripts/tests/sc4_decaleffect_test.cpp, src/app/NativeDecalOverlay.cpp |
| `IDemandHost` | `src/sc4/cSC4Demand.h` | src/tools/city_probe_creators.cpp, src/tools/city_probe_hosts.h | implemented | scripts/tests/sc4_demand_test.cpp, src/sc4/DemandSimulator.h |
| `IDemandSimulatorHost` | `src/sc4/DemandSimulator.h` | src/app/NativeGrowthDevelopers.h, src/sc4/DemandSimulator.cpp | implemented | scripts/tests/sc4_demandsimulator_test.cpp, src/app/NativeDemandHost.cpp |
| `IDemolishToolHost` | `src/sc4/ViewInputControlDemolish.h` | src/sc4/ViewInputControlDemolish.cpp | implemented | src/app/NativeDemolishTool.cpp |
| `IDemolitionHost` | `src/sc4/Demolition.h` | src/app/NativeDemolition.h, src/gz/SimHostRegistry.h, src/tools/city_probe_creators.cpp ... | implemented | scripts/tests/sc4_demolition_test.cpp, src/app/NativeDemolition.cpp |
| `IDetokenizerHost` | `src/sc4/StringDetokenizer.h` | src/app/NativeBoot.cpp, src/sc4/StringDetokenizer.cpp | implemented | scripts/tests/sc4_stringdetokenizer_test.cpp, src/app/NativeView3DWinHost.cpp |
| `IDisasterHost` | `src/sc4/Disaster.h` | src/sc4/Disaster.cpp, src/sc4/DisasterInstanceEarthquake.cpp, src/sc4/DisasterInstanceVolcano.cpp ... | implemented | scripts/tests/sc4_disaster_test.cpp, scripts/tests/sc4_disasterinstanceriot_test.cpp |
| `IDisasterToolHost` | `src/sc4/ViewInputControlDisaster.h` | src/sc4/ViewInputControlDisaster.cpp | implemented | src/app/NativeDisasterTools.cpp |
| `IDispatchHost` | `src/sc4/DispatchManager.h` | src/sc4/DispatchManager.cpp | implemented | scripts/tests/sc4_dispatchmanager_test.cpp, src/app/NativeDispatch.cpp |
| `IDispatchToolHost` | `src/sc4/ViewInputControlDispatch.h` | src/sc4/ViewInputControlDispatch.cpp | implemented | src/app/NativeDispatchTool.cpp |
| `IDrawContextHost` | `src/gz/DrawContext.h` | src/gz/DrawContext.cpp | implemented | scripts/tests/gz_drawcontext_test.cpp, scripts/tests/gz_drawsystem_test.cpp |
| `IDrawFontRendererHost` | `src/gz/DrawFontRenderer.h` | src/gz/DrawFontRenderer.cpp | implemented | scripts/tests/gz_drawfontrenderer_test.cpp, src/app/game_main.mm |
| `IDrawSystemHost` | `src/gz/DrawSystem.h` | src/gz/DrawSystem.cpp | test fake only | scripts/tests/gz_drawsystem_test.cpp |
| `IEarthquakeHost` | `src/sc4/DisasterInstanceEarthquake.h` | src/sc4/DisasterInstanceEarthquake.cpp | implemented | src/app/NativeDisasters.cpp |
| `IEffectToolHost` | `src/sc4/ViewInputControlEffect.h` | src/sc4/ViewInputControlDisaster.cpp, src/sc4/ViewInputControlEffect.cpp | implemented | scripts/tests/sc4_viewinputcontrol_effect_test.cpp, src/app/NativeDisasterTools.cpp |
| `IEffectsCollectionHost` | `src/sc4/EffectsCollection.h` | src/sc4/EffectsCollection.cpp | test fake only | scripts/tests/sc4_effectscollection_test.cpp |
| `IEffectsHost` | `src/sc4/EffectComponents.h` | src/sc4/EffectComponents.cpp | implemented | scripts/tests/sc4_effectcomponents_test.cpp, src/app/NativeEffects.cpp |
| `IEffectsManagerHost` | `src/sc4/EffectsManager.h` | src/sc4/EffectsManager.cpp | implemented | scripts/tests/sc4_effectsmanager_test.cpp, src/app/NativeEffects.cpp |
| `IEmergencyVehicleHost` | `src/sc4/EmergencyVehicle.h` | src/sc4/EmergencyVehicle.cpp, src/sc4/FireTruck.cpp, src/sc4/FireTruck.h ... | implemented | scripts/tests/sc4_vehicle_test.cpp, src/app/NativeDispatch.cpp |
| `IEstablishCityHost` | `src/sc4/EstablishCityWinProc.h` | src/sc4/EstablishCityWinProc.cpp | implemented | src/app/NativeEstablishCity.cpp |
| `IFeatureHost` | `src/sc4/BusinessFeature.h` | src/sc4/BusinessFeature.cpp | implemented | scripts/tests/sc4_businessfeature_test.cpp, src/app/NativeAppServices.cpp |
| `IFileBrowserHost` | `src/gz/WinFileBrowser.h` |  | test fake only | scripts/tests/gz_winfilebrowser_test.cpp |
| `IFileSystemHost` | `src/gz/FileSystem.h` | src/gz/FileSystem.cpp | implemented | scripts/tests/gz_filesystem_test.cpp, src/app/NativeBoot.cpp |
| `IFireHost` | `src/sc4/FireProtectionSimulator.h` | src/app/NativeDispatch.cpp, src/gz/SimHostRegistry.h, src/sc4/FireProtectionSimulator.cpp | implemented | scripts/tests/sc4_fireprotectionsimulator_test.cpp, src/app/NativeSimHosts.cpp |
| `IFireInstanceHost` | `src/sc4/DisasterInstanceFire.h` | src/sc4/DisasterInstanceFire.cpp | implemented | src/app/NativeDisasters.cpp |
| `IFireTruckHost` | `src/sc4/FireTruck.h` | src/app/NativeFireCrew.cpp, src/sc4/FireTruck.cpp, src/tools/city_probe_creators8.cpp ... | implemented | scripts/tests/sc4_vehicle_test.cpp, src/app/NativeDispatch.cpp |
| `IFlammabilityHost` | `src/sc4/cSC4FlammabilitySimulator.h` | src/app/NativeSimHosts.cpp, src/gz/SimHostRegistry.h, src/sc4/cSC4FlammabilitySimulator.cpp | implemented | scripts/tests/sc4_flammability_test.cpp, src/app/NativeFlammabilityHost.cpp |
| `IFloraHost` | `src/sc4/FloraOccupant.h` | src/app/NativeFlora.h, src/sc4/FloraOccupant.cpp | implemented | scripts/tests/sc4_floraoccupant_test.cpp, src/app/NativeFlora.cpp |
| `IFloraSimulatorHost` | `src/sc4/FloraSimulator.h` | src/app/NativeFlora.h, src/sc4/FloraSimulator.cpp | implemented | scripts/tests/sc4_florasimulator_test.cpp, src/app/NativeFlora.cpp |
| `IFloraToolHost` | `src/sc4/ViewInputControlFlora.h` | src/sc4/ViewInputControlFlora.cpp | implemented | src/app/NativeFlora.cpp |
| `IFontHost` | `src/gz/FontServices.h` | src/gz/GZFont.cpp, src/gz/GZFont.h | implemented | scripts/tests/gz_gzfont_test.cpp, src/app/NativeFonts.h |
| `IFontSysHost` | `src/gz/FontSys.h` | src/gz/FontSys.cpp | test fake only | scripts/tests/gz_fontsys_test.cpp |
| `IFoundationModelHost` | `src/sc4/FoundationModel.h` | src/sc4/FoundationModel.cpp | implemented | scripts/tests/sc4_foundationmodel_test.cpp, src/app/NativeLotWalls.cpp |
| `IGLApi` | `src/render/GLApi.h` | src/render/D3DDevice7.cpp, src/render/D3DDevice7.h, src/render/D3DVertexBuffer7.cpp ... | implemented | scripts/tests/gl_trace.h, src/render/GLApi.cpp |
| `IGLCacheHost` | `src/gz/BufferSimGLCache.h` | src/gz/BufferSimGLCache.cpp | implemented | scripts/tests/gz_buffersimglcache_test.cpp, src/gz/DrawContextRenderToSimGL.h |
| `IGZCOMHost` | `src/gz/cGZCOM.h` |  | implemented | scripts/tests/gz_com_test.cpp, src/gz/GZFramework.h |
| `IGetawayVanHost` | `src/sc4/SpecialVehicles.h` | src/sc4/SpecialVehicles.cpp | implemented | scripts/tests/sc4_vehicle_test.cpp, src/app/NativeDispatch.cpp |
| `IGlyphTextureHost` | `src/sc4/GlyphTextureManager.h` | src/sc4/GlyphTextureManager.cpp | implemented | scripts/tests/sc4_glyphtexturemanager_test.cpp, src/app/NativeGlyphTextures.cpp |
| `IGraphHost` | `src/gz/Graph.h` | src/gz/BarGraph.cpp, src/gz/Graph.cpp, src/gz/LineGraph.cpp ... | implemented | scripts/tests/fake_graph.h, src/app/NativeGraphsView.cpp |
| `IGraphicSystemHost` | `src/gz/GraphicSystemW32.h` | src/gz/GraphicSystemW32.cpp | test fake only | scripts/tests/gz_graphicsystemw32_test.cpp |
| `IGraphsViewHost` | `src/sc4/GraphsViewWinProc.h` |  | implemented | scripts/tests/sc4_graphsviewwinproc_test.cpp, src/app/NativeGraphsView.cpp |
| `IGrowthAuraApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthBuildingDevelopmentApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthCityApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp, src/sc4/GrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthCommercialApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthCrimeApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthDemandApi` | `src/sc4/GrowthDeveloper.h` | src/app/NativeTractDevelopment.cpp, src/app/NativeZoneDeveloperHost.cpp, src/sc4/GrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthDemandSimulatorApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp, src/sc4/GrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthDeveloperApi` | `src/sc4/TractDeveloper.h` | src/app/hosts/StubTractDeveloper.cpp, src/sc4/TractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `IGrowthDeveloperHost` | `src/sc4/GrowthDeveloper.h` | src/app/NativeGrowthDevelopers.h, src/sc4/GrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthDeveloperService` | `src/sc4/GrowthDeveloper.h` | src/sc4/PropDeveloper.h | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthLandValueApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthNeighborsApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthPollutionApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthResidentialApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthTrafficApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthTrafficDensityApi` | `src/sc4/GrowthDeveloper.h` | src/sc4/GrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IGrowthZoneManagerApi` | `src/sc4/GrowthDeveloper.h` | src/app/hosts/StubGrowthDeveloper.cpp | implemented | scripts/tests/sc4_growthdeveloper_test.cpp, src/app/NativeGrowthDevelopers.cpp |
| `IHardwareRenderHost` | `src/gz/DrawContextRenderToHardware.h` |  | implemented | scripts/tests/gz_drawcontextrendertohardware_test.cpp, src/gz/DrawContextRenderToSimGL.h |
| `IHelicopterHost` | `src/sc4/Helicopter.h` | src/sc4/Helicopter.cpp | implemented | scripts/tests/sc4_helicopter_test.cpp, src/app/hosts/StubHelicopter.cpp |
| `IHitDirectorHost` | `src/audio/HitManDirector.h` | src/audio/HitManDirector.cpp | implemented | scripts/tests/audio_hitmandirector_test.cpp, src/sc4audio/AudioCOMDirector.h |
| `IHitIniHost` | `src/audio/HitIniFile.h` | src/audio/HitIniFile.cpp, src/sc4audio/AudioCOMDirector.cpp, src/sc4audio/AudioCOMDirector.h | implemented | scripts/tests/audio_hitinifile_test.cpp, src/app/NativeAudioDirector.cpp |
| `IHtmlDocumentServices` | `src/html/HTMLTypes.h` | src/html/HTMLElements.cpp, src/html/HTMLElements.h | implemented | scripts/tests/html_elements_test.cpp, src/html/HTMLDocument.h |
| `IIMEProxyHost` | `src/gz/WinIMEProxy.h` | src/gz/WinIMEProxy.cpp | test fake only | scripts/tests/gz_winimeproxy_test.cpp |
| `ILabelToolHost` | `src/sc4/ViewInputControlLabelTool.h` | src/sc4/ViewInputControlLabelTool.cpp | implemented | scripts/tests/sc4_viewinputcontrol_labeltool_test.cpp, src/app/NativeSignLabel.cpp |
| `ILandValueHost` | `src/sc4/LandValueSimulator.h` | src/gz/SimHostRegistry.h, src/tools/city_probe_creators.cpp, src/tools/city_probe_hosts.h | implemented | scripts/tests/sc4_landvaluesimulator_test.cpp, src/app/NativeSimHosts.cpp |
| `ILandfillBudgetApi` | `src/sc4/LandfillDeveloper.h` | src/app/hosts/StubLandfillDeveloper.cpp | implemented | scripts/tests/sc4_landfilldeveloper_test.cpp, src/app/NativeLandfill.cpp |
| `ILandfillCityApi` | `src/sc4/LandfillDeveloper.h` | src/app/hosts/StubLandfillDeveloper.cpp | implemented | scripts/tests/sc4_landfilldeveloper_test.cpp, src/app/NativeLandfill.cpp |
| `ILandfillDeveloperHost` | `src/sc4/LandfillDeveloper.h` | src/sc4/LandfillDeveloper.cpp | implemented | scripts/tests/sc4_landfilldeveloper_test.cpp, src/app/NativeLandfill.cpp |
| `ILandfillPollutionApi` | `src/sc4/LandfillDeveloper.h` | src/app/hosts/StubLandfillDeveloper.cpp | implemented | scripts/tests/sc4_landfilldeveloper_test.cpp, src/app/NativeLandfill.cpp |
| `ILandfillTerrainApi` | `src/sc4/LandfillDeveloper.h` | src/app/hosts/StubLandfillDeveloper.cpp | implemented | scripts/tests/sc4_landfilldeveloper_test.cpp, src/app/NativeLandfill.cpp |
| `ILandfillZoneHost` | `src/sc4/LandfillZone.h` | src/app/hosts/StubLandfillDeveloper.cpp, src/sc4/LandfillDeveloper.cpp, src/sc4/LandfillDeveloper.h ... | implemented | scripts/tests/sc4_landfilldeveloper_test.cpp, scripts/tests/sc4_landfillzone_test.cpp |
| `ILavaHost` | `src/sc4/VolcanoLava.h` |  | implemented | scripts/tests/sc4_volcanolava_test.cpp, src/app/NativeVolcanoLava.cpp |
| `ILightingHost` | `src/sc4/LightingManager.h` | src/app/NativeLighting.h, src/sc4/LightingManager.cpp | implemented | scripts/tests/sc4_lightingmanager_test.cpp, src/app/NativeLighting.cpp |
| `ILoadHeightFieldHost` | `src/sc4/WinLoadHeightField.h` | src/sc4/WinLoadHeightField.cpp | implemented | scripts/tests/sc4_winloadheightfield_test.cpp, src/app/hosts/StubLoadHeightField.cpp |
| `ILocalGridHost` | `src/sc4/LocalGridHelper.h` | src/sc4/LocalGridHelper.cpp, src/sc4/ViewInputControlNetworkTool.cpp | implemented | scripts/tests/sc4_localgridhelper_test.cpp, src/app/NativeLocalGrid.cpp |
| `ILogOutputBackEndHost` | `src/gz/LogService.h` | src/gz/LogService.cpp | test fake only | scripts/tests/gz_logservice_test.cpp |
| `ILogServiceHost` | `src/gz/LogService.h` | src/gz/LogService.cpp | test fake only | scripts/tests/gz_logservice_test.cpp |
| `ILotBaseTextureHost` | `src/sc4/LotBaseTextureOccupant.h` | src/sc4/LotBaseTextureOccupant.cpp | implemented | scripts/tests/sc4_lotbasetextureoccupant_test.cpp, src/app/NativeModelMaker.cpp |
| `ILotBaseTextureServices` | `src/sc4/LotDeveloper.h` | src/sc4/LotDeveloper.cpp | implemented | src/app/NativeZoneDeveloperHost.cpp |
| `ILotConfigurationHost` | `src/sc4/LotConfiguration.h` | src/sc4/LotConfiguration.cpp | implemented | scripts/tests/sc4_lotconfiguration_test.cpp, src/app/NativeLotConfigHost.cpp |
| `ILotConfigurationManagerHost` | `src/sc4/LotConfigurationManager.h` | src/sc4/LotConfigurationManager.cpp | implemented | scripts/tests/sc4_lotconfigurationmanager_test.cpp, src/app/NativeLotConfigHost.cpp |
| `ILotConstructionServices` | `src/sc4/LotDeveloper.h` | src/sc4/LotDeveloper.cpp | implemented | scripts/tests/sc4_businessunit_test.cpp, src/app/NativeLotConstruction.cpp |
| `ILotDailyServices` | `src/sc4/LotDeveloper.h` | src/sc4/LotDeveloper.cpp | implemented | src/app/NativeLotConstruction.cpp |
| `ILotGateServices` | `src/sc4/LotDeveloper.h` |  | implemented | scripts/tests/sc4_lotdeveloper_test.cpp, src/app/NativeLotConstruction.cpp |
| `ILotHost` | `src/sc4/Lot.h` | src/sc4/Lot.cpp, src/sc4/LotDeveloper.cpp | implemented | scripts/tests/sc4_aurasimulator_test.cpp, scripts/tests/sc4_businessunit_test.cpp |
| `ILotRetainingWallHost` | `src/sc4/LotRetainingWallOccupant.h` | src/sc4/LotRetainingWallOccupant.cpp | implemented | scripts/tests/sc4_lotretainingwalloccupant_test.cpp, src/app/NativeLotWalls.cpp |
| `ILotRetainingWallModelHost` | `src/sc4/LotRetainingWallModel.h` | src/sc4/LotRetainingWallModel.cpp | implemented | scripts/tests/sc4_lotretainingwallmodel_test.cpp, src/app/NativeLotWalls.cpp |
| `ILuaApi` | `src/lua/Lua5Thread.h` | src/gz/LuaScriptServer.h, src/lua/Lua5.cpp, src/lua/Lua5.h ... | implemented | scripts/tests/lua_lua5thread_test.cpp, scripts/tests/lua_minilua.h |
| `ILuaCommandGeneratorApi` | `src/gz/LuaScriptServer.h` |  | test fake only | scripts/tests/gz_luascriptserver_test.cpp |
| `ILuaScriptServerHost` | `src/gz/LuaScriptServer.h` | src/gz/LuaScriptServer.cpp | test fake only | scripts/tests/gz_luascriptserver_test.cpp |
| `IMacToolsHost` | `src/platform/MacTools.h` | src/platform/MacTools.cpp | test fake only | scripts/tests/mactools_test.cpp |
| `IMeshBuilderHost` | `src/sc4/DrawContext.h` | src/sc4/DrawContext.cpp | test fake only | scripts/tests/sc4_drawcontext_test.cpp |
| `IMeshMaterialHost` | `src/sc4/ViewUtilities.h` | src/sc4/ViewUtilities.cpp | test fake only | scripts/tests/sc4_viewutilities_test.cpp |
| `IMeshPickHost` | `src/sc4/ViewUtilities.h` | src/sc4/ViewUtilities.cpp | test fake only | scripts/tests/sc4_viewutilities_test.cpp |
| `IMiniMapHost` | `src/sc4/WinMiniMap.h` | src/sc4/WinMiniMap.cpp | implemented | scripts/tests/sc4_winminimap_test.cpp, src/app/NativeMiniMap.cpp |
| `IModelInstanceHost` | `src/s3d/ModelInstance.h` | src/s3d/ModelInstance.cpp | implemented | scripts/tests/s3d_modelinstance_test.cpp, src/app/NativeModelMaker.cpp |
| `IModelMakerHost` | `src/sc4/ModelMaker.h` | src/sc4/ModelMaker.cpp | implemented | scripts/tests/sc4_modelmaker_test.cpp, src/app/NativeModelMaker.cpp |
| `IModelParticlesHost` | `src/sc4/ModelBasedParticlesEffect.h` | src/sc4/ModelBasedParticlesEffect.cpp | implemented | src/app/NativeEffects.cpp |
| `IMySimAdviceHost` | `src/sc4/MySimAdvice.h` | src/sc4/MySimAdvice.cpp | implemented | scripts/tests/sc4_advice_subclasses_test.cpp, src/app/hosts/StubMySimAdvice.cpp |
| `IMySimAgentHost` | `src/sc4/MySimAgentSimulator.h` | src/gz/GZCOM.h, src/gz/gzcom_creators/cSC4MySimAgentSimulator.cpp, src/sc4/MySimAgentSimulator.cpp | implemented | scripts/tests/sc4_mysimagent_test.cpp, src/app/NativeTerrainHost.cpp |
| `IMySimDispatchHost` | `src/sc4/MySimDispatch.h` | src/sc4/MySimDispatch.cpp | implemented | scripts/tests/sc4_mysimdispatch_test.cpp, src/app/hosts/StubMySimDispatch.cpp |
| `IMySimFilterHost` | `src/sc4/MySimFilters.h` | src/sc4/MySimFilters.cpp | implemented | scripts/tests/sc4_mysimfilters_test.cpp, src/app/hosts/StubMySimFilter.cpp |
| `IMySimHost` | `src/sc4/MySim.h` | src/sc4/MySim.cpp | implemented | scripts/tests/sc4_mysim_test.cpp, src/app/hosts/StubMySim.cpp |
| `IMySimWinProcHost` | `src/sc4/MySimWinProc.h` | src/sc4/MySimWinProc.cpp | implemented | scripts/tests/sc4_mysimwinproc_test.cpp, src/app/NativeCityUi.cpp |
| `INSC4UIHost` | `src/sc4/SC4UI.h` | src/sc4/SC4UIWindows.cpp, src/sc4/WinRegionScreen.cpp | implemented | scripts/tests/sc4_ui_helpers_test.cpp, src/app/game_main.mm |
| `INationHost` | `src/sc4/Nation.h` | src/app/NativeRegion.h, src/app/NativeRegionScreen.cpp, src/sc4/Nation.cpp | implemented | scripts/tests/sc4_nation_test.cpp, src/app/NativeRegion.cpp |
| `INeighborConnectionHost` | `src/sc4/NeighborConnection.h` | src/sc4/NeighborConnection.cpp | implemented | scripts/tests/sc4_neighborconnection_test.cpp, scripts/tests/sc4_neighborssimulator_test.cpp |
| `INeighborDealHost` | `src/sc4/NeighborDeal.h` | src/sc4/NeighborDeal.cpp | implemented | scripts/tests/sc4_neighbordeal_test.cpp, scripts/tests/sc4_neighborssimulator_test.cpp |
| `INeighborsBudgetApi` | `src/sc4/BudgetUIWinProc.h` | src/app/NativeBudgetUi.cpp, src/app/hosts/StubBudgetUIWinProc.cpp, src/sc4/BudgetUIWinProc.cpp | test fake only | scripts/tests/sc4_budgetuiwinproc_test.cpp |
| `INeighborsHost` | `src/sc4/NeighborsSimulator.h` | src/sc4/NeighborsSimulator.cpp, src/tools/city_probe_creators.cpp, src/tools/city_probe_hosts.h | implemented | scripts/tests/sc4_neighborssimulator_test.cpp, src/app/NativeRegion.cpp |
| `INetworkInputToolHost` | `src/sc4/ViewInputControlNetworkTool.h` | src/sc4/ViewInputControlNetworkTool.cpp | implemented | src/app/NativeNetworkTool.cpp |
| `INetworkLotManagerHost` | `src/sc4/NetworkLotManager.h` | src/app/NativeNetworkLots.h, src/sc4/NetworkLotManager.cpp | implemented | scripts/tests/sc4_networklotmanager_test.cpp, src/app/NativeNetworkLots.cpp |
| `INetworkManagerHost` | `src/sc4/NetworkManager.h` | src/sc4/NetworkManager.cpp | implemented | scripts/tests/sc4_networkmanager_test.cpp, src/app/hosts/StubNetworkManager.cpp |
| `INetworkModelHost` | `src/sc4/NetworkOccupantModel.h` | src/sc4/NetworkOccupantModel.cpp | implemented | src/app/NativeModelMaker.cpp |
| `INetworkOccupantHost` | `src/sc4/NetworkOccupant.h` | src/app/NativeNetworkLots.h, src/sc4/NetworkOccupant.cpp | implemented | scripts/tests/sc4_networkoccupant_test.cpp, scripts/tests/sc4_networkoccupantwithprebuiltmodel_test.cpp |
| `INetworkOccupantPathHost` | `src/sc4/NetworkOccupant.h` | src/sc4/NetworkOccupant.cpp | implemented | src/app/NativeNetworkManager.cpp |
| `INetworkPreviewHost` | `src/sc4/NetworkPreview.h` | src/sc4/NetworkPreview.cpp | implemented | src/app/NativeNetworkTool.cpp |
| `INetworkToolHost` | `src/sc4/NetworkTool.h` | src/sc4/NetworkTool.cpp, src/sc4/NetworkToolSolve.cpp, src/sc4/PipeTool.cpp | implemented | scripts/tests/sc4_networktool_test.cpp, scripts/tests/sc4_powerlinetool_test.cpp |
| `INetworkToolUIHost` | `src/sc4/NetworkToolUI.h` | src/sc4/NetworkToolUI.cpp | implemented | src/app/NativeNetworkTool.cpp |
| `IObliterateCityHost` | `src/sc4/ObliterateCity.h` | src/app/NativeObliterate.h, src/sc4/ObliterateCity.cpp | implemented | scripts/tests/sc4_obliteratecity_test.cpp, src/app/NativeObliterate.cpp |
| `IOccupantGroupHost` | `src/sc4/OccupantGroupTemplate.h` | src/sc4/OccupantGroupTemplate.cpp | implemented | scripts/tests/sc4_occupantgrouptemplate_test.cpp, src/app/NativeAutomataGroups.cpp |
| `IOrdinanceBudgetApi` | `src/sc4/BudgetUIWinProc.h` | src/app/hosts/StubBudgetUIWinProc.cpp, src/sc4/BudgetUIWinProc.cpp | implemented | scripts/tests/sc4_budgetuiwinproc_test.cpp, src/app/NativeBudgetUi.cpp |
| `IOrdinanceHost` | `src/sc4/Ordinance.h` | src/sc4/Ordinance.cpp | implemented | scripts/tests/sc4_ordinance_test.cpp, scripts/tests/sc4_ordinancetypes_test.cpp |
| `IOrdinanceSimHost` | `src/sc4/OrdinanceTypes.h` | src/sc4/OrdinanceTypes.cpp | implemented | scripts/tests/sc4_ordinancetypes_test.cpp, src/app/NativeOrdinance.cpp |
| `IOverlayHost` | `src/sc4/OverlayManager.h` | src/sc4/OverlayManager.cpp | implemented | scripts/tests/sc4_decaleffect_test.cpp, scripts/tests/sc4_overlaymanager_test.cpp |
| `IOverlayShadowService` | `src/sc4/OverlayManager.h` | src/app/NativeDecalOverlay.cpp, src/app/hosts/StubOverlay.cpp, src/sc4/OverlayManager.cpp | implemented | scripts/tests/sc4_overlaymanager_test.cpp, src/app/NativeShadowOverlay.cpp |
| `IPathFinderHost` | `src/sc4/PathFinder.h` | src/sc4/PathFinder.cpp | implemented | scripts/tests/sc4_pathfinder_test.cpp, src/sc4/TrafficCycle.cpp |
| `IPathInfoServices` | `src/sc4/PathInfo.h` | src/sc4/PathInfo.cpp | test fake only | scripts/tests/sc4_pathinfo_test.cpp |
| `IPatrolCarHost` | `src/sc4/PatrolCar.h` | src/app/NativeRiot.cpp, src/app/NativeRiotPolice.cpp, src/sc4/PatrolCar.cpp ... | implemented | scripts/tests/sc4_vehicle_test.cpp, src/app/NativeDispatch.cpp |
| `IPedestrianHost` | `src/sc4/Pedestrian.h` | src/app/NativeFireCrew.cpp, src/app/NativeRiotPolice.cpp, src/app/NativeSimHosts.cpp ... | implemented | scripts/machine_oracle/vehicle_oracle.cpp, scripts/tests/sc4_vehicle_test.cpp |
| `IPedestrianManagerHost` | `src/sc4/PedestrianManager.h` | src/sc4/PedestrianManager.cpp | NOT IMPLEMENTED |  |
| `IPersistBufferHost` | `src/gz/PersistBufferResource.h` | src/gz/PersistBufferResourceFactory.cpp, src/gz/PersistBufferResourceFactory.h | implemented | scripts/tests/gz_persistbufferresource_test.cpp, scripts/tests/gz_persistbufferresourcefactory_test.cpp |
| `IPersistHost` | `src/gz/PersistStreams.h` | src/gz/PersistASCIIWriter.cpp, src/gz/PersistBinaryReader.cpp, src/gz/PersistBinaryWriter.cpp ... | test fake only | scripts/tests/gz_persist_test.cpp |
| `IPipeBurstHost` | `src/sc4/PipeBurstInstance.h` | src/sc4/PipeBurstInstance.cpp | implemented | scripts/tests/sc4_pipeburstinstance_test.cpp, src/app/NativeDisasters.cpp |
| `IPlaceLotHost` | `src/sc4/ViewInputControlPlaceLot.h` | src/app/NativePlaceLot.h, src/sc4/ViewInputControlPlaceLot.cpp | implemented | scripts/tests/sc4_viewinputcontrolplacelot_test.cpp, src/app/NativePlaceLot.cpp |
| `IPlaceableHost` | `src/sc4/PlaceableObject.h` | src/sc4/PlaceableObject.cpp | implemented | scripts/tests/sc4_placeableobject_test.cpp, src/app/NativeModelMaker.cpp |
| `IPlumbingHost` | `src/sc4/PlumbingSimulator.h` | src/app/NativePlumbing.h, src/gz/SimHostRegistry.h, src/sc4/PlumbingSimulator.cpp | implemented | scripts/tests/sc4_plumbingsimulator_test.cpp, src/app/NativePlumbing.cpp |
| `IPoliceHost` | `src/sc4/PoliceSimulator.h` | src/app/NativeDispatch.cpp, src/gz/SimHostRegistry.h, src/sc4/PoliceSimulator.cpp | implemented | scripts/tests/sc4_policesimulator_test.cpp, src/app/NativeSimHosts.cpp |
| `IPollutionHost` | `src/sc4/PollutionSimulator.h` | src/gz/SimHostRegistry.h, src/sc4/PollutionSimulator.cpp | implemented | scripts/tests/sc4_pollutionsimulator_test.cpp, src/app/NativeSimHosts.cpp |
| `IPollutionServices` | `src/sc4/PollutionSimulator.h` |  | test fake only | scripts/tests/sc4_pollutionsimulator_test.cpp |
| `IPortAdvisorSystemApi` | `src/sc4/PortDeveloper.h` | src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortBudgetApi` | `src/sc4/PortDeveloper.h` | src/app/hosts/StubPortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortBuildingDevelopmentApi` | `src/sc4/PortDeveloper.h` | src/app/hosts/StubPortDeveloper.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortCityApi` | `src/sc4/PortDeveloper.h` | src/app/NativePorts.cpp, src/app/hosts/StubPortDeveloper.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortDemandApi` | `src/sc4/PortDeveloper.h` | src/app/hosts/StubPortDeveloper.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortDeveloperHost` | `src/sc4/PortDeveloper.h` | src/app/NativePorts.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortDeveloperService` | `src/sc4/PortDeveloper.h` |  | NOT IMPLEMENTED |  |
| `IPortLotConfigurationManagerApi` | `src/sc4/PortDeveloper.h` | src/app/hosts/StubPortDeveloper.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortLotDeveloperApi` | `src/sc4/PortDeveloper.h` | src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortLotManagerApi` | `src/sc4/PortDeveloper.h` | src/app/hosts/StubPortDeveloper.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortResourceApi` | `src/sc4/PortDeveloper.h` | src/app/hosts/StubPortDeveloper.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortSimulatorApi` | `src/sc4/PortDeveloper.h` | src/app/hosts/StubPortDeveloper.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPortZoneManagerApi` | `src/sc4/PortDeveloper.h` | src/app/hosts/StubPortDeveloper.cpp, src/sc4/PortDeveloper.cpp | implemented | scripts/tests/sc4_airportdeveloper_test.cpp, scripts/tests/sc4_portdeveloper_test.cpp |
| `IPowerHost` | `src/sc4/PowerSimulator.h` | src/app/NativePower.h, src/gz/SimHostRegistry.h, src/sc4/PowerSimulator.cpp | implemented | scripts/tests/sc4_powersimulator_test.cpp, src/app/NativePower.cpp |
| `IPowerLineToolHost` | `src/sc4/PowerLineTool.h` | src/sc4/PowerLineTool.cpp | implemented | scripts/tests/sc4_powerlinetool_test.cpp, src/app/hosts/StubPowerLineTool.cpp |
| `IPreBuiltModelOccupantHost` | `src/sc4/NetworkOccupantWithPreBuiltModel.h` | src/sc4/NetworkOccupantWithPreBuiltModel.cpp | implemented | scripts/tests/sc4_networkoccupantwithprebuiltmodel_test.cpp, src/app/hosts/StubPreBuiltModelOccupant.cpp |
| `IPrimitiveHost` | `src/render/gd/PrimitiveManager.h` |  | test fake only | scripts/tests/render_primitivemanager_test.cpp |
| `IPrimitiveManagerHost` | `src/render/gd/SGLDX7Driver.h` | src/render/gd/SGLDX7Driver.cpp | implemented | scripts/tests/render_sgldx7driver_test.cpp, src/render/gd/PrimitiveManager.h |
| `IPropBuildingDevelopmentApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropCityApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropClockApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropControllerHost` | `src/sc4/PropOccupantAutomataController.h` | src/sc4/PropOccupantAutomataController.cpp | implemented | scripts/tests/sc4_propoccupantautomatacontroller_test.cpp, src/app/hosts/StubPropController.cpp |
| `IPropDeveloperHost` | `src/sc4/PropDeveloper.h` | src/sc4/PropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropDeveloperService` | `src/sc4/PropDeveloper.h` | src/app/NativePropDeveloper.cpp, src/app/hosts/StubPropDeveloper.cpp | NOT IMPLEMENTED |  |
| `IPropHost` | `src/sc4/PropOccupant.h` | src/sc4/PropManager.cpp, src/sc4/PropOccupant.cpp | implemented | scripts/tests/sc4_propmanager_test.cpp, scripts/tests/sc4_propoccupant_test.cpp |
| `IPropLightingApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropLotDeveloperApi` | `src/sc4/PropDeveloper.h` | src/sc4/PropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropLotManagerApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp, src/sc4/PropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropManagerApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp, src/sc4/PropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropManagerHost` | `src/sc4/PropManager.h` | src/sc4/PropManager.cpp | implemented | scripts/tests/sc4_propmanager_test.cpp, src/app/NativeProps.cpp |
| `IPropNetworkLotManagerApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp, src/sc4/PropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropOccupantManagerApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropPowerApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropSimulatorApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp, src/sc4/PropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropTerrainApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropView3DApi` | `src/sc4/PropDeveloper.h` | src/app/hosts/StubPropDeveloper.cpp, src/sc4/PropDeveloper.cpp | implemented | scripts/tests/sc4_propdeveloper_test.cpp, src/app/NativePropDeveloper.cpp |
| `IPropVisualEffectHost` | `src/sc4/PropOccupantVisualEffect.h` | src/sc4/PropOccupantVisualEffect.cpp | implemented | scripts/tests/sc4_propoccupantvisualeffect_test.cpp, src/sc4/EffectsManager.h |
| `IPropertyViewHost` | `src/sc4/PropertyView.h` | src/sc4/PropertyView.cpp | implemented | scripts/tests/sc4_propertyview_test.cpp, src/app/hosts/StubPropertyView.cpp |
| `IProtectionHost` | `src/sc4/ProtectionSimulator.h` | src/sc4/ProtectionSimulator.cpp | implemented | scripts/tests/sc4_protectionsimulator_test.cpp, src/sc4/FireProtectionSimulator.h |
| `IPylonOccupantHost` | `src/sc4/PylonOccupant.h` | src/sc4/PylonOccupant.cpp | implemented | scripts/tests/sc4_pylonoccupant_test.cpp, src/app/NativeDispatchTool.cpp |
| `IQueryHost` | `src/sc4/ViewInputControlQuery.h` | src/sc4/ViewInputControlQuery.cpp | implemented | scripts/tests/sc4_viewinputcontrolquery_test.cpp, src/app/NativeQuery.cpp |
| `IRadioHost` | `src/rz/Radio.h` |  | implemented | scripts/tests/rz_radio_test.cpp, src/rz/Radio.cpp |
| `IRadioStationHost` | `src/rz/Radio.h` | src/rz/DiskRadioStation.cpp, src/rz/Radio.cpp | implemented | scripts/tests/rz_diskradiostation_test.cpp, scripts/tests/rz_radio_test.cpp |
| `IRegEffectsHost` | `src/sc4/ModelRegEffects.h` | src/sc4/ModelRegEffects.cpp | implemented | scripts/tests/sc4_modelregeffects_test.cpp, src/app/NativeModelRegEffects.cpp |
| `IRegionCloudsHost` | `src/sc4/WinRegionClouds.h` | src/sc4/WinRegionClouds.cpp | implemented | scripts/tests/sc4_winregionclouds_test.cpp, src/app/NativeRegionExtras.cpp |
| `IRegionFrameService` | `src/sc4/WinRegionScreen.h` |  | implemented | scripts/tests/sc4_winregionscreen_test.cpp, src/app/NativeRegionScreen.cpp |
| `IRegionHost` | `src/sc4/Region.h` | src/app/NativeRegion.h, src/sc4/Region.cpp | implemented | scripts/tests/sc4_nation_test.cpp, scripts/tests/sc4_region_test.cpp |
| `IRegionScreenHost` | `src/sc4/WinRegionScreen.h` | src/app/NativeRegionScreen.h, src/sc4/WinRegionScreen.cpp | implemented | scripts/tests/sc4_winregionscreen_test.cpp, src/app/NativeRegionScreen.cpp |
| `IRegionScrollHost` | `src/sc4/WinRegionScroll.h` | src/sc4/WinRegionScroll.cpp | implemented | scripts/tests/sc4_winregionscroll_test.cpp, src/app/NativeRegionExtras.cpp |
| `IRegionViewHost` | `src/sc4/WinRegionView.h` | src/app/NativeRegionScreen.h, src/sc4/WinRegionScreen.cpp, src/sc4/WinRegionView.cpp | implemented | scripts/tests/sc4_winregionview_test.cpp, src/app/NativeRegionScreen.cpp |
| `IRegionalCityHost` | `src/sc4/RegionalCity.h` | src/app/NativeRegion.h, src/sc4/NeighborsSimulator.h, src/sc4/RegionalCity.cpp | implemented | scripts/tests/sc4_regionalcity_test.cpp, src/app/NativeRegion.cpp |
| `IRender3DHost` | `src/sc4/Render3D.h` | src/app/NativeLighting.h, src/sc4/Render3D.cpp | implemented | scripts/tests/sc4_3drender_test.cpp, src/app/NativeRender3DHost.cpp |
| `IResidentialHost` | `src/sc4/ResidentialSimulator.h` | src/gz/SimHostRegistry.h, src/sc4/ResidentialSimulator.cpp | implemented | scripts/tests/sc4_residentialpopulation_test.cpp, scripts/tests/sc4_residentialsimulator_test.cpp |
| `IResizableOutlineHost` | `src/sc4/WinResizableOutline.h` | src/sc4/WinResizableOutline.cpp | implemented | scripts/tests/sc4_winresizableoutline_test.cpp, src/app/hosts/StubResizableOutline.cpp |
| `IResourceCatalogHost` | `src/sc4/ResourceCatalogView.h` | src/sc4/ResourceCatalogView.cpp | test fake only | scripts/tests/sc4_resourcecatalogview_test.cpp |
| `IRewardAdviceHost` | `src/sc4/RewardAdvice.h` | src/sc4/RewardAdvice.cpp | implemented | scripts/tests/sc4_advice_subclasses_test.cpp, src/app/NativeAdvisors.cpp |
| `IRiotInstanceHost` | `src/sc4/DisasterInstanceRiot.h` | src/sc4/DisasterInstanceRiot.cpp | implemented | scripts/tests/sc4_disasterinstanceriot_test.cpp, src/app/NativeRiot.cpp |
| `IRiotPoliceHost` | `src/sc4/RiotPolice.h` | src/sc4/RiotPolice.cpp | implemented | src/app/NativeRiotPolice.cpp |
| `ISCLuaHost` | `src/lua/SCLua.h` | src/lua/SCLua.cpp | implemented | scripts/tests/lua_luasystem_test.cpp, src/lua/SCLuaInstance.cpp |
| `ISLBufferHost` | `src/sc4/SLBuffer.h` | src/sc4/SLBuffer.cpp | implemented | scripts/tests/sc4_advice_subclasses_test.cpp, scripts/tests/sc4_pylonoccupant_test.cpp |
| `ISLStringHost` | `src/sc4/SLString.h` | src/app/NativeUi.cpp, src/sc4/SLString.cpp | test fake only | scripts/tests/sc4_advice_subclasses_test.cpp, scripts/tests/sc4_advice_test.cpp |
| `IScriptControlHost` | `src/gz/LegacyControl.h` | src/gz/LegacyControl.cpp | implemented | scripts/tests/gz_legacycontrol_test.cpp, src/app/NativeUi.cpp |
| `IScriptResourceHost` | `src/sc4/UIResourceObjects.h` |  | test fake only | scripts/tests/sc4_uiresource_test.cpp |
| `IScriptTerpHost` | `src/gz/ScriptTerp.h` |  | implemented | scripts/tests/gz_scriptterp_test.cpp, scripts/tests/gz_uiscriptservice_test.cpp |
| `IScrubberHost` | `src/sc4/ScrubberEffect.h` | src/sc4/ScrubberEffect.cpp | implemented | scripts/tests/sc4_cityeffects_test.cpp, src/app/NativeEffectComponents.cpp |
| `ISeaportBudgetSimApi` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISeaportCityApi` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISeaportDepartmentBudgetApi` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISeaportHost` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISeaportLineItemApi` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISeaportOccupantManagerApi` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISeaportPropOccupantApi` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISeaportTerrainApi` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISeaportTrafficApi` | `src/sc4/SeaportDeveloper.h` | src/sc4/SeaportDeveloper.cpp | implemented | scripts/tests/sc4_seaportdeveloper_test.cpp, src/app/NativePorts.cpp |
| `ISequenceHost` | `src/sc4/AttractorEffect.h` | src/app/NativeEffectComponents.cpp, src/sc4/AttractorEffect.cpp | implemented | scripts/tests/sc4_cityeffects_test.cpp, src/app/NativeSequenceEffects.cpp |
| `ISfxHost` | `src/audio/SFX.h` | src/audio/SFX.cpp | test fake only | scripts/tests/sfx_test.cpp |
| `ISignLabelDemolishHost` | `src/sc4/ViewInputControlSignLabelDemolish.h` | src/sc4/ViewInputControlSignLabelDemolish.cpp | implemented | src/app/NativeSignLabel.cpp |
| `ISignToolHost` | `src/sc4/ViewInputControlSignTool.h` | src/sc4/ViewInputControlSignTool.cpp | implemented | scripts/tests/sc4_viewinputcontrol_signtool_test.cpp, src/app/NativeSignLabel.cpp |
| `ISignpostHost` | `src/sc4/SignpostOccupant.h` | src/sc4/SignpostOccupant.cpp | implemented | scripts/tests/sc4_signpostoccupant_test.cpp, src/app/NativeSignLabel.cpp |
| `ISimFireCrewHost` | `src/sc4/SimFireCrew.h` | src/sc4/SimFireCrew.cpp | implemented | src/app/NativeFireCrew.cpp |
| `ISimGLDrawServices` | `src/gz/DrawContextRenderToSimGL.h` | src/app/NativeGraphics.cpp, src/gz/DrawContextRenderToSimGL.cpp | implemented | scripts/tests/render_uipath_test.mm, src/app/NativeGraphics.h |
| `ISimulatorHost` | `src/sc4/Simulator.h` | src/sc4/Simulator.cpp | implemented | scripts/tests/sc4_simulator_test.cpp, src/app/NativeSimulatorHost.cpp |
| `ISnapshotHost` | `src/sc4/WinSnapshot.h` | src/sc4/WinSnapshot.cpp | implemented | scripts/tests/sc4_winsnapshot_test.cpp, src/app/NativeSnapshot.cpp |
| `ISoundBankHost` | `src/sc4/AudioAEMSSoundBank.h` | src/sc4/AudioAEMSSoundBank.cpp | implemented | scripts/tests/sc4_audioaemseventsystem_test.cpp, scripts/tests/sc4_audioaemssoundbank_test.cpp |
| `ISplashScreenHost` | `src/sc4/WinSplashScreen.h` | src/sc4/WinSplashScreen.cpp | implemented | scripts/tests/sc4_winsplashscreen_test.cpp, src/app/game_main.mm |
| `ISpriteInstanceHost` | `src/s3d/ModelInstanceAnimatedSprite.h` | src/s3d/ModelInstanceAnimatedSprite.cpp | implemented | scripts/tests/s3d_modelinstanceanimatedsprite_test.cpp, scripts/tests/s3d_resspriteanim_test.cpp |
| `ISubBudgetWindowHost` | `src/sc4/SubBudgetWindow.h` | src/app/NativeQuery.cpp, src/sc4/SubBudgetWindow.cpp | implemented | scripts/tests/sc4_subbudgetwindow_test.cpp, src/app/NativeBudgetUi.cpp |
| `ISurfaceWaterHost` | `src/sc4/SurfaceWater.h` | src/gz/GZCOM.h, src/gz/gzcom_creators/cSTESurfaceWater.cpp | implemented | scripts/tests/sc4_surfacewater_test.cpp, src/app/NativeTerrainHost.cpp |
| `ITerrainBrush` | `src/sc4/EffectComponents.h` | src/sc4/EffectComponents.cpp | implemented | scripts/tests/sc4_effectcomponents_test.cpp, src/app/NativeEffectComponents.cpp |
| `ITerrainChangedHost` | `src/sc4/BuildingDevelopmentSimulator.h` | src/sc4/BuildingDevelopmentSimulator.cpp | implemented | scripts/tests/sc4_buildingdevelopment_test.cpp, src/app/NativeTerrainChangedLots.cpp |
| `ITerrainChangedLot` | `src/sc4/BuildingDevelopmentSimulator.h` | src/sc4/BuildingDevelopmentSimulator.cpp | implemented | scripts/tests/sc4_buildingdevelopment_test.cpp, src/app/NativeTerrainChangedLots.cpp |
| `ITerrainDrawContext` | `src/sc4/cSTETerrainView3D.h` | src/sc4/OverlayManager.h, src/sc4/cSTETerrainView3D.cpp, src/sc4/cSTETerrainView3DDraw.cpp | implemented | scripts/terrain_shot.mm, scripts/tests/sc4_terrainview3d_test.cpp |
| `ITerrainEditor` | `src/sc4/View3DWin.h` | src/app/NativeTerrainTool.h, src/app/NativeView3DWinHost.cpp, src/sc4/View3DWin.cpp | implemented | scripts/tests/sc4_view3dwin_test.cpp, src/app/NativeTerrainTool.cpp |
| `ITerrainEffect` | `src/sc4/cSTETerrain.h` | src/app/NativeEffects.h, src/app/NativeTerrainHost.cpp, src/sc4/cSTETerrain.cpp | implemented | scripts/tests/sc4_steterrain_test.cpp, src/app/NativeEffects.cpp |
| `ITerrainHeightImage` | `src/sc4/cSTETerrain.h` | src/app/NativeTerrainHost.cpp, src/sc4/cSTETerrain.cpp | test fake only | scripts/tests/sc4_steterrain_test.cpp |
| `ITerrainHost` | `src/sc4/cSTETerrain.h` | src/sc4/cSTETerrain.cpp | implemented | scripts/tests/sc4_steterrain_test.cpp, src/app/NativeTerrainHost.cpp |
| `ITerrainIniFile` | `src/sc4/cSTETerrain.h` | src/sc4/cSTETerrain.cpp | implemented | scripts/tests/sc4_steterrain_test.cpp, src/app/NativeTerrainHost.cpp |
| `ITerrainMapSource` | `src/sc4/EffectMaps.h` | src/sc4/EffectMaps.cpp | test fake only | scripts/tests/sc4_effectmaps_test.cpp |
| `ITerrainScrubberHost` | `src/sc4/ViewInputControlTerrainScrubber.h` | src/sc4/ViewInputControlTerrainScrubber.cpp | implemented | src/app/NativeTerrainTool.cpp |
| `ITerrainSelection` | `src/sc4/cSTETerrainView3D.h` |  | NOT IMPLEMENTED |  |
| `ITerrainVertBufExtn` | `src/sc4/cSTETerrainView3D.h` | src/app/NativeDecalOverlay.cpp, src/app/NativeShadowOverlay.cpp, src/app/hosts/StubOverlay.cpp ... | NOT IMPLEMENTED |  |
| `ITerrainView` | `src/sc4/cSTETerrain.h` | src/app/NativeTerrainHost.cpp, src/gz/SimHostRegistry.h, src/sc4/ServiceLocator.cpp ... | implemented | scripts/tests/sc4_steterrain_test.cpp, src/app/NativeTerrainView.cpp |
| `ITerrainViewHost` | `src/sc4/cSTETerrainView3D.h` | src/gz/SimHostRegistry.h, src/sc4/cSTETerrainView3D.cpp | implemented | scripts/terrain_shot.mm, scripts/tests/sc4_terrainview3d_test.cpp |
| `ITerrainViewSource` | `src/sc4/ServiceLocator.h` | src/sc4/ServiceLocator.cpp | test fake only | scripts/tests/sc4_servicelocator_test.cpp |
| `ITerrainWaveSystem` | `src/sc4/cSTETerrain.h` | src/sc4/cSTETerrain.cpp | test fake only | scripts/tests/sc4_steterrain_test.cpp |
| `ITextDecalHost` | `src/sc4/TextDecalOccupant.h` | src/sc4/TextDecalOccupant.cpp | implemented | scripts/tests/sc4_textdecaloccupant_test.cpp, src/app/NativeSignLabel.cpp |
| `ITextureManagerHost` | `src/render/gd/SGLDX7Driver.h` | src/render/gd/SGLDX7Driver.cpp | test fake only | scripts/tests/render_sgldx7driver_test.cpp |
| `ITextureStageHost` | `src/render/TextureStageData.h` | src/render/TextureStageData.cpp, src/render/gd/SGLDX7Driver.h | implemented | scripts/tests/render_texturestage_oracle_driver.cpp, src/render/gd/GLGDriver.cpp |
| `ITipOfTheDayHost` | `src/sc4/TipOfTheDayWinProc.h` | src/sc4/TipOfTheDayWinProc.cpp | implemented | scripts/tests/sc4_tipoftheday_test.cpp, src/app/NativeTips.cpp |
| `IToxicHost` | `src/sc4/ToxicOccupant.h` | src/sc4/ToxicOccupant.cpp | implemented | scripts/tests/sc4_disaster_test.cpp, scripts/tests/sc4_miscoccupants_test.cpp |
| `ITractBuildingDevelopmentApi` | `src/sc4/TractDeveloper.h` | src/app/hosts/StubTractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractCityApi` | `src/sc4/TractDeveloper.h` | src/app/hosts/StubTractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractDemandApi` | `src/sc4/TractDeveloper.h` | src/sc4/TractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractDeveloperHost` | `src/sc4/TractDeveloper.h` | src/app/NativeGrowthDevelopers.h, src/sc4/TractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractDeveloperService` | `src/sc4/TractDeveloper.h` | src/app/hosts/StubTractDeveloper.cpp, src/sc4/GrowthDeveloper.h | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractLotDeveloperApi` | `src/sc4/TractDeveloper.h` | src/app/hosts/StubTractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractLotManagerApi` | `src/sc4/TractDeveloper.h` | src/app/hosts/StubTractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractNetworkCellApi` | `src/sc4/TractDeveloper.h` | src/sc4/TractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractNetworkManagerApi` | `src/sc4/TractDeveloper.h` | src/app/hosts/StubTractDeveloper.cpp, src/sc4/TractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITractZoneManagerApi` | `src/sc4/TractDeveloper.h` | src/app/hosts/StubTractDeveloper.cpp | implemented | scripts/tests/sc4_tractdeveloper_test.cpp, src/app/NativeTractDevelopment.cpp |
| `ITrafficAutomataManagerHost` | `src/sc4/TrafficAutomataManager.h` | src/app/NativeAutomataManagerHost.cpp, src/sc4/PedestrianManager.cpp, src/sc4/TrafficAutomataManager.cpp | implemented | scripts/tests/sc4_trafficautomatamanager_test.cpp, src/sc4/PedestrianManager.h |
| `ITrafficAutomatonHost` | `src/sc4/TrafficAutomaton.h` | src/app/NativeSimHosts.cpp, src/app/NativeTrafficAutomata.cpp, src/sc4/EmergencyVehicle.cpp ... | implemented | scripts/tests/sc4_trafficautomaton_test.cpp, scripts/tests/sc4_vehicle_test.cpp |
| `ITrafficEffectHost` | `src/sc4/TrafficEffectManager.h` |  | implemented | scripts/tests/sc4_trafficautomaton_test.cpp, scripts/tests/sc4_trafficeffectmanager_test.cpp |
| `ITrafficHost` | `src/sc4/TrafficSimulator.h` | src/gz/SimHostRegistry.h, src/sc4/NeighborsSimulator.h, src/sc4/TrafficSimulator.cpp | implemented | scripts/tests/sc4_trafficsimulator_test.cpp, src/app/NativeTraffic.cpp |
| `ITrafficNetworkManagerApi` | `src/sc4/TrafficAutomataManager.h` | src/app/NativeAutomataManagerHost.cpp, src/app/NativeCrime.cpp, src/sc4/TrafficAutomataManager.cpp | implemented | scripts/tests/sc4_trafficautomatamanager_test.cpp, src/app/NativeTrafficAutomata.cpp |
| `ITrafficNetworkMapHost` | `src/sc4/TrafficNetworkMap.h` | src/sc4/TrafficNetworkMap.cpp | implemented | scripts/machine_oracle/vehicle_oracle.cpp, scripts/tests/sc4_trafficautomaton_test.cpp |
| `ITrafficSimulatorApi` | `src/sc4/TrafficAutomataManager.h` | src/app/NativeAutomataManagerHost.cpp, src/sc4/TrafficAutomataManager.cpp | implemented | scripts/tests/sc4_trafficautomatamanager_test.cpp, src/app/NativeTrafficAutomata.cpp |
| `ITreeViewServices` | `src/gz/WinTreeView.h` |  | implemented | scripts/tests/gz_wintreeview_test.cpp, src/gz/WinTreeView.cpp |
| `ITrendBarHost` | `src/sc4/WinTrendBar.h` | src/sc4/WinTrendBar.cpp | implemented | scripts/tests/sc4_wintrendbar_test.cpp, src/app/NativeCityPanel.cpp |
| `ITutorialSystemHost` | `src/sc4/TutorialSystem.h` | src/sc4/TutorialSystem.cpp | implemented | scripts/tests/sc4_tutorialsystem_test.cpp, src/app/NativeTutorial.cpp |
| `ITutorialTaskHost` | `src/sc4/TutorialTask.h` | src/sc4/TutorialTask.cpp | implemented | scripts/tests/sc4_tutorialtask_test.cpp, src/app/NativeTutorial.cpp |
| `ITutorialUIHost` | `src/sc4/TutorialUIWinProc.h` | src/sc4/TutorialUIWinProc.cpp | implemented | src/app/NativeTutorial.cpp |
| `IUndergroundCustomViewHost` | `src/sc4/UndergroundCustomView.h` | src/sc4/UndergroundCustomView.cpp | implemented | scripts/tests/sc4_undergroundview_test.cpp, src/app/NativeUnderground.cpp |
| `IUndergroundNetworkToolHost` | `src/sc4/UndergroundNetworkTool.h` | src/sc4/PipeTool.cpp, src/sc4/UndergroundNetworkTool.cpp | implemented | scripts/tests/sc4_undergroundnetworktool_test.cpp, src/app/NativeUnderground.cpp |
| `IUndergroundOccupantHost` | `src/sc4/UndergroundOccupant.h` | src/sc4/UndergroundOccupant.cpp | implemented | scripts/tests/sc4_undergroundoccupant_test.cpp, scripts/tests/sc4_undergroundview_test.cpp |
| `IVehicleHost` | `src/sc4/Vehicle.h` | src/app/NativeSimHosts.cpp, src/sc4/EmergencyVehicle.cpp, src/sc4/EmergencyVehicle.h ... | implemented | scripts/tests/sc4_vehicle_test.cpp, src/app/NativeTrafficAutomata.cpp |
| `IVehicleManagerHost` | `src/sc4/VehicleManager.h` | src/sc4/VehicleManager.cpp | implemented | src/app/NativeAutomataManagerHost.cpp |
| `IVertexBufferHost` | `src/render/gd/PrimitiveManager.h` | src/render/gd/PrimitiveManager.cpp | test fake only | scripts/tests/render_primitivemanager_test.cpp |
| `IVertexHtConstraintHost` | `src/sc4/VertexHtConstraintSatisfier.h` |  | implemented | scripts/tests/sc4_vertexhtconstraintsatisfier_test.cpp, src/app/hosts/StubVertexHtConstraint.cpp |
| `IView3DWinHost` | `src/sc4/View3DWin.h` | src/app/NativeDemolishTool.h, src/sc4/View3DWin.cpp | implemented | scripts/tests/sc4_view3dwin_test.cpp, src/app/NativeView3DWinHost.cpp |
| `IViewInputHost` | `src/sc4/ViewInputControl.h` | src/sc4/ViewInputControl.cpp, src/sc4/ViewInputControlQuery.cpp, src/sc4/ViewInputControlQuery.h | implemented | scripts/tests/sc4_obliteratecity_test.cpp, scripts/tests/sc4_view3dwin_test.cpp |
| `IViewUtilitiesHost` | `src/sc4/ViewUtilities.h` |  | implemented | scripts/tests/sc4_viewutilities_test.cpp, src/sc4/Render3D.h |
| `IVocabularyServices` | `src/gz/LegacyControl.h` | src/gz/LegacyControl.cpp | implemented | scripts/tests/gz_legacycontrol_test.cpp, src/app/NativeUi.cpp |
| `IVolcanoHost` | `src/sc4/DisasterInstanceVolcano.h` | src/sc4/DisasterInstanceVolcano.cpp | implemented | scripts/tests/sc4_disasterinstancevolcano_test.cpp, src/app/NativeDisasters.cpp |
| `IWatercraftHost` | `src/sc4/Watercraft.h` | src/sc4/Watercraft.cpp | test fake only | scripts/tests/sc4_watercraft_test.cpp |
| `IWatercraftManagerHost` | `src/sc4/WatercraftManager.h` |  | implemented | scripts/tests/sc4_watercraftmanager_test.cpp, src/tools/watercraft_neutral.cpp |
| `IWealthHost` | `src/sc4/WealthRequester.h` | src/app/hosts/StubPropDeveloper.cpp, src/gz/SimHostRegistry.h, src/sc4/PropDeveloper.h ... | implemented | scripts/tests/sc4_propdeveloper_test.cpp, scripts/tests/sc4_smallsim_test.cpp |
| `IWeatherHost` | `src/sc4/WeatherSimulator.h` | src/sc4/WeatherSimulator.cpp, src/tools/city_probe_creators.cpp, src/tools/city_probe_hosts.h | implemented | scripts/tests/sc4_weathersimulator_test.cpp, src/app/NativeWeather.cpp |
| `IWinAdviceListHost` | `src/sc4/WinAdviceList.h` | src/sc4/WinAdviceList.cpp | implemented | scripts/tests/sc4_winadvicelist_test.cpp, src/app/NativeWinHtml.cpp |
| `IWinAnimationRecorderHost` | `src/sc4/WinAnimationRecorder.h` | src/sc4/WinAnimationRecorder.cpp | implemented | scripts/tests/sc4_winanimationrecorder_test.cpp, src/app/hosts/StubWinAnimationRecorder.cpp |
| `IWinBMPViewHost` | `src/sc4/WinBMPView.h` |  | implemented | scripts/tests/sc4_winbmpview_test.cpp, src/app/NativeSnapshot.cpp |
| `IWinButtonHost` | `src/gz/WinBtn.h` | src/gz/LegacyControl.cpp, src/gz/WinBtn.cpp, src/gz/WinCombo.cpp ... | implemented | scripts/tests/gz_winbtn_test.cpp, scripts/tests/gz_wincombo_test.cpp |
| `IWinCalloutBoxServices` | `src/sc4/WinCalloutBox.h` | src/sc4/WinCalloutBox.cpp | implemented | scripts/tests/sc4_wincalloutbox_test.cpp, src/app/NativeAppServices.cpp |
| `IWinCompatHost` | `src/platform/WinCompat.h` | src/platform/WinCompat.cpp | test fake only | scripts/tests/rz_netsocket_test.cpp |
| `IWinFoldersHost` | `src/gz/WinFolders.h` |  | implemented | scripts/tests/gz_winfolders_test.cpp, src/gz/WinFolders.cpp |
| `IWinGenHost` | `src/gz/WinGen.h` | src/gz/WinGen.cpp | NOT IMPLEMENTED |  |
| `IWinGridHost` | `src/gz/WinGrid.h` | src/app/NativeUi.cpp, src/gz/WinGrid.cpp | test fake only | scripts/tests/gz_wingrid_test.cpp |
| `IWinHtmlHost` | `src/gz/WinHTML.h` |  | implemented | scripts/tests/gz_winhtml_test.cpp, src/app/NativeWinHtml.cpp |
| `IWinLotConfigurationEditorHost` | `src/sc4/WinLotConfigurationEditor.h` | src/sc4/WinLotConfigurationEditor.cpp | implemented | scripts/tests/sc4_winlotconfigurationeditor_test.cpp, src/app/hosts/StubWinLotConfigurationEditor.cpp |
| `IWinMapViewHost` | `src/sc4/WinMapView.h` |  | implemented | scripts/tests/sc4_winmapview_test.cpp, src/app/NativeWinMapView.cpp |
| `IWinMgrBaseHost` | `src/gz/WinMgrBase.h` | src/gz/WinMgrBase.cpp | implemented | scripts/tests/gz_winmgrbase_test.cpp, src/app/NativeWinMgr.h |
| `IWinRCIHost` | `src/sc4/WinRCI.h` | src/sc4/WinRCI.cpp | implemented | scripts/tests/sc4_winrci_test.cpp, src/app/NativeCityPanel.cpp |
| `IWinSC4AppHost` | `src/sc4/WinSC4App.h` | src/sc4/WinSC4App.cpp | implemented | scripts/tests/sc4_winsc4app_test.cpp, src/app/NativeWinSC4App.h |
| `IWinTertiaryMenuFrameHost` | `src/sc4/WinTertiaryMenuFrame.h` | src/sc4/WinTertiaryMenuFrame.cpp | implemented | scripts/tests/sc4_wintertiarymenuframe_test.cpp, src/app/NativeCatalogUi.cpp |
| `IWinTextEditHost` | `src/gz/WinTextEdit.h` | src/gz/WinTextEdit.cpp | implemented | scripts/tests/gz_wincombo_test.cpp, scripts/tests/gz_wintextedit_test.cpp |
| `IWinTextTickerHost` | `src/gz/WinTextTicker.h` | src/gz/WinTextTicker.cpp | test fake only | scripts/tests/gz_winttextticker_test.cpp |
| `IZoneDeveloperBuildingDevelopmentApi` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperCityApi` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperDemandApi` | `src/sc4/ZoneDeveloper.h` |  | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperGridApi` | `src/sc4/ZoneDeveloper.h` | src/sc4/ZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperGrowthApi` | `src/sc4/ZoneDeveloper.h` | src/sc4/ZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperHost` | `src/sc4/ZoneDeveloper.h` | src/app/NativeZoneDeveloperHost.h, src/sc4/ZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperLotDeveloperApi` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp, src/sc4/ZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperNetworkApi` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp, src/sc4/ZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperOccupantManagerApi` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperSafeListApi` | `src/sc4/ZoneDeveloper.h` | src/app/NativeZoneDeveloperHost.cpp, src/app/hosts/StubZoneDeveloper.cpp, src/sc4/ZoneDeveloper.cpp | test fake only | scripts/tests/sc4_zonedeveloper_test.cpp |
| `IZoneDeveloperService` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp, src/sc4/TractDeveloper.h | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperTerrainApi` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperTerrainViewApi` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneDeveloperZoneManagerApi` | `src/sc4/ZoneDeveloper.h` | src/app/hosts/StubZoneDeveloper.cpp | implemented | scripts/tests/sc4_zonedeveloper_test.cpp, src/app/NativeZoneDeveloperHost.cpp |
| `IZoneManagerHost` | `src/sc4/cSC4ZoneManager.h` | src/gz/SimHostRegistry.h, src/sc4/cSC4ZoneManager.cpp | implemented | scripts/tests/sc4_zonemanager_test.cpp, src/tools/city_probe_bindings.cpp |
| `IZonePlacementHost` | `src/sc4/cSC4ZoneManager.h` | src/sc4/cSC4ZoneManager.cpp | implemented | src/app/NativeZoneTool.cpp |
| `IZoneToolHost` | `src/sc4/ViewInputControlZone.h` | src/sc4/ViewInputControlZone.cpp | implemented | src/app/NativeZoneTool.cpp |
<!-- generated:end -->

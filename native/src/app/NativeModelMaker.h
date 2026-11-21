// The city's model pipeline on the native Metal application: the S3D model resource (type 0x5ad0e817) the resource
// manager hands cSC4ModelMaker, the texture bindings its materials point at, and the hosts that connect the recovered
// model maker, placeable objects and model instances to the city and the 3D renderer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gd { class cIGZGDriver; }
class cIGZUnknown;
namespace sc4 { class cSC43DRender; class cSC4City; class cISC4Occupant; class cSC4ModelMaker; }

namespace native {

// Registers the S3D model factory with the resource manager (once) and makes the driver the texture target.
void InstallModelResources(gd::cIGZGDriver* driver);
// The driver texture of an FSH resource {0x7ab50e44, group, instance} (0 when it is missing).
unsigned int FshTextureId(unsigned int group, unsigned int instance);
// Its size in texels (false when it is missing).
bool FshTextureSize(unsigned int group, unsigned int instance, int& width, int& height);
// Every frame of an FSH {0x7ab50e44, group, instance} as a driver texture, in file order (empty when missing).
const std::vector<std::uint32_t>& FshFrameTextureIds(std::uint32_t group, std::uint32_t instance);
// The pixels of an FSH image's first frame, four bytes each (red, green, blue, alpha), top row first.
bool FshPixelsRgba(std::uint32_t group, std::uint32_t instance, std::vector<std::uint8_t>& rgba, int& width, int& height);
// A driver texture from 32-bit BGRA pixels (one level), and its deletion; 0 without a driver.
unsigned int CreateBgraTexture(int width, int height, const void* pixels);
void DeleteDriverTexture(unsigned int id);
// "zooms Z models N: [group/instance tex instance:WxH ...] ..." of an occupant's placeable models (debug probes).
std::string DescribeOccupantModels(sc4::cISC4Occupant* occupant);
// Debug: building models in the renderer made for another zoom, and whether their owners still hold them (scenario `audit`).
void AuditRendererModels();
// The city view's model maker (cSC4View3DWin +0x190; null without a city view).
sc4::cSC4ModelMaker* CityModelMaker();
// The model maker's hosts and factories, and the renderer its models go into: before the view makes the model maker
// (DoPreCityInit).
void InstallModelMakerHosts(sc4::cSC43DRender& render);
// cSC4View3DWin::DoPreCityInit's model maker: made (AddRef'd), then initialized; its Shutdown when the view shuts down.
cIGZUnknown* CreateModelMaker();
void InitModelMaker(cIGZUnknown* modelMaker);
void ShutdownModelMaker(cIGZUnknown* modelMaker);
// The loaded city's shadow and decal overlays (the view's DoPostCityInit, before the model maker's: its shadows go there).
void AttachModelMakerOverlays(sc4::cSC43DRender& render, sc4::cSC4City& city);
// "city view: model maker up: ..." (after the model maker's DoPostCityInit).
void ReportModelMaker();
// The overlays go with the city's renderer.
void DetachModelMaker();
// The place-lot tool's preview building: an occupant outside the occupant manager whose models the model maker makes (show) or
// drops (hide) as for a created + shown / hidden + destroyed occupant. False without a model maker.
bool ShowPreviewOccupant(sc4::cISC4Occupant* occupant, bool show);

void NativeTerrainHidingReport();   // kit command `terrainflags`

}  // namespace native

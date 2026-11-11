// The city's model pipeline on the native Metal application: the S3D model resource (type 0x5ad0e817) the resource
// manager hands cSC4ModelMaker, the texture bindings its materials point at, and the hosts that connect the recovered
// model maker, placeable objects and model instances to the city and the 3D renderer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gd { class cIGZGDriver; }
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
// A driver texture from 32-bit BGRA pixels (one level), and its deletion; 0 without a driver.
unsigned int CreateBgraTexture(int width, int height, const void* pixels);
void DeleteDriverTexture(unsigned int id);
// "zooms Z models N: [group/instance tex instance:WxH ...] ..." of an occupant's placeable models (debug probes).
std::string DescribeOccupantModels(sc4::cISC4Occupant* occupant);
// Debug: building models in the renderer made for another zoom, and whether their owners still hold them (scenario `audit`).
void AuditRendererModels();
// The city view's model maker (null without a city view).
sc4::cSC4ModelMaker* CityModelMaker();
// Builds and initialises the model maker for the loaded city; its models go into `render`.
bool AttachModelMaker(sc4::cSC43DRender& render, sc4::cSC4City& city);
// Shuts the model maker of the city being left down (before its renderer goes): the original keeps one model maker for the
// application, the port makes one per city, and a stale one stays subscribed to the view messages with the old city's
// occupants (their models point into the old renderer).
void DetachModelMaker();
// The place-lot tool's preview building: an occupant outside the occupant manager whose models the model maker makes (show) or
// drops (hide) as for a created + shown / hidden + destroyed occupant. False without a model maker.
bool ShowPreviewOccupant(sc4::cISC4Occupant* occupant, bool show);

}  // namespace native

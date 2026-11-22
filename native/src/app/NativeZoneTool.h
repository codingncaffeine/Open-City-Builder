// The zoning tool on the native application: the services of the recovered cSC4ViewInputControlZone and of
// cSC4ZoneManager::PlaceZone (funds, lots, occupants, terrain, the drag highlight) over the loaded city.
#pragma once
#include <cstdint>

class cISC4City;
class cIGZMessageTarget2;
namespace sc4 { class cSC4View3DWin; class cSC43DRender; class cSC4ViewInputControl; }

namespace native {

// Installs the hosts for the loaded city and puts the zone overlay on the renderer.
void InstallZoneTool(sc4::cSC4View3DWin& view, cISC4City* city, sc4::cSC43DRender& render);
// A zoning tool for zone type `type` (0 = de-zone, 1..9 R/C/I, 14 = landfill), one reference.
sc4::cSC4ViewInputControl* CreateZoneTool(int type);
// The animation timer the tool registers with: call once a frame with the elapsed milliseconds.
void PumpToolTicks(int elapsedMs);
void StartToolTick(cIGZMessageTarget2* target, std::uint32_t interval);
void StopToolTick(cIGZMessageTarget2* target);
// Kit verb `zonerect TYPE X0 Z0 X1 Z1`: a zoning tool of that type places the inclusive cell rectangle as its mouse-up does
// (cSC4ViewInputControlZone::PlaceZone on the selected area; no input events, no sound).
void NativeZoneRect(int type, int x0, int z0, int x1, int z1);

}  // namespace native

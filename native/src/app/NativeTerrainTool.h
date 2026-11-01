// The god-mode terrain tools on the native application: the terrain brush tool and the view's terrain editor.
#pragma once
#include <cstdint>

namespace sc4 { class ITerrainEditor; class cSC4ViewInputControl; }

namespace native {

void InstallTerrainTools();                       // the brush tool's host
sc4::ITerrainEditor* CityTerrainEditor();         // null until the city has a terrain
// A terrain brush tool for brush `brushType` (free = no cost, the god-mode default brushes), one reference.
sc4::cSC4ViewInputControl* CreateTerrainScrubberTool(std::uint32_t brushType, bool freeBrush);

}  // namespace native

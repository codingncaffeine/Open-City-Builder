// The terrain view's overlay managers for effect decals (the place-lot footprint, direction arrow and coverage rings, the
// terrain brushes' rings, craters and the rest): the original's cSTETerrainView3D owns four cSTEOverlayManagers that
// SL::TerrainView() vslot 0x48 hands out by index (0 dynamic land, 1 dynamic water, 2 land, 3 water). The recovered
// cSC4DecalEffect adds its decal to one of them; a view object after the models draws every enabled decal overlay as
// terrain-conforming triangles whose texture coordinates are the overlay's texture matrix applied to the world position,
// with the render state of its draw mode (DrawDecals 0x10072aa56).
#pragma once

#include <cstdint>

namespace sc4 { class cSC43DRender; class cSC4City; class cSTETerrain; class cSTEOverlayManager; class IOverlayTexture; }

namespace native {

// Creates the four overlay managers for this city and puts their drawing in the render; installs the decal effects' host.
void AttachDecalOverlays(sc4::cSC43DRender& render, sc4::cSC4City& city, sc4::cSTETerrain& terrain);
// Drops them with the city view (the decal effects were stopped by the effects manager's shutdown).
void DetachDecalOverlays();
sc4::cSTEOverlayManager* DecalOverlayManager(int index);
// The place-lot tool's footprint texture (GetBuildingPreviewModel, i386 0x1003bc9ac): size x size texels, clear, a white
// rectangle inset by the two margins and its inside 2 texels further in at alpha 0xaa. One reference for the caller.
sc4::IOverlayTexture* CreateFootprintTexture(int size, int marginX, int marginZ);

}  // namespace native

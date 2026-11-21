// The underground network hosts on the city (NativeUnderground.cpp).
#pragma once

namespace native {

// The underground tool's and the underground occupant's hosts (replace the neutral load-time hosts).
void InstallUndergroundHosts();
// Kit command `pipecells X Z`: the pipes (count, pipe edges, network flags) and other network pieces on the 3x3 cells around.
void NativeUndergroundCellsReport(int x, int z);
// Kit command `subways [N]`: the city's subway pieces (cSC4SubwayOccupant) and subway connections (cSC4SubwayConnectionOccupant):
// the counts, then the first N of each (cell, piece, rotation, edges, flags, model) and each connection's cell and joined pieces.
void NativeSubwayReport(int limit);

}  // namespace native

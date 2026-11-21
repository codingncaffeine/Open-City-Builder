// The underground network hosts on the city (NativeUnderground.cpp).
#pragma once
#include <cstdint>

namespace native {

// The underground tool's and the underground occupant's hosts (replace the neutral load-time hosts).
void InstallUndergroundHosts();
// Kit command `pipecells X Z`: the pipes (count, pipe edges, network flags) and other network pieces on the 3x3 cells around.
void NativeUndergroundCellsReport(int x, int z);
// Kit command `subways [N]`: the city's subway pieces (cSC4SubwayOccupant) and subway connections (cSC4SubwayConnectionOccupant):
// the counts, then the first N of each (cell, piece, rotation, edges, flags, model) and each connection's cell and joined pieces;
// with "paths", each listed piece's train paths (builds its path info) and the traffic simulator's subway connections there.
void NativeSubwayReport(int limit, bool paths = false);
// subwayroute X0 Z0 X1 Z1 ALLOWED START: one traffic path search between two cells with the given travel types
// (app/NativeTrafficAutomata.cpp)
void NativeSubwayRouteReport(int x0, int z0, int x1, int z1, std::uint32_t allowed, std::uint32_t start);

}  // namespace native

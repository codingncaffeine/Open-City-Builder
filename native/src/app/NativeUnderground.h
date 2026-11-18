// The underground network hosts on the city (NativeUnderground.cpp).
#pragma once

namespace native {

// The underground tool's and the underground occupant's hosts (replace the neutral load-time hosts).
void InstallUndergroundHosts();
// Kit command `pipecells X Z`: the pipes (count, pipe edges, network flags) and other network pieces on the 3x3 cells around.
void NativeUndergroundCellsReport(int x, int z);

}  // namespace native

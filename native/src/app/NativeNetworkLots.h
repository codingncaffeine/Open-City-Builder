// The network lot manager's host and the network pieces' messages on the native city (NativeNetworkLots.cpp).
#pragma once

namespace native {

void InstallNetworkLotHosts();   // replaces the neutral INetworkLotManagerHost and INetworkOccupantHost
// Kit report: the manager's lot and configuration counts, the props and trees it placed, and the pieces on the
// (2R+1)^2 cells around X,Z (R < 0: the whole city; piece, under-texture, variation, the cell's lot configuration and street prop count).
void NativeNetworkLotReport(int x, int z, int radius);
// Kit check: every piece restyled and compared with the style the original saved; every lot checked for compatibility.
void NativeNetworkLotVerify();
void NativeNetworkLotPothole(int x, int z, bool damaged);   // kit: the pothole message on a cell

}  // namespace native

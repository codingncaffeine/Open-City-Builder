// The network drawing tools (roads, rail, pipes, power lines, ...) on the native application: the hosts of the recovered
// cSC4ViewInputControlNetworkTool, cSC4NetworkToolUI and cSC4NetworkTool over the loaded city.
#pragma once
#include <cstdint>

class cISC4City;
namespace sc4 { class cSC4View3DWin; class cSC4ViewInputControl; }

namespace native {

// Installs the hosts for the loaded city.
void InstallNetworkTool(sc4::cSC4View3DWin& view, cISC4City* city);
// cSC4View3DWin's catalog branch: new cSC4ViewInputControlNetworkTool(type), one reference.
sc4::cSC4ViewInputControl* CreateNetworkTool(int type);
// cSC4View3DWin's catalog branch for the items 0x6a47xxxx: new cSC4ViewInputControlNetworkIntxTool(item & 0xffff), one reference.
sc4::cSC4ViewInputControl* CreateNetworkIntxTool(std::uint32_t ruleId);
// Kit verbs: `puzzles [ID]` (read-only) lists the intersection rules, `puzzle ID X Z ROT` runs the intersection tool's own
// search, dry run and placement at a cell (no input), `intxpieces` (read-only) lists the city's intersection pieces.
void NativePuzzleReport(std::uint32_t only);
void NativePlacePuzzle(std::uint32_t id, int x, int z, int rotation);
void NativeIntersectionPieceReport();
void NativeNetworkEndReport(int limit);   // kit verb netends
// Kit verb `prebuiltpieces X0 Z0 X1 Z1` (read-only): the pre-built model network pieces of a rectangle, their support models
// and the shadow texture the network manager maps each to at the current zoom.
void NativePreBuiltPieceReport(int x0, int z0, int x1, int z1);
void NativeSupportStructureReport(int x0, int z0, int x1, int z1);   // supports X0 Z0 X1 Z1 (read-only)
// Kit verb `terrainheights X0 Z0 X1 Z1` (read-only): the terrain altitude of every vertex in the inclusive rectangle, one
// line per vertex row.
void NativeTerrainHeightsReport(int x0, int z0, int x1, int z1);

}  // namespace native

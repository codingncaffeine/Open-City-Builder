// cSC4ViewInputControlLevelTerrain (i386 0x002d0854..0x002d0bde): the god-mode "level terrain" tool RunGlobalTerrainTool(7)
// puts up. Dragging from a cell marks the rectangle to the pointer; releasing levels every vertex of the rectangle (one past its
// far edges, clamped to the city) to the altitude of the vertex the drag started at. Layout (i386): +0x28 the terrain, +0x2c its
// view (selection marking), +0x30 dragging, +0x34 / +0x38 the start cell, +0x3c / +0x40 the current cell, +0x44..+0x50 the
// normalized rectangle, +0x54 the cell width; id 0x88cea620, cursor 0x071e5703.
#pragma once

#include <cstdint>

#include "sc4/ViewInputControl.h"

namespace sc4 {

class cSTETerrain;
struct TerrainRect;

constexpr std::uint32_t kLevelTerrainToolId = 0x88cea620;

class ILevelTerrainHost {
public:
    virtual ~ILevelTerrainHost() = default;
    virtual cSTETerrain* Terrain() = 0;                                        // SL::spTerrain
    virtual bool PickTerrain(int x, int y, float out[3]) = 0;                  // the tool's view vslot 0x90(x, y, out, 0)
    // The terrain view's selection (view vslots 0x14 MarkSelected(rect, 2, 1), 0x24 clear, 0x28 show / hide, answering the
    // previous state): drawing only.
    virtual void MarkSelected(const TerrainRect& rect) { (void)rect; }
    virtual void ClearSelection() {}
    virtual bool ShowSelection(bool show) { (void)show; return false; }
};
ILevelTerrainHost* LevelTerrainHost();
void SetLevelTerrainHost(ILevelTerrainHost* host);

class cSC4ViewInputControlLevelTerrain final : public cSC4ViewInputControl {
public:
    cSC4ViewInputControlLevelTerrain();                                   // i386 0x002d0a46
    bool Init() override;                                                 // i386 0x002d08b6
    bool Shutdown() override;                                             // i386 0x002d09da
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override { return !IsKeyToBePassed(key, modifiers); }   // 0x002d09b4
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;    // i386 0x002d0b3e
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;      // i386 0x002d0a84
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;     // i386 0x002d0bde

private:
    void ComputeAndMarkSelectedArea();                                    // i386 0x002d0854
    void LevelTerrain();                                                  // i386 0x002d091c

    cSTETerrain* terrain_ = nullptr;   // +0x28 (counted by the original; the city keeps it)
    bool dragging_ = false;            // +0x30
    bool selectionShown_ = false;      // +0x24 (the selection display's state before the drag)
    int startX_ = 0, startZ_ = 0;      // +0x34 / +0x38
    int endX_ = 0, endZ_ = 0;          // +0x3c / +0x40
    int left_ = 0, top_ = 0, right_ = 0, bottom_ = 0;   // +0x44..+0x50
    float cellWidth_ = 16.0f;          // +0x54
};

}  // namespace sc4

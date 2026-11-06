// cSC4TextDecalOccupant — a prop that paints a line of text onto the terrain (street names, labels): 0x1007114c6..0x100712800
// plus the thunks around it (GZCOM factory 0x100711440, operator new/delete 0x10071147e / 0x100711498 are ported
// elsewhere). Class id 0x8b79c707, interface 0x2b3b7d86, record version 3. It is a cSC4PropOccupant (prop type 0xcb79919b)
// that also listens for messages.
//
// Original layout (the prop sits at +8 of the object; offsets below are original ones): +0xa8 text (cRZString),
// +0xd0 text height (1.0), +0xd4 / +0xd8 decal ids in the two overlay managers (-1 = none), +0xdc dynamic flag,
// +0xe0 decal width (derived), +0xe4/+0xe8 position x/z, +0xec..+0xf4 colour (255,255,255), +0xf8 alpha (1.0),
// +0xfc orientation, +0x100 the rendered text texture, +0x108 / +0x110 overlay managers A / B (A: terrain overlay,
// B: rotated overlay), the prop's bounding box (+0x78..+0x8c) is rewritten from the decal rectangle on every update.
// Overlay manager index: A = dynamic*2, B = dynamic*2+1 (the dynamic variants move/recolour a decal without rebuilding it).
// Occupant flag bit 2 means "shown": it is only kept while the occupant is in the 3D view and the prop developer allows it.
// Quirks kept: Read re-reads the position after the width (same values); records older than 3 carry an unused u32;
// version 1 stored the height as an integer; the turn table is indexed with the unchecked orientation.
// Not ported here: cSC4PropOccupant::DoMessage (0x10015ebb2, owned by the prop class) is reached through DoPropMessage.
#pragma once
#include <cstdint>

#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4TextDecalOccupant = 0x8b79c707;
constexpr std::uint32_t kGZIID_cISC4TextDecalOccupant = 0x2b3b7d86;
constexpr std::uint32_t kPropTypeTextDecal = 0xcb79919b;
constexpr std::uint32_t kMessageTextDecalViewChanged = 0x26d31ec1;      // re-fetch the overlay managers
constexpr std::uint32_t kMessageTextDecalVisibility = 0x06cdb65b;       // data2 -> pointer to the view set
constexpr std::uint32_t kMessageTextDecalRefresh = 0xa6b79602;          // rebuild the decals
constexpr std::uint32_t kTextDecalFlagShown = 2;
constexpr std::uint32_t kGlyphTextureType = 0xcc1fd62c;

// The rendered text (glyph texture): only its size is used. vslot 0x18.
class ITextTexture : public cIGZUnknown {
public:
    virtual void GetSize(std::int32_t* widthHeight) = 0;     // [3]
};

// The set of occupants currently in the 3D view (View3D vslot 0x90 result, vslot 0x18).
class ITextDecalViewSet : public cIGZUnknown {
public:
    virtual bool ContainsOccupant(cISC4Occupant* occupant) = 0;   // [3]
};

// A terrain-view decal overlay manager (TerrainView vslot 0x90 result).
class ITextDecalOverlay : public cIGZUnknown {
public:
    // vslot 0x30 on manager A (width, texture, position) and on manager B (width, turns, texture, position); -1 = failed
    virtual std::int32_t AddDecal(float width, float turns, ITextTexture* texture, const float* positionXZ) = 0;
    virtual void MoveDecal(std::int32_t id, const float* positionXZ) = 0;      // vslot 0x60
    virtual void RemoveDecal(std::int32_t id) = 0;                             // vslot 0x78
    virtual void SetDecalAlpha(std::int32_t id, float alpha) = 0;              // vslot 0x88
    virtual void SetDecalColor(std::int32_t id, const float* rgb) = 0;         // vslot 0x90
};

// Services replacing the SL::sp* globals.
class ITextDecalHost {
public:
    virtual ~ITextDecalHost() = default;
    virtual cIGZMessageServer2* Messages() = 0;                                // spMS2
    virtual cISC4OccupantManager* OccupantManager() = 0;                       // spOccupantManager
    virtual ITextDecalViewSet* AcquireViewSet() = 0;                           // spView3D vslot 0x90; one reference or null
    virtual bool PropDeveloperAllowsDecals() = 0;                              // spPropDeveloper vslot 0x50 (meaning unresolved)
    virtual ITextDecalOverlay* OverlayManager(std::uint32_t index) = 0;        // SL::TerrainView() vslot 0x90; not owned
    virtual bool CreateTextTexture(const cRZString& text, ITextTexture*& out) = 0;   // spGlyphTextureManager vslot 0x28
    virtual float TerrainHeight(float x, float z) = 0;                         // spTerrain vslot 0x60
    virtual int ViewRotation() = 0;                                            // spRenderer vslot 0x50 result +0x18 (0..3)
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0; // spCity vslot 0x2f8 (result unused)
};
ITextDecalHost* TextDecalHost();
void SetTextDecalHost(ITextDecalHost* host);

// cISC4TextDecalOccupant (outer vtable 0x100ae90a0): slots after cIGZUnknown.
class cISC4TextDecalOccupant : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                                   // [3]
    virtual void SetText(cIGZString& text) = 0;                                // [4]
    virtual void GetText(cIGZString& out) = 0;                                 // [5]
    virtual void SetTextHeight(float height) = 0;                              // [6]
    virtual void SetDynamic(bool dynamic) = 0;                                 // [7]
    virtual void SetTextColor(const float* rgb) = 0;                           // [8]
    virtual void SetOrientation(std::int32_t orientation) = 0;                 // [9]
    virtual void SetAlpha(float alpha) = 0;                                    // [10]
};

class cSC4TextDecalOccupant : public cSC4PropOccupant, public cISC4TextDecalOccupant {   // the message target is the prop's
public:
    cSC4TextDecalOccupant();
    ~cSC4TextDecalOccupant() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cSC4PropOccupant::AddRef(); }
    std::uint32_t Release() override { return cSC4PropOccupant::Release(); }

    bool Init() override;
    bool Shutdown() override;
    void SetPosition(const float* xyz) override;
    void SetAllFlags(std::uint32_t flags) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4TextDecalOccupant; }
    bool DoMessage(cIGZMessage2* message) override;

    // cISC4TextDecalOccupant
    cISC4Occupant* AsOccupant() override { return static_cast<cSC4Occupant*>(this); }
    void SetText(cIGZString& text) override;
    void GetText(cIGZString& out) override;
    void SetTextHeight(float height) override;           // only 0 < h < 8 is accepted
    void SetDynamic(bool dynamic) override;
    void SetTextColor(const float* rgb) override;
    void SetOrientation(std::int32_t orientation) override;
    void SetAlpha(float alpha) override;

    void UpdateOverlayManager();
    void UpdateDecal();
    void RemoveDecals();
    void MoveDecal();
    void UpdateTexture();

    // accessors for tests
    float TextHeight() const { return height_; }
    float DecalWidth() const { return width_; }
    float PositionX() const { return pos_[0]; }
    float PositionZ() const { return pos_[1]; }
    const float* Color() const { return color_; }
    float Alpha() const { return alpha_; }
    std::int32_t Orientation() const { return orientation_; }
    bool Dynamic() const { return dynamic_; }
    std::int32_t DecalIdA() const { return decalA_; }
    std::int32_t DecalIdB() const { return decalB_; }
    const cRZString& Text() const { return text_; }
    ITextTexture* Texture() const { return texture_; }

protected:
    // cSC4PropOccupant::DoMessage (0x10015ebb2) is not part of the recovered prop class; override to chain to it.
    virtual bool DoPropMessage(cIGZMessage2*) { return true; }

private:
    cRZString text_;                       // +0xa8
    float height_ = 1.0f;                  // +0xd0
    std::int32_t decalA_ = -1;             // +0xd4
    std::int32_t decalB_ = -1;             // +0xd8
    bool dynamic_ = false;                 // +0xdc
    float width_ = 100.0f;                 // +0xe0
    float pos_[2] = {0.0f, 0.0f};          // +0xe4 / +0xe8
    float color_[3] = {255.0f, 255.0f, 255.0f};   // +0xec
    float alpha_ = 1.0f;                   // +0xf8
    std::int32_t orientation_ = 1;         // +0xfc
    ITextTexture* texture_ = nullptr;      // +0x100
    ITextDecalOverlay* overlayA_ = nullptr;   // +0x108
    ITextDecalOverlay* overlayB_ = nullptr;   // +0x110
};

}  // namespace sc4

// nSC4Networks::cIntRule — one pre-built intersection ("puzzle piece") rule of the intersection rule file
// {0x0a5bcf4b, 0xaa5bcf57, 0x10000000}: the pattern of existing network cells the piece replaces, the cells it newly
// takes, the models it places, and the ring links the intersection tool cycles through. The rules live in
// cSC4NetworkTool::sIntersectionRules (a hash map keyed by rule id), filled by ReloadRules through
// ReadHighwayIntersectionRules.
//
// Recovered from the Mac i386 build with symbols: the cIntRule methods (0x0019778e..0x00198b4a, ctor 0x0067333e, copy
// 0x00671f08), the file readers (ReadHighwayIntersectionRules 0x001520da, ReadIntersectionRule 0x00156306,
// ReadIntersectionEntries 0x00154b0c, ReadIntersectionOrderingEntries 0x0015647a) and the transform helpers of
// SC4NetworkIntersections.cpp (RotateMap 0x00197930, TransposeMap 0x00197d56, RotatePoint* 0x006a4xxx).
//
// i386 layout of the rule (the comments below carry it): +0x00 id, +0x04 pieces, +0x10 / +0x14 step offsets, +0x18..+0x24
// bounds, +0x28 new cells, +0x34 a point list nothing fills, +0x40 / +0x44 / +0x48 replacement, +0x4c occupied cells, +0x58
// tagged cells, +0x64 check types, +0x78 / +0x7c query string ids, +0x80 preview effect, +0x8c handle offset, +0x94..+0xac
// constraint grid, +0xb0..+0xbc tile grid, +0xc0 / +0xc4 auto tile / path base, +0xc8 tile map, +0xdc type mask, +0xe0
// one-way direction, +0xe1 auto place, +0xe4..+0xf0 the rotation ring (previous, next) and the type ring (previous, next).
#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

class cRZIniFile;

namespace sc4 {

class cSC4NetworkTool;

namespace nSC4Networks {

// cIntPiece (16 bytes): one model the rule places, at an offset from the anchor cell's centre.
struct cIntPiece {
    std::uint32_t id = 0;        // +0x0 the model exemplar (group 0xa8434037 when placed)
    std::uint8_t rotation = 0;   // +0x4
    std::uint8_t flip = 0;       // +0x5
    float x = 0.0f;              // +0x8
    float z = 0.0f;              // +0xc
};

// SC4Point<int>: a cell offset from the rule's anchor.
struct cIntPoint {
    int x = 0;
    int z = 0;
};

// cIntCheckCell (12 bytes): a tagged cell of the cell layout.
struct cIntCheckCell {
    int x = 0;
    int z = 0;
    std::uint32_t tag = 0;   // the layout character
};

// The one-way pattern of each OneWayDir (kOneWayDirectionMasks, i386 0x00846e14).
constexpr std::uint8_t kOneWayDirectionMasks[8] = {0x22, 0xaa, 0x88, 0x99, 0x11, 0x55, 0x44, 0x66};

class cIntRule {
public:
    // cIntRule::CheckType (24 bytes): what a tag requires of its cell. At most two network checks; type -1 = unused.
    struct CheckType {
        std::int8_t types[2] = {-1, -1};   // +0x0 / +0x1
        struct Check {
            std::uint32_t edges = 0;       // +0x4 / +0xc
            std::uint32_t mask = 0;        // +0x8 / +0x10
        } checks[2];
        bool multiOk = false;              // +0x14 other network types may share the cell
        bool occupies = true;              // +0x15 the piece takes the cell (cleared by "check" and "optional")
        bool optional = false;             // +0x16 the cell is not tested
    };
    // cIntRule::cTileDef: the piece one occupied cell gets (AutoTileBase + tile, turned by orientation).
    struct cTileDef {
        std::uint32_t tile = 0;            // +0x0 column * 0x100 + row * 0x10
        std::uint8_t orientation = 0;      // +0x4 rotation | 0x80 flipped
        std::uint8_t newCell = 1;          // +0x5 0 when the cell is also a tagged cell
    };
    // cIntRule::eConstraintType: the constraint layout's codes ('-' '|' '/' '\' 'F' 'R' '#' 'X').
    enum eConstraintType : std::uint8_t {
        kConstraintNone = 0, kConstraintHorizontal = 1, kConstraintVertical = 2, kConstraintSlash = 3, kConstraintBackslash = 4,
        kConstraintF = 5, kConstraintR = 6, kConstraintFlat = 7, kConstraintX = 8,
    };

    // The test results of Test.
    enum { kMatch = 0, kNoMatch = 1, kFail = 2 };

    void ParseCellsFromASCIIArt(const std::list<std::string>& lines);         // 0x001980e6
    void ParseConstraintsFromASCIIArt(const std::list<std::string>& lines);   // 0x00197eb6
    void Translate(int dx, int dz);                                          // 0x001977c8
    void Rotate(int quarterTurns);                                           // 0x00197b2a
    void Transpose();                                                        // 0x00198b4a
    bool IsWithinBounds(int x, int z, std::uint32_t width, std::uint32_t height) const;   // 0x0019778e
    int GetTypeAtOrigin() const;                                             // 0x001978c0
    const cTileDef* GetTileInfo(int dx, int dz) const;                       // 0x0066d6d2
    std::uint8_t GetConstraintAtCell(int dx, int dz) const;                  // 0x0067b4b0
    const CheckType* FindCheckType(std::uint32_t tag) const;
    // 0x001986f4: whether the rule fits at (x, z) of the tool's cell infos (kMatch / kNoMatch / kFail).
    int Test(int x, int z, cSC4NetworkTool& tool) const;
    // 0x0019852a: the edges the rule's arms still need become additional draws of the tool.
    bool DoAutoComplete(int x, int z, cSC4NetworkTool& tool) const;

    std::uint32_t id = 0;                                    // +0x00
    std::vector<cIntPiece> pieces;                           // +0x04
    int stepOffsetA = 0;                                     // +0x10
    int stepOffsetB = 0;                                     // +0x14
    int minX = 0, minZ = 0, maxX = 0, maxZ = 0;              // +0x18 .. +0x24 (left unset by the original's ctor)
    std::vector<cIntPoint> newCells;                         // +0x28 '+' cells: taken, holding no network yet
    std::vector<cIntPoint> unknown34;                        // +0x34 copied and kept, never filled
    int replacementX = 0, replacementZ = 0;                  // +0x40 / +0x44 the anchor of an intersection this one replaces
    bool hasReplacement = false;                             // +0x48
    std::vector<cIntPoint> occupiedCells;                    // +0x4c the cells the piece takes
    std::vector<cIntCheckCell> checkCells;                   // +0x58
    std::unordered_map<std::uint8_t, CheckType> checkTypes;  // +0x64
    std::uint32_t convertQueryId = 0;                        // +0x78
    std::uint32_t placeQueryId = 0;                          // +0x7c (left unset by the original's ctor)
    std::string previewEffect;                               // +0x80
    int handleX = 0, handleZ = 0;                            // +0x8c / +0x90 the mouse cell relative to the anchor
    std::vector<std::uint8_t> constraints;                   // +0x94
    int constraintWidth = 0;                                 // +0xa0
    int constraintCenterX = 0, constraintCenterZ = 0;        // +0xa4 / +0xa8
    int cost = 0;                                            // +0xac
    std::vector<std::uint8_t> tiles;                         // +0xb0 one tile code per cell of the bounds
    int tileWidth = 0;                                       // +0xbc
    std::uint32_t autoTileBase = 0;                          // +0xc0
    std::uint32_t autoPathBase = 0;                          // +0xc4
    std::unordered_map<std::uint8_t, cTileDef> tileMap;      // +0xc8
    std::uint32_t typeMask = 0;                              // +0xdc the network type bits the check types name
    std::uint8_t oneWayDir = 0;                              // +0xe0
    bool autoPlace = false;                                  // +0xe1
    std::uint32_t rotationPrev = 0xffffffffu;                // +0xe4
    std::uint32_t rotationNext = 0xffffffffu;                // +0xe8 (Home / End and the tool's auto-fit walk it)
    std::uint32_t typePrev = 0xffffffffu;                    // +0xec (Shift+Tab)
    std::uint32_t typeNext = 0xffffffffu;                    // +0xf0 (Tab)
};

using IntersectionRuleMap = std::unordered_map<std::uint32_t, cIntRule>;

// ReadHighwayIntersectionRules (0x001520da): every "HighwayIntersectionInfo_<id>" section of the ini becomes a rule (keys
// in file order), every "Ordering..." section links the rules into their rotation and type rings. The map is cleared first.
void ReadHighwayIntersectionRules(cRZIniFile& ini, IntersectionRuleMap& rules);

// ReadIntersectionEntries' CheckType value: "<tag> - <Network>:<edges>[,<mask>] [<Network>:<edges>[,<mask>]] [check]
// [multiok] [optional]". False (nothing stored) on a malformed value.
bool ParseCheckType(const std::string& value, std::uint8_t& tag, cIntRule::CheckType& out);

}  // namespace nSC4Networks
}  // namespace sc4

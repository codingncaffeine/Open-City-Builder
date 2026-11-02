// The network tile-rule engine: the rule text reader (cSC4FileTokenizer), one tile of a rule's solution (cSC4TileInfo), one
// neighbour condition (tCompactNeighborInfo), a rule (cSC4TileRule), the neighbourhood a rule is matched against
// (cSC4TileNeighborhood, abstract: the network tool's cSC4NetworkTileNeighborhood reads its cells) and the rule set
// (cSC4TileEvaluator). The network tool asks the evaluator, for a cell and its neighbours' edge descriptors, which tile
// pieces solve it (cSC4NetworkTool::ApplyNetworkRules).
//
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (0x1001aeb20..0x1001b0600). Rule text format (comma separated, ';'
// starts a comment line): a '#' header line (#Transmogrify expands every rule into its rotations and mirror images), then
// per rule an optional "noflip" (mask 0xf: the four rotations) or "notrans" (mask 1: as written), then "1,e0,e1,e2,e3" (the
// cell's own edges), "2,pos,e0,e1,e2,e3" conditions on a neighbour ('?' = any) and "3,pos,id,rotation,flip" solutions.
// An edge value "kd" is minor type k and connection kind d (packed k * 8 | d).
//
// Port note: cSC4FileTokenizer reads its text through a loader the host installs (SetTileRuleResourceLoader) instead of
// SC4GetResourceAsFile + ReadLine; InitFromText feeds it directly.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "sc4/TileNeighborhood.h"

namespace sc4 {

// SC4GetResourceAsFile (0x10003a3d2) for the tokenizer: the resource's bytes, or false.
using TileRuleResourceLoader = std::function<bool(const cGZPersistResourceKey&, std::string&)>;
void SetTileRuleResourceLoader(TileRuleResourceLoader loader);
bool LoadTileRuleResourceText(const cGZPersistResourceKey& key, std::string& out);   // through the installed loader

class cSC4FileTokenizer final : public ITileTokenSource {
public:
    cSC4FileTokenizer() = default;
    ~cSC4FileTokenizer() override = default;                       // @ 0x1001b056e
    bool Init(const cGZPersistResourceKey& key);                    // @ 0x1001b0502
    bool InitFromText(const std::string& text);
    const char* GetToken() override;                                // @ 0x1001aeca2
    void PushBack() { pushedBack_ = true; }                         // +0x69 (cSC4TileRule::Read sets it)

private:
    bool ReadLine(std::string& out);                                // ReadLine(cIGZFile*, cIGZString&) @ 0x1001b05eb
    bool open_ = false;                                             // +0x00
    std::string text_;                                              // the file (+0x08 cIGZFile*)
    std::size_t position_ = 0;
    std::string line_;                                              // +0x18 cRZString
    std::string token_;                                             // +0x48
    bool atEnd_ = false;                                            // +0x68
    bool pushedBack_ = false;                                       // +0x69
};

// One solution tile: the piece id, where it goes (meta position 0 = the cell, 1..8 the ring around it, 9..24 the next ring)
// and its rotation (bits 0..6) with the flip flag (bit 7).
struct cSC4TileInfo {
    std::uint32_t id = 0;              // +0
    std::uint8_t metaPos = 0;          // +4
    std::uint8_t rotation = 0;         // +5

    static void ConvertMetaToRelativePos(std::uint8_t meta, std::int8_t& dx, std::int8_t& dz);   // @ 0x1001aeb20
    static std::uint8_t ConvertRelativePosToMeta(int dx, int dz);  // sMetaForRelativePos (inlined by its callers)
    void GetCell(std::uint32_t x, std::uint32_t z, std::uint32_t& outX, std::uint32_t& outZ) const;   // @ 0x1001aeb4a
    void GetRotationalEquivalent(int quarterTurns, cSC4TileInfo& out) const;   // @ 0x1001aeb82
    bool Read(cSC4FileTokenizer& tokens);                           // @ 0x1001aec36
};
static_assert(sizeof(cSC4TileInfo) == 8, "cSC4TileInfo stride");

// A rotated meta position (0 stays, the rings turn by 2 resp. 4 places per quarter turn).
std::uint8_t RotateMetaPos(std::uint8_t meta, int quarterTurns);

// A condition on one neighbour: (compact ^ value) & mask must be 0.
struct tCompactNeighborInfo {
    std::uint8_t metaPos = 0;          // +0
    std::uint32_t value = 0;           // +4
    std::uint32_t mask = 0;            // +8
    void GetMirrorImageEquivalent(tCompactNeighborInfo& out) const;            // @ 0x1001af2ae
    void GetRotationalEquivalent(int quarterTurns, tCompactNeighborInfo& out) const;   // @ 0x1001af470
};

// The neighbourhood a rule is matched in. GetCompactRepresentation caches each meta position's compact descriptor.
class cSC4TileNeighborhood {
public:
    virtual ~cSC4TileNeighborhood() = default;
    virtual bool IsValidSolutionPos(std::uint8_t meta) = 0;                                          // vslot 0
    virtual void ComputeCompactRepresentation(std::uint8_t meta, cSC4TileNeighborhoodCompact& out) = 0;   // vslot 8
    void GetCompactRepresentation(std::uint8_t meta, cSC4TileNeighborhoodCompact& out);             // @ 0x1001af142
    std::uint32_t center_ = 0;                                       // +0x08 the cell's own compact (the evaluator's key)
    std::vector<tCompactNeighborInfo> cache_;                        // +0x10
};

class cSC4TileRule {
public:
    void GetMirrorImageEquivalent(cSC4TileRule& out) const;                     // @ 0x1001af1d0
    void AddCondition(const tCompactNeighborInfo& condition);                   // @ 0x1001af314
    void AddSolution(const cSC4TileInfo& solution);                             // @ 0x1001af344
    void GetRotationalEquivalent(int quarterTurns, cSC4TileRule& out) const;    // @ 0x1001af374
    bool IsEquivalentByCondition(const cSC4TileRule& other) const;              // @ 0x1001af576
    bool Match(cSC4TileNeighborhood& neighborhood, const std::vector<cSC4TileInfo>*& solutions) const;   // @ 0x1001af600
    bool Read(cSC4FileTokenizer& tokens);                                       // @ 0x1001af69a

    cSC4TileNeighborhoodCompact compact;                // +0x00 the cell's own edges (the rule set's key)
    std::vector<tCompactNeighborInfo> conditions;       // +0x08
    std::vector<cSC4TileInfo> solutions;                // +0x20
    std::uint32_t transforms = 0xff;                    // +0x38 bit i: variant i (i & 3 quarter turns, i & 4 mirrored)
};

struct TileCompactLess {
    bool operator()(const cSC4TileNeighborhoodCompact& a, const cSC4TileNeighborhoodCompact& b) const { return a.Packed() < b.Packed(); }
};
using TileRuleSet = std::multimap<cSC4TileNeighborhoodCompact, cSC4TileRule, TileCompactLess>;

class cSC4TileEvaluator {
public:
    cSC4TileEvaluator() = default;                                              // @ 0x1001af904
    virtual ~cSC4TileEvaluator() = default;                                     // @ 0x1001af932
    bool Lookup(cSC4TileNeighborhood& neighborhood, const std::vector<cSC4TileInfo>*& solutions) const;   // @ 0x1001af97c
    static void TransmogrifyRuleSet(TileRuleSet& out, const TileRuleSet& rules);   // @ 0x1001afa46
    bool AddRulesFromResource(const cGZPersistResourceKey& key, bool unused = false);   // @ 0x1001b015a
    bool AddRulesFromText(const std::string& text);     // AddRulesFromResource over InitFromText (tests)
    const TileRuleSet& Rules() const { return rules_; }

private:
    bool AddRules(cSC4FileTokenizer& tokens);
    TileRuleSet rules_;                                 // +0x08
};

// The RUL rule files (type 0x0a5bcf4b, group 0xaa5bcf57; the k*ResKey statics set by SC4NetworkTool.cpp's initializer
// 0x10019256a): per network type a "simple" and a "complex" set, instances 1..24, plus 0x10000000 intersections, 0x10000001
// multi-type rules and 0x10000002 rule overrides.
constexpr std::uint32_t kNetworkRulesType = 0x0a5bcf4b, kNetworkRulesGroup = 0xaa5bcf57;

// cSC4NetworkTypeInfo +0x28: a network type's tile evaluators, the complex rules first (13 types, 5 = power line has none).
const std::vector<cSC4TileEvaluator*>& NetworkTileEvaluators(std::uint32_t networkType);
// The evaluator half of cSC4NetworkTool::ReloadNetworkTypeInfo (0x100186f9c): every type's record is cleared and gets
// LoadEvaluator(complex key) then LoadEvaluator(simple key) (cSC4NetworkTypeInfo::LoadEvaluator 0x1001c7390 pushes the
// evaluator even when its file did not load). Returns the number of rule files that loaded.
int LoadNetworkTileEvaluators();

}  // namespace sc4

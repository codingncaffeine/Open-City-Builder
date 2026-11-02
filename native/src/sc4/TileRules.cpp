// The network tile-rule engine (see TileRules.h).
#include "sc4/TileRules.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "rz/RZConvert.h"

namespace sc4 {
namespace {

// sRelativeXForMetaPos / sRelativeZForMetaPos (0x100a1a320 / 0x100a1a340): meta position -> offset from the cell.
constexpr std::int8_t kRelativeX[25] = {0, -1, -1, 0, 1, 1, 1, 0, -1, -2, -2, -2, -1, 0, 1, 2, 2, 2, 2, 2, 1, 0, -1, -2, -2};
constexpr std::int8_t kRelativeZ[25] = {0, 0, -1, -1, -1, 0, 1, 1, 1, 0, -1, -2, -2, -2, -2, -2, -1, 0, 1, 2, 2, 2, 2, 2, 1};
// sMetaForRelativePos (0x100a1a360): (dz + 2) * 5 + dx + 2 -> meta position.
constexpr std::uint8_t kMetaForRelative[25] = {11, 12, 13, 14, 15, 10, 2, 3, 4, 16, 9, 1, 0, 5, 17, 24, 8, 7, 6, 18, 23, 22, 21, 20, 19};
// GetRotationalEquivalent's kResultRotation (0x100a1a380): a flipped tile turns the other way.
constexpr std::uint8_t kResultRotation[16] = {0, 3, 2, 1, 1, 0, 3, 2, 2, 1, 0, 3, 3, 2, 1, 0};

TileRuleResourceLoader& Loader() {
    static TileRuleResourceLoader loader;
    return loader;
}

// Byte i of `packed` moves to byte (i + quarterTurns) % 4.
std::uint32_t RotateBytes(std::uint32_t packed, int quarterTurns) {
    std::uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        const unsigned shift = static_cast<unsigned>((quarterTurns + i) % 4) * 8;
        out = (((packed >> (i * 8)) & 0xff) << shift) | (~(0xffu << shift) & out);
    }
    return out;
}

std::uint32_t MirrorCompact(std::uint32_t packed) {
    cSC4TileNeighborhoodCompact mirrored;
    cSC4TileNeighborhoodCompact(packed).GetMirrorImageEquivalent(mirrored);
    return mirrored.Packed();
}

std::uint8_t MirrorMetaPos(std::uint8_t meta) {
    return cSC4TileInfo::ConvertRelativePosToMeta(-kRelativeX[meta], kRelativeZ[meta]);
}

}  // namespace

void SetTileRuleResourceLoader(TileRuleResourceLoader loader) { Loader() = std::move(loader); }
bool LoadTileRuleResourceText(const cGZPersistResourceKey& key, std::string& out) { return Loader() && Loader()(key, out); }

// ---------------------------------------------------------------------------------------------------- cSC4FileTokenizer

bool cSC4FileTokenizer::Init(const cGZPersistResourceKey& key) {   // @ 0x1001b0502
    if (open_) return false;
    std::string text;
    if (!Loader() || !Loader()(key, text)) return false;
    return InitFromText(text);
}

bool cSC4FileTokenizer::InitFromText(const std::string& text) {
    if (open_) return false;
    text_ = text;
    position_ = 0;
    atEnd_ = false;
    pushedBack_ = false;
    open_ = true;
    return true;
}

// One line without its terminator; false at the end of the file.
bool cSC4FileTokenizer::ReadLine(std::string& out) {   // ReadLine(cIGZFile*, cIGZString&) @ 0x1001b05eb
    out.clear();
    if (position_ >= text_.size()) return false;
    std::size_t end = text_.find('\n', position_);
    if (end == std::string::npos) end = text_.size();
    out.assign(text_, position_, end - position_);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    position_ = end + 1;
    return true;
}

// The next comma-separated token. Empty lines and lines starting with ';' are skipped; each line read gets a trailing ','
// so its last token splits off too. A pushed-back token is returned once more.
const char* cSC4FileTokenizer::GetToken() {   // @ 0x1001aeca2
    if (pushedBack_) {
        pushedBack_ = false;
        return token_.c_str();
    }
    if (atEnd_) return nullptr;
    do {
        if (line_.empty()) {
            bool lastLine = false;
            do {
                const bool read = ReadLine(line_);
                atEnd_ = !read;
                if (!read) lastLine = true;
                if (!line_.empty() && line_[0] != ';') {
                    line_.push_back(',');
                    break;
                }
            } while (!lastLine);
            if (atEnd_) break;
        }
        const std::size_t comma = line_.find(',');
        if (comma == std::string::npos) {   // cRZString::SplitToken(",") finds nothing: an empty token
            token_.clear();
        } else {
            token_.assign(line_, 0, comma);
            line_.erase(0, comma + 1);
        }
        if (!token_.empty()) return token_.c_str();
    } while (!atEnd_);
    return nullptr;
}

// ------------------------------------------------------------------------------------------------------- cSC4TileInfo

void cSC4TileInfo::ConvertMetaToRelativePos(std::uint8_t meta, std::int8_t& dx, std::int8_t& dz) {   // @ 0x1001aeb20
    dx = kRelativeX[meta];
    dz = kRelativeZ[meta];
}

std::uint8_t cSC4TileInfo::ConvertRelativePosToMeta(int dx, int dz) { return kMetaForRelative[(dz + 2) * 5 + dx + 2]; }

void cSC4TileInfo::GetCell(std::uint32_t x, std::uint32_t z, std::uint32_t& outX, std::uint32_t& outZ) const {   // @ 0x1001aeb4a
    outX = static_cast<std::uint32_t>(kRelativeX[metaPos]) + x;
    outZ = static_cast<std::uint32_t>(kRelativeZ[metaPos]) + z;
}

std::uint8_t RotateMetaPos(std::uint8_t meta, int quarterTurns) {
    if (meta == 0) return 0;
    if (meta < 9) return static_cast<std::uint8_t>((meta - 1 + quarterTurns * 2) % 8 + 1);
    if (meta < 25) return static_cast<std::uint8_t>((meta - 9 + quarterTurns * 4) % 16 + 9);
    return meta;
}

void cSC4TileInfo::GetRotationalEquivalent(int quarterTurns, cSC4TileInfo& out) const {   // @ 0x1001aeb82
    out.metaPos = RotateMetaPos(metaPos, quarterTurns);
    out.id = id;
    const std::uint8_t turn = rotation & 0x7f;
    const std::uint8_t flip = rotation & 0x80;
    const std::uint8_t turned = flip == 0 ? static_cast<std::uint8_t>((turn + quarterTurns) % 4) : kResultRotation[quarterTurns + turn * 4];
    out.rotation = static_cast<std::uint8_t>(turned | flip);
}

// "pos, id, rotation, flip"
bool cSC4TileInfo::Read(cSC4FileTokenizer& tokens) {   // @ 0x1001aec36
    metaPos = static_cast<std::uint8_t>(std::atoi(tokens.GetToken()));
    id = RZ::ConvertToUint(tokens.GetToken());
    rotation = static_cast<std::uint8_t>(std::atoi(tokens.GetToken()));
    rotation = static_cast<std::uint8_t>(rotation | (std::atoi(tokens.GetToken()) != 0) << 7);
    return true;
}

// -------------------------------------------------------------------------------------------------- tCompactNeighborInfo

void tCompactNeighborInfo::GetMirrorImageEquivalent(tCompactNeighborInfo& out) const {   // @ 0x1001af2ae
    out.metaPos = MirrorMetaPos(metaPos);
    out.value = MirrorCompact(value);
    out.mask = MirrorCompact(mask);
}

void tCompactNeighborInfo::GetRotationalEquivalent(int quarterTurns, tCompactNeighborInfo& out) const {   // @ 0x1001af470
    out.metaPos = RotateMetaPos(metaPos, quarterTurns);
    out.value = RotateBytes(value, quarterTurns);
    out.mask = RotateBytes(mask, quarterTurns);
}

// -------------------------------------------------------------------------------------------------- cSC4TileNeighborhood

void cSC4TileNeighborhood::GetCompactRepresentation(std::uint8_t meta, cSC4TileNeighborhoodCompact& out) {   // @ 0x1001af142
    for (const tCompactNeighborInfo& cached : cache_) {
        if (cached.metaPos == meta) {
            out = cSC4TileNeighborhoodCompact(cached.value);
            return;
        }
    }
    tCompactNeighborInfo entry;
    entry.metaPos = meta;
    cSC4TileNeighborhoodCompact computed;
    ComputeCompactRepresentation(meta, computed);
    entry.value = computed.Packed();
    cache_.push_back(entry);
    out = computed;
}

// ---------------------------------------------------------------------------------------------------------- cSC4TileRule

void cSC4TileRule::GetMirrorImageEquivalent(cSC4TileRule& out) const {   // @ 0x1001af1d0
    compact.GetMirrorImageEquivalent(out.compact);
    for (const tCompactNeighborInfo& condition : conditions) {
        tCompactNeighborInfo mirrored;
        condition.GetMirrorImageEquivalent(mirrored);
        out.AddCondition(mirrored);
    }
    for (const cSC4TileInfo& solution : solutions) {
        cSC4TileInfo mirrored;
        mirrored.id = solution.id;
        mirrored.metaPos = MirrorMetaPos(solution.metaPos);
        mirrored.rotation = static_cast<std::uint8_t>(solution.rotation ^ 0x80);
        out.AddSolution(mirrored);
    }
}

void cSC4TileRule::AddCondition(const tCompactNeighborInfo& condition) { conditions.push_back(condition); }   // @ 0x1001af314
void cSC4TileRule::AddSolution(const cSC4TileInfo& solution) { solutions.push_back(solution); }               // @ 0x1001af344

void cSC4TileRule::GetRotationalEquivalent(int quarterTurns, cSC4TileRule& out) const {   // @ 0x1001af374
    out.compact = cSC4TileNeighborhoodCompact(RotateBytes(compact.Packed(), quarterTurns));
    for (const tCompactNeighborInfo& condition : conditions) {
        tCompactNeighborInfo rotated;
        condition.GetRotationalEquivalent(quarterTurns, rotated);
        out.AddCondition(rotated);
    }
    for (const cSC4TileInfo& solution : solutions) {
        cSC4TileInfo rotated;
        solution.GetRotationalEquivalent(quarterTurns, rotated);
        out.AddSolution(rotated);
    }
}

// Same own edges and the same set of (position, value) conditions; the masks are not compared.
bool cSC4TileRule::IsEquivalentByCondition(const cSC4TileRule& other) const {   // @ 0x1001af576
    if (other.compact.Packed() != compact.Packed()) return false;
    const auto contains = [](const std::vector<tCompactNeighborInfo>& list, const tCompactNeighborInfo& wanted) {
        for (const tCompactNeighborInfo& c : list)
            if (c.metaPos == wanted.metaPos && c.value == wanted.value) return true;
        return false;
    };
    for (const tCompactNeighborInfo& c : conditions)
        if (!contains(other.conditions, c)) return false;
    for (const tCompactNeighborInfo& c : other.conditions)
        if (!contains(conditions, c)) return false;
    return true;
}

bool cSC4TileRule::Match(cSC4TileNeighborhood& neighborhood, const std::vector<cSC4TileInfo>*& out) const {   // @ 0x1001af600
    for (const tCompactNeighborInfo& condition : conditions) {
        cSC4TileNeighborhoodCompact actual;
        neighborhood.GetCompactRepresentation(condition.metaPos, actual);
        if ((condition.mask & (actual.Packed() ^ condition.value)) != 0) return false;
    }
    for (const cSC4TileInfo& solution : solutions)
        if (!neighborhood.IsValidSolutionPos(solution.metaPos)) return false;
    out = &solutions;
    return true;
}

bool cSC4TileRule::Read(cSC4FileTokenizer& tokens) {   // @ 0x1001af69a
    const char* token = tokens.GetToken();
    transforms = 0xff;
    if (!token) return false;
    if (std::strcmp(token, "noflip") == 0 || std::strcmp(token, "notrans") == 0) {
        transforms = std::strcmp(token, "noflip") == 0 ? 0xf : 1;
        token = tokens.GetToken();
        if (!token) return false;
    }
    if (std::atoi(token) != 1) return false;
    compact.Read(tokens);
    for (token = tokens.GetToken(); token; token = tokens.GetToken()) {
        if (*token < 0 || !std::isdigit(static_cast<unsigned char>(*token))) {
            tokens.PushBack();
            return true;
        }
        const int kind = std::atoi(token);
        if (kind == 3) {
            cSC4TileInfo solution;
            solution.Read(tokens);
            solutions.push_back(solution);
        } else if (kind == 2) {
            tCompactNeighborInfo condition;
            condition.metaPos = static_cast<std::uint8_t>(std::atoi(tokens.GetToken()));
            for (unsigned shift = 0; shift < 32; shift += 8) {
                const char* edge = tokens.GetToken();
                if (!edge) return false;
                if (*edge == '?') {
                    condition.mask = ~(0xffu << shift) & condition.mask;
                } else {
                    const std::uint32_t number = static_cast<std::uint32_t>(std::atoi(edge)) & 0xff;
                    const std::uint32_t entry = (((number / 10) * 8) & 0xf8) | (number % 10);
                    condition.value = (entry << shift) | (~(0xffu << shift) & condition.value);
                    condition.mask |= 0xffu << shift;
                }
            }
            conditions.push_back(condition);
        } else {
            tokens.PushBack();
            return true;
        }
    }
    return !solutions.empty();
}

// ----------------------------------------------------------------------------------------------------- cSC4TileEvaluator

bool cSC4TileEvaluator::Lookup(cSC4TileNeighborhood& neighborhood, const std::vector<cSC4TileInfo>*& solutions) const {   // @ 0x1001af97c
    solutions = nullptr;
    const auto range = rules_.equal_range(cSC4TileNeighborhoodCompact(neighborhood.center_));
    for (auto it = range.first; it != range.second; ++it)
        if (it->second.Match(neighborhood, solutions)) return true;
    return false;
}

// Every rule becomes the variants its transform mask allows (bit i: i & 3 quarter turns of the rule, mirrored first when
// i & 4); a variant whose conditions equal an earlier variant's of the same rule is dropped.
void cSC4TileEvaluator::TransmogrifyRuleSet(TileRuleSet& out, const TileRuleSet& rules) {   // @ 0x1001afa46
    for (const auto& entry : rules) {
        const cSC4TileRule& rule = entry.second;
        std::vector<cSC4TileRule> kept;
        TileRuleSet variants;
        for (unsigned i = 0; i < 8; ++i) {
            if (((rule.transforms >> i) & 1) == 0) continue;
            cSC4TileRule source;
            if ((i & 4) == 0) {
                source = rule;
            } else {
                rule.GetMirrorImageEquivalent(source);
            }
            cSC4TileRule variant;
            source.GetRotationalEquivalent(static_cast<int>(i & 3), variant);
            bool duplicate = false;
            for (const cSC4TileRule& earlier : kept)
                if (earlier.IsEquivalentByCondition(variant)) { duplicate = true; break; }
            if (duplicate) continue;
            kept.push_back(variant);
            variants.insert({variant.compact, variant});
        }
        for (const auto& variant : variants) out.insert(variant);
    }
}

bool cSC4TileEvaluator::AddRulesFromResource(const cGZPersistResourceKey& key, bool) {   // @ 0x1001b015a
    cSC4FileTokenizer tokens;
    if (!tokens.Init(key)) return false;
    return AddRules(tokens);
}

bool cSC4TileEvaluator::AddRulesFromText(const std::string& text) {
    cSC4FileTokenizer tokens;
    tokens.InitFromText(text);
    return AddRules(tokens);
}

// The header token: "#Transmogrify" expands the whole set afterwards, any other '#' line does not, anything else fails.
bool cSC4TileEvaluator::AddRules(cSC4FileTokenizer& tokens) {
    const char* header = tokens.GetToken();
    if (!header) return false;
    bool transmogrify;
    if (std::strcmp(header, "#Transmogrify") == 0) {
        transmogrify = true;
    } else if (*header == '#') {
        transmogrify = false;
    } else {
        return false;
    }
    for (;;) {
        cSC4TileRule rule;
        if (!rule.Read(tokens)) break;
        rules_.insert({rule.compact, rule});
    }
    if (transmogrify) {
        TileRuleSet expanded;
        TransmogrifyRuleSet(expanded, rules_);
        rules_.swap(expanded);
    }
    return true;
}

// ------------------------------------------------------------------------------------------- the network types' evaluators

namespace {
constexpr std::uint32_t kNetworkTypeCount = 13;
std::vector<cSC4TileEvaluator*>& EvaluatorsOf(std::uint32_t type) {
    static std::vector<cSC4TileEvaluator*> evaluators[kNetworkTypeCount];
    return evaluators[type];
}
}  // namespace

const std::vector<cSC4TileEvaluator*>& NetworkTileEvaluators(std::uint32_t networkType) {
    static const std::vector<cSC4TileEvaluator*> none;
    return networkType < kNetworkTypeCount ? EvaluatorsOf(networkType) : none;
}

int LoadNetworkTileEvaluators() {
    // {network type, complex instance, simple instance} in ReloadNetworkTypeInfo's order; the power line (5) has no rules.
    static constexpr std::uint32_t kFiles[12][3] = {
        {0, 8, 7},   {10, 18, 17}, {11, 20, 19}, {1, 6, 5},  {8, 16, 15}, {9, 22, 21},
        {3, 10, 9},  {2, 2, 1},    {12, 24, 23}, {6, 14, 13}, {4, 4, 3},  {7, 12, 11},
    };
    int loaded = 0;
    for (std::uint32_t type = 0; type < kNetworkTypeCount; ++type) {   // cSC4NetworkTypeInfo::Clear (0x1001c7308)
        for (cSC4TileEvaluator* evaluator : EvaluatorsOf(type)) delete evaluator;
        EvaluatorsOf(type).clear();
    }
    for (const auto& file : kFiles) {
        for (int which = 1; which <= 2; ++which) {
            auto* evaluator = new cSC4TileEvaluator();
            if (evaluator->AddRulesFromResource(cGZPersistResourceKey{kNetworkRulesType, kNetworkRulesGroup, file[which]})) ++loaded;
            EvaluatorsOf(file[0]).push_back(evaluator);
        }
    }
    return loaded;
}

}  // namespace sc4

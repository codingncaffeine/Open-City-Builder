// The bridge types of a network type and its support structures, as cSC4NetworkTypeInfo loads them: LoadBridgeTypes reads the
// network ini's "<Type>BridgeExemplars" section and each named bridge exemplar (ReadBridgeTypeInfo), LoadSupports every support
// structure exemplar of a group (ReadSupportStructureInfos). Recovered from the Mac i386 build with symbols.
#include "sc4/NetworkTool.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <unordered_map>

#include "gz/cIGZFile.h"

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "gz/cIGZPersistResourceManager.h"
#include "rz/RZConvert.h"
#include "rz/RZStringUtils.h"
#include "rz/cRZIniFile.h"
#include "sc4/Exemplar.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/ResourceAsFile.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"

namespace sc4 {

namespace {

constexpr std::uint32_t kExemplarType = 0x6534284a;
constexpr std::uint32_t kExemplarIID = 0xa52160f5;

// SC4GeometryUtilities::RotatePoint2D about the origin, as ReadBridgeTypeInfo / ReadSupportStructureInfo append the three
// quarter turns of a 4-corner footprint (8 floats in, 32 out).
void AppendQuarterTurns(std::vector<float>& corners) {
    if (corners.size() < 8) corners.resize(8, 0.0f);
    float base[8];
    std::copy(corners.begin(), corners.begin() + 8, base);
    for (int turn = 1; turn != 4; ++turn) {
        const float angle = static_cast<float>(turn) * 1.5707964f;
        const float origin[2] = {0.0f, 0.0f};
        for (int i = 0; i != 4; ++i) {
            float turned[2];
            SC4GeometryUtilities::RotatePoint2D(origin, &base[i * 2], angle, turned);
            corners.push_back(turned[0]);
            corners.push_back(turned[1]);
        }
    }
}

// The model ids and rotations of one piece list (ids: a uint32 array property, rotations: a uint8 array property), at most
// `count` entries, as ReadBridgeTypeInfo copies them out of the variants.
void ReadModels(const cISCPropertyHolder* holder, std::uint32_t idProperty, std::uint32_t rotationProperty, std::uint32_t count,
                std::vector<cSC4NetworkModelInfo>& models) {
    std::vector<std::uint32_t> ids;
    if (cSCPropertyHelp::GetPropertyValue(holder, idProperty, ids)) {
        const std::size_t n = std::min<std::size_t>(ids.size(), count);
        for (std::size_t i = 0; i < n; ++i) models[i].id = ids[i];
    }
    std::vector<std::uint8_t> rotations;
    if (cSCPropertyHelp::GetPropertyValue(holder, rotationProperty, rotations)) {
        // Quirk kept: the original compares the variant's count truncated to a byte with the model count.
        std::uint32_t n = static_cast<std::uint32_t>(rotations.size()) & 0xff;
        if (count < n) n = count;
        for (std::uint32_t i = 0; i < n; ++i) models[i].rotation = rotations[i];
    }
}

void ReadFloats(const cISCPropertyHolder* holder, std::uint32_t property, std::uint32_t count, std::vector<float>& out) {
    std::vector<float> values;
    if (!cSCPropertyHelp::GetPropertyValue(holder, property, values)) return;
    const std::size_t n = std::min<std::size_t>(values.size(), count);
    for (std::size_t i = 0; i < n; ++i) out[i] = values[i];
}

template <typename T>
void ReadOr(const cISCPropertyHolder* holder, std::uint32_t property, T& out, T fallback) {
    if (!cSCPropertyHelp::GetPropertyValue(holder, property, out)) out = fallback;
}

// Three consecutive floats of a float array property, from index `at`.
bool ReadVector3Pair(const cISCPropertyHolder* holder, std::uint32_t property, float (*out)[3]) {
    std::vector<float> values;
    if (!cSCPropertyHelp::GetPropertyValue(holder, property, values) || values.size() < 6) return false;
    for (int i = 0; i != 3; ++i) {
        out[0][i] = values[static_cast<std::size_t>(i)];
        out[1][i] = values[static_cast<std::size_t>(3 + i)];
    }
    return true;
}

struct BridgeIniContext {
    std::vector<cSC4NetworkBridgeTypeInfo>* types;
};

// ReadBridgeTypeInfo's ini half: the key is the bridge id, the value "type,group,instance,name" names the exemplar.
void ReadBridgeTypeEntry(const cRZString& key, const cRZString& value, void* context) {
    auto& types = *static_cast<BridgeIniContext*>(context)->types;
    types.emplace_back();
    cSC4NetworkBridgeTypeInfo& info = types.back();
    info.id = RZ::ConvertToUint(key.ToChar());
    cRZString fields[4];
    unsigned count = 4;
    RZParseDelimitedText(value.ToChar(), value.Strlen(), ',', fields, count);
    bool loaded = false;
    if (count == 4) {
        const cGZPersistResourceKey exemplarKey{RZ::ConvertToUint(fields[0].ToChar()), RZ::ConvertToUint(fields[1].ToChar()),
                                                RZ::ConvertToUint(fields[2].ToChar())};
        void* raw = nullptr;
        if (SL::spRM && SL::spRM->GetResource(exemplarKey, kExemplarIID, &raw, 0, nullptr) && raw) {
            auto* exemplar = static_cast<cISCResExemplar*>(raw);
            info.name = fields[3].ToChar();
            ReadNetworkBridgeTypeInfo(exemplar->AsISCPropertyHolder(), info);
            exemplar->Release();
            loaded = true;
        }
    }
    // A line that does not name a loadable exemplar leaves no record (the original pops the entry it pushed).
    if (!loaded) types.pop_back();
}

// ReadSupportStructureInfos' enumeration filter (cSCResFilterFixedGroupAndTypeId(0x6534284a, group)).
class GroupAndTypeFilter final : public cIGZPersistResourceKeyFilter {
public:
    explicit GroupAndTypeFilter(std::uint32_t group) : group_(group) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == kExemplarType && key.group == group_; }

private:
    std::uint32_t group_;
};

}  // namespace

// ============================================================================================================
// bridge types and support structures
// ============================================================================================================

bool ReadNetworkBridgeTypeInfo(const cISCPropertyHolder* p, cSC4NetworkBridgeTypeInfo& b) {
    using H = cSCPropertyHelp;
    if (!p) return false;
    H::GetPropertyValue(p, 0x482cb441, b.waterClearance);
    ReadOr<std::uint32_t>(p, 0x482cb45f, b.width, 1);
    ReadOr<std::uint32_t>(p, 0x482cb442, b.rampLength, 0);
    b.rampModels.assign(static_cast<std::size_t>(b.rampLength) * b.width, cSC4NetworkModelInfo{});
    b.rampRise.assign(b.rampLength + 1, 0.0f);
    b.rampDrop.assign(b.rampLength, 0.0f);
    b.rampSupportHeights.assign(b.rampLength, 0.0f);
    if (b.rampLength == 0) {
        b.lastSupportedRamp = 0;
    } else {
        ReadModels(p, 0x482cb443, 0x482cb444, b.rampLength * b.width, b.rampModels);
        std::vector<float> heights;
        if (H::GetPropertyValue(p, 0x482cb44d, heights)) {
            const std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(heights.size()), b.rampLength);
            for (std::uint32_t i = 0; i < n; ++i) b.rampSupportHeights[i] = heights[i];
            b.lastSupportedRamp = 0;
            for (std::uint32_t i = 1; i < n; ++i)
                if (0.0f <= b.rampSupportHeights[i]) b.lastSupportedRamp = i;
        }
    }
    H::GetPropertyValue(p, 0x482cb445, b.deckModel);
    H::GetPropertyValue(p, 0x482cb446, b.deckRotation);
    b.pylonLength = 0;
    b.pylonSpacing = 0;
    b.pylonInset = 0;
    H::GetPropertyValue(p, 0x482cb448, b.pylonLength);
    if (b.pylonLength != 0) {
        b.pylonModels.assign(static_cast<std::size_t>(b.pylonLength) * b.width, cSC4NetworkModelInfo{});
        b.pylonSupportHeights.assign(b.pylonLength, 0.0f);
        H::GetPropertyValue(p, 0x482cb447, b.pylonSpacing);
        ReadOr<std::uint32_t>(p, 0x482cb459, b.pylonInset, b.pylonSpacing);
        b.pylonSpacingFactor = 0.0f;
        H::GetPropertyValue(p, 0x482cd465, b.pylonSpacingFactor);
        ReadModels(p, 0x482cb449, 0x482cb44a, b.pylonLength * b.width, b.pylonModels);
        ReadFloats(p, 0x482cb44c, b.pylonLength, b.pylonSupportHeights);
    }
    H::GetPropertyValue(p, 0x482cb44e, b.rampFootprint);
    // The ramp rise / drop tables: from the far end inwards, each 0x482cb44b pair (a, b) adds (b - a) to the rise and the
    // drop of a step is b plus the rise beyond it. The land clearance is the whole rise.
    b.landClearance = 0.0f;
    std::vector<float> pairs;
    if (b.rampLength != 0 && H::GetPropertyValue(p, 0x482cb44b, pairs)) {
        pairs.resize(std::max<std::size_t>(pairs.size(), static_cast<std::size_t>(b.rampLength) * 2), 0.0f);
        b.rampRise[b.rampLength] = 0.0f;
        for (int i = static_cast<int>(b.rampLength) - 1; i >= 0; --i) {
            const float a = pairs[static_cast<std::size_t>(i) * 2], c = pairs[static_cast<std::size_t>(i) * 2 + 1];
            b.rampRise[static_cast<std::size_t>(i)] = (c - a) + b.rampRise[static_cast<std::size_t>(i) + 1];
            b.rampDrop[static_cast<std::size_t>(i)] = c + b.rampRise[static_cast<std::size_t>(i) + 1];
        }
        b.landClearance = b.rampRise[0];
    }
    if (!H::GetPropertyValue(p, 0x482cb45e, b.pylonFootprint)) b.pylonFootprint = b.rampFootprint;
    AppendQuarterTurns(b.rampFootprint);
    AppendQuarterTurns(b.pylonFootprint);
    ReadOr<std::uint32_t>(p, 0x482cb458, b.minSpan, b.rampLength * 2 + 2);
    ReadOr<std::uint32_t>(p, 0x482cb467, b.maxSpan, 100000);
    H::GetPropertyValue(p, 0x482cb44f, b.isLand);
    H::GetPropertyValue(p, 0x482cb450, b.isWater);
    ReadOr(p, 0x482cb455, b.suspensionSag, 0.0f);
    ReadOr(p, 0x482cb456, b.suspensionEndSag, 0.0f);
    ReadOr(p, 0x482cb457, b.suspensionHeight, 0.0f);
    ReadOr<std::uint32_t>(p, 0x482cb45a, b.minPylons, 0);
    ReadOr<std::uint32_t>(p, 0x482cb45b, b.maxPylons, 100);
    ReadOr<std::uint32_t>(p, 0x482cb45c, b.suspensionTopTexture, 0);
    ReadOr<std::uint32_t>(p, 0x482cb45d, b.suspensionHangerTexture, 0);
    ReadOr(p, 0x482cb471, b.suspensionTileHeight, 0.0f);
    ReadOr(p, 0x482cd460, b.suspensionDeckOffset, 0.0f);
    ReadOr(p, 0x482cd461, b.suspensionInset, 0.0f);
    ReadOr(p, 0x482cd462, b.pylonDeckOffset, 0.0f);
    ReadOr(p, 0x482cd463, b.minWaterDepth, 0.0f);
    ReadOr(p, 0x482cd464, b.maxWaterDepth, 10000.0f);
    ReadOr(p, 0x482cb468, b.passageClearance, 0.0f);
    ReadOr(p, 0x482cb472, b.deckOffset, 0.0f);
    {
        cRZString name;
        if (H::GetPropertyValue(p, 0x482cd466, name)) b.name = name.ToChar();
    }
    std::vector<std::uint32_t> keys;
    if (H::GetPropertyValue(p, 0x482cb46f, keys) && keys.size() >= 2) {
        b.bitmapKeyA = keys[0];
        b.bitmapKeyB = keys[1];
    } else {
        b.bitmapKeyA = b.bitmapKeyB = 0;
    }
    keys.clear();
    if (H::GetPropertyValue(p, 0x482cb470, keys) && keys.size() >= 2) {
        b.unknown150[0] = keys[0];
        b.unknown150[1] = keys[1];
    } else {
        b.unknown150[0] = b.unknown150[1] = 0;
    }
    // The cables: every one of the six properties must be there.
    b.hasCables = false;
    if (H::GetPropertyValue(p, 0x482cb46b, b.cableTexture) && H::GetPropertyValue(p, 0x482cb46c, b.cableHalfWidth) &&
        ReadVector3Pair(p, 0x482cb469, b.cableTowerA) && ReadVector3Pair(p, 0x482cb46a, b.cableTowerB) &&
        ReadVector3Pair(p, 0x482cb46d, b.cableDeck)) {
        for (int i = 0; i != 2; ++i) {
            b.cableDeckMirrored[i][0] = -b.cableDeck[i][0];
            b.cableDeckMirrored[i][1] = b.cableDeck[i][1];
            b.cableDeckMirrored[i][2] = b.cableDeck[i][2];
        }
        if (H::GetPropertyValue(p, 0x482cb46e, b.cableCount)) b.hasCables = true;
    }
    return true;
}

namespace {

// ============================================================================================================
// the bridge specialization script
// ============================================================================================================

// (anonymous namespace)::cParser (i386 0x0015186a..0x00152042): a line tokenizer. Tokens: 0 = the end of a line (a line
// break, ';', or the end of the text), 0x100 = an unsigned number (decimal, octal with a leading 0, or 0x hex), 0x101 = a
// number with a '.', 0x102 = a word (a letter, then letters, digits and '_'); any other character is its own token. '#'
// starts a comment to the end of the line; spaces and tabs separate tokens. One token can be pushed back.
class cParser {
public:
    enum : int { kEnd = 0, kInteger = 0x100, kFloat = 0x101, kWord = 0x102 };

    void SetSource(const char* text, long size) {
        start_ = pos_ = text;
        end_ = text + size;
    }
    bool AtEnd() const { return pos_ >= end_; }
    void PushToken(int token) { pushed_ = token; }
    const std::string& Word() const { return word_; }
    std::uint32_t Integer() const { return integer_; }

    // i386 0x00151cf2
    int NextToken() {
        if (pushed_ >= 0) {
            const int token = pushed_;
            pushed_ = -1;
            return token;
        }
        const char* first = nullptr;
        unsigned char c = 0;
        do {
            for (;;) {
                if (pos_ >= end_) return kEnd;
                first = pos_;
                c = static_cast<unsigned char>(*pos_++);
                if (c != '#') break;
                while (pos_ < end_ && *pos_ != '\r' && *pos_ != '\n') ++pos_;
            }
        } while (c == ' ' || c == '\t');
        if (c == '\r' || c == '\n' || c == ';') return kEnd;
        if (c == '0' && pos_ < end_ && *pos_ == 'x') {
            const char* digits = first + 2;
            pos_ = digits;
            while (pos_ < end_ && std::isxdigit(static_cast<unsigned char>(*pos_))) ++pos_;
            integer_ = RZAtoUint32HexA(std::string(digits, pos_).c_str());
            return kInteger;
        }
        if (c - static_cast<unsigned>('0') < 10 || c == '.' ||
            (c == '-' && pos_ < end_ &&
             (static_cast<unsigned>(static_cast<int>(static_cast<signed char>(*pos_)) - '0') < 10 || *pos_ == '.'))) {
            bool dot = c == '.';
            while (pos_ < end_) {
                const unsigned char d = static_cast<unsigned char>(*pos_);
                if (d == '.') dot = true;
                else if (d - static_cast<unsigned>('0') > 9 && d != 'x') break;
                ++pos_;
            }
            const std::string text(first, pos_);
            if (dot) {
                real_ = static_cast<float>(RZAtofEnglishA(text.c_str()));
                return kFloat;
            }
            integer_ = static_cast<std::uint32_t>(std::strtoul(text.c_str(), nullptr, 0));
            return kInteger;
        }
        if (std::isalpha(c)) {
            while (pos_ < end_ && (*pos_ == '_' || std::isalnum(static_cast<unsigned char>(*pos_)))) ++pos_;
            word_.assign(first, pos_);
            return kWord;
        }
        return static_cast<int>(static_cast<signed char>(c));
    }
    // i386 0x00151f98: an integer converts to the float.
    bool ParseFloat(float& out) {
        const int token = NextToken();
        if (token == kFloat) out = real_;
        else if (token == kInteger) out = static_cast<float>(integer_);
        else return false;
        return true;
    }
    bool ParseUint(std::uint32_t& out) {   // i386 0x00151ff6
        const int token = NextToken();
        if (token == kInteger) out = integer_;
        return token == kInteger;
    }
    bool Expect(int token) { return NextToken() == token; }   // i386 0x00152026
    bool ExpectEnd() { return NextToken() == kEnd; }          // i386 0x00152042

private:
    const char* start_ = nullptr;   // +0x04
    const char* pos_ = nullptr;     // +0x08
    const char* end_ = nullptr;     // +0x0c
    int pushed_ = -1;               // +0x10
    std::string word_;              // +0x14
    std::uint32_t integer_ = 0;     // +0x18
    float real_ = 0.0f;             // +0x1c
};

// (anonymous namespace)::cBridgeSpecializationParser (ctor i386 0x00152484). The script:
//   section NAME ... end          a named piece list for later "piece" lines (in the scope it is read in)
//   bridge ID ... end             the layouts of the bridge type whose ini key is ID (its sections are local to it):
//     length N ... end            the layout of a span of exactly N steps (N within the type's span range, once)
//   stop                          the end of the script
// and inside a section or a length:
//   height H                      added to every height of this layout when it is done
//   piece ITEM, ITEM ...          ITEM = [COUNT *] <MODEL, ROTATION [, STEP RISE [, HEIGHT]]> [: SUPPORT | pylon | end, ...]
//                                     or [COUNT *] NAME | rev(NAME) | length(N) (a section, reversed, or another length)
class cBridgeSpecializationParser : public cParser {
public:
    using Spec = cSC4NetworkBridgeSpecialization;
    cBridgeSpecializationParser(std::vector<cSC4NetworkBridgeTypeInfo>& types, int width) : types_(types), width_(width) {}

    // i386 0x0015604e
    bool Parse(const char* text, long size) {
        SetSource(text, size);
        PushScope();
        for (;;) {
            if (AtEnd()) return true;
            const int token = NextToken();
            if (token == kEnd) continue;
            if (token != kWord) return false;
            if (Word() == "stop") return true;
            bool ok;
            if (Word() == "section") {
                if (!Expect(kWord)) return false;
                ok = ParseBridgeSpecialization(nullptr, scopes_.back()[Word()]);
            } else if (Word() == "bridge") {
                ok = ParseBridge();
            } else {
                return false;
            }
            if (!ok) return false;
        }
    }

private:
    void PushScope() { scopes_.emplace_back(); }   // i386 0x00155e6c

    // i386 0x00155eb4
    bool ParseBridge() {
        if (NextToken() != kInteger || !ExpectEnd()) return false;
        const std::uint32_t id = Integer();
        cSC4NetworkBridgeTypeInfo* type = nullptr;
        for (cSC4NetworkBridgeTypeInfo& candidate : types_) {
            if (candidate.id == id) {
                type = &candidate;
                break;
            }
        }
        if (!type) return false;
        PushScope();
        for (;;) {
            if (AtEnd()) {
                scopes_.pop_back();
                return true;
            }
            const int token = NextToken();
            if (token == kEnd) continue;
            if (token != kWord) return false;
            bool ok;
            if (Word() == "section") {
                if (!Expect(kWord)) return false;
                ok = ParseBridgeSpecialization(nullptr, scopes_.back()[Word()]);
            } else if (Word() == "length") {
                ok = ParseBridgeInstance(*type);
            } else if (Word() == "end") {
                if (!ExpectEnd()) return false;
                scopes_.pop_back();
                return true;
            } else {
                return false;
            }
            if (!ok) return false;
        }
    }

    // i386 0x00153368: one span length, within the type's range and not given before.
    bool ParseBridgeInstance(cSC4NetworkBridgeTypeInfo& type) {
        if (!Expect(kInteger) || !ExpectEnd()) return false;
        const std::uint32_t span = Integer();
        if (span < type.minSpan || type.maxSpan < span) return false;
        const int key = static_cast<int>(span);
        if (type.specializations.find(key) != type.specializations.end()) return false;
        return ParseBridgeSpecialization(&type, type.specializations[key]);
    }

    // One piece appended; every `width` pieces close a step, whose far boundary sits `rise` above the last.
    void Append(Spec& spec, const Spec::Piece& piece, float rise) {
        spec.pieces.push_back(piece);
        if (spec.pieces.size() % static_cast<std::size_t>(width_) == 0) spec.heights.push_back(spec.heights.back() + rise);
    }

    // i386 0x00152b9e
    bool ParseBridgeSpecialization(cSC4NetworkBridgeTypeInfo* type, Spec& spec) {
        float lift = 0.0f;
        spec.heights.push_back(0.0f);
        for (;;) {
            if (AtEnd()) break;
            int token = NextToken();
            if (token == kEnd) continue;
            if (token != kWord) return false;
            if (Word() == "height") {
                if (!ParseFloat(lift) || !ExpectEnd()) return false;
                continue;
            }
            if (Word() == "end") {
                if (!ExpectEnd()) return false;
                break;
            }
            if (Word() != "piece") return false;
            while (!AtEnd()) {
                token = NextToken();
                if (token == kEnd) break;
                int count = 1;
                if (token == kInteger) {
                    count = static_cast<int>(Integer());
                    if (!Expect('*')) return false;
                    token = NextToken();
                }
                if (token == kWord) {
                    bool reversed = false;
                    if (Word() == "rev") {
                        if (!Expect('(') || !Expect(kWord) || !Expect(')')) return false;
                        reversed = true;
                    }
                    const Spec* from = nullptr;
                    if (Word() == "length") {
                        if (!type || !Expect('(')) return false;
                        std::uint32_t span = 0;
                        if (!ParseUint(span) || !Expect(')')) return false;
                        const auto found = type->specializations.find(static_cast<int>(span));
                        if (found == type->specializations.end()) return false;
                        from = &found->second;
                    } else {
                        for (auto& scope : scopes_) {
                            const auto found = scope.find(Word());
                            if (found != scope.end()) {
                                from = &found->second;
                                break;
                            }
                        }
                        if (!from) return false;
                    }
                    if (!CopyPieces(spec, *from, count, reversed)) return false;
                } else if (token == '<') {
                    std::uint32_t model = 0, rotation = 0;
                    float rise = 0.0f, height = 0.0f, support = -1.0f;
                    if (!ParseUint(model) || !Expect(',') || !ParseUint(rotation)) return false;
                    token = NextToken();
                    if (token == ',') {
                        if (!ParseFloat(rise)) return false;
                        token = NextToken();
                        if (token == ',') {
                            if (!ParseFloat(height)) return false;
                            token = NextToken();
                        }
                    }
                    if (token != '>') return false;
                    int attribute = 0;
                    token = NextToken();
                    if (token == ':') {
                        do {
                            token = NextToken();
                            if (token == kInteger || token == kFloat) {
                                PushToken(token);
                                if (!ParseFloat(support)) return false;
                            } else if (token == kWord && Word() == "pylon") {
                                attribute = 1;
                            } else if (token == kWord && Word() == "end") {
                                attribute = 2;
                            } else {
                                return false;
                            }
                            token = NextToken();
                        } while (token == ',');
                    }
                    PushToken(token);
                    for (int i = 0; i != count; ++i) {
                        Spec::Piece piece;
                        piece.height = spec.heights.back() + height;
                        piece.model = model;
                        piece.rotation = static_cast<std::uint8_t>(rotation);
                        piece.supportHeight = support;
                        piece.attribute = attribute;
                        Append(spec, piece, rise);
                    }
                } else {
                    return false;
                }
            }
        }
        for (Spec::Piece& piece : spec.pieces) piece.height += lift;
        for (float& h : spec.heights) h += lift;
        return true;
    }

    // A referenced layout copied `count` times onto the end of this one, its heights continuing from this one's last step
    // boundary; reversed, it runs backwards and every piece turns half way round.
    bool CopyPieces(Spec& spec, const Spec& from, int count, bool reversed) {
        if (&from == &spec || from.heights.empty()) return true;
        const std::size_t size = from.pieces.size();
        const std::size_t width = static_cast<std::size_t>(width_);
        for (int i = 0; i != count; ++i) {
            const float base = spec.heights.back();
            if (reversed) {
                for (std::size_t j = size; j-- > 0;) {
                    Spec::Piece piece = from.pieces[j];
                    piece.rotation ^= 2;
                    piece.height = (base - from.heights.back()) + piece.height;
                    spec.pieces.push_back(piece);
                    if (spec.pieces.size() % width == 0) spec.heights.push_back((base + from.heights[j / width]) - from.heights.back());
                }
            } else {
                for (std::size_t j = 0; j < size; ++j) {
                    Spec::Piece piece = from.pieces[j];
                    piece.height = (base - from.heights[0]) + piece.height;
                    spec.pieces.push_back(piece);
                    if (spec.pieces.size() % width == 0) spec.heights.push_back((base + from.heights[j / width + 1]) - from.heights[0]);
                }
            }
        }
        return true;
    }

    std::vector<cSC4NetworkBridgeTypeInfo>& types_;               // +0x20
    std::list<std::unordered_map<std::string, Spec>> scopes_;     // +0x24 (hash maps keyed by cRZString)
    int width_ = 1;                                               // +0x2c
};

}  // namespace

bool ParseNetworkBridgeSpecializations(const char* text, long size, std::vector<cSC4NetworkBridgeTypeInfo>& types, int width) {
    if (width < 1) return false;
    cBridgeSpecializationParser parser(types, width);
    return parser.Parse(text, size);
}

namespace {

// ReadBridgeTypeSpecializations (i386 0x0015616e): the script {0x0a5bcf4b, 0xaa5bcf57, instance}, when there is one.
void ReadBridgeTypeSpecializations(std::uint32_t instance, std::vector<cSC4NetworkBridgeTypeInfo>& types, int width) {
    cIGZFile* file = nullptr;
    if (SC4GetResourceAsFile(cGZPersistResourceKey{0x0a5bcf4b, 0xaa5bcf57, instance}, &file, false) && file) {
        const std::uint32_t length = file->Length();
        std::vector<char> text(length == 0xffffffffu ? 0u : length);
        file->Read(text.data(), static_cast<std::uint32_t>(text.size()));
        ParseNetworkBridgeSpecializations(text.data(), static_cast<long>(text.size()), types, width);
    }
    if (file) file->Release();
}

}  // namespace

void LoadNetworkBridgeTypes(::cRZIniFile& ini, const char* section, std::vector<cSC4NetworkBridgeTypeInfo>& out,
                            std::uint32_t specializationInstance, int width) {
    // cSC4NetworkTypeInfo::LoadBridgeTypes (i386 0x00156290)
    BridgeIniContext context{&out};
    ini.EnumEntries(cRZString(section), &ReadBridgeTypeEntry, &context);
    ReadBridgeTypeSpecializations(specializationInstance, out, width);
}

void LoadNetworkSupportStructures(std::uint32_t group, std::map<std::uint32_t, cSC4NetworkSupportStructInfo>& out) {
    if (!SL::spRM) return;
    GroupAndTypeFilter filter(group);
    cIGZPersistResourceKeyList* list = nullptr;
    SL::spRM->GetAvailableResourceList(&list, &filter);
    if (!list) return;
    std::vector<cGZPersistResourceKey> keys;
    for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
    list->Release();
    for (const cGZPersistResourceKey& key : keys) {
        // ReadSupportStructureInfo (i386 0x0015495c)
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, kExemplarIID, &raw, 0, nullptr) || !raw) continue;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        const cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        std::uint32_t id = 0;
        cSC4NetworkSupportStructInfo info;
        cSCPropertyHelp::GetPropertyValue(holder, 0xe974eaa1, id);
        cSCPropertyHelp::GetPropertyValue(holder, 0xe974eaa2, info.edges);
        cSCPropertyHelp::GetPropertyValue(holder, 0xe974eaa3, info.footprint);
        AppendQuarterTurns(info.footprint);
        out[id] = info;
        exemplar->Release();
    }
}

}  // namespace sc4

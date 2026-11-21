#include "sc4/NetworkIntersectionRules.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "rz/RZStringUtils.h"
#include "rz/cRZIniFile.h"
#include "rz/cRZString.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkTool.h"

namespace sc4 {
namespace nSC4Networks {

namespace {

// cSC4NetworkTool::kNetworkTypeNames: the names a CheckType compares (strcasecmp) with.
constexpr const char* kNetworkTypeNames[kNetworkTypeCount] = {"Road", "Rail", "Highway", "Street", "Pipe", "PowerLine", "Avenue",
                                                            "Subway", "LightRail", "Monorail", "OneWayRoad", "DirtRoad", "GroundHighway"};

// RZConvertToUint32A: hex after a "0x" prefix, else decimal.
std::uint32_t ConvertToUint32(const char* text) {
    while (*text == ' ' || *text == '\t') ++text;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) return static_cast<std::uint32_t>(std::strtoul(text + 2, nullptr, 16));
    return static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10));
}

// The one bit pair per side the low two bits of each edge byte give (D(e) of the one-way checks).
std::uint32_t SideMask(std::uint32_t edges) {
    return ((((edges & 0x3030303u) + 0x3030303u) & 0x4040404u) * 0xc30c3u & 0xfffffffu) >> 20;
}

// TranslatePointArray / RotatePoint<SC4Point<int>> / TransposePointArray (0x006a43c6, 0x006a411e, 0x006a432e).
template <typename T>
void TranslatePoints(std::vector<T>& points, int dx, int dz) {
    for (T& p : points) {
        p.x += dx;
        p.z += dz;
    }
}
template <typename T>
void RotatePoint(T& p, int turns) {
    if (turns == 2) {
        p.x = -p.x;
        p.z = -p.z;
    } else if (turns == 3) {
        const auto x = p.x;
        p.x = p.z;
        p.z = -x;
    } else if (turns == 1) {
        const auto z = p.z;
        p.z = p.x;
        p.x = -z;
    }
}
template <typename T>
void RotatePoints(std::vector<T>& points, int turns) {
    if (turns == 0) return;
    for (T& p : points) RotatePoint(p, turns);
}
template <typename T>
void TransposePoints(std::vector<T>& points) {
    for (T& p : points) std::swap(p.x, p.z);
}

// RotateMap (0x00197930): the grid of `width` columns turned by `turns` quarter turns. A half turn reverses it and mirrors the
// centre; an odd turn then makes the columns rows. For the constraint grid the direction codes turn with it.
void RotateMap(std::vector<std::uint8_t>& map, int& width, cIntPoint& center, int turns, bool constraints) {
    if (width == 0) return;
    const int size = static_cast<int>(map.size());
    const int columns = width;
    const int rows = size / columns;
    if (turns & 2) {
        center.x = columns - center.x - 1;
        center.z = rows - center.z - 1;
        for (int a = 0, b = size - 1; a < b; ++a, --b) std::swap(map[static_cast<std::size_t>(a)], map[static_cast<std::size_t>(b)]);
    }
    if (turns & 1) {
        std::vector<std::uint8_t> turned(static_cast<std::size_t>(size));
        for (int c = 0; c < columns; ++c) {
            for (int r = 0, from = rows - 1; r < rows; ++r, --from) {
                std::uint8_t value = map[static_cast<std::size_t>(columns * from + c)];
                if (constraints) {
                    switch (value) {
                        case 1: value = 2; break;
                        case 2: value = 1; break;
                        case 3: value = 4; break;
                        case 4: value = 3; break;
                        case 5: value = 6; break;
                        case 6: value = 5; break;
                        default: break;
                    }
                }
                turned[static_cast<std::size_t>(r + c * rows)] = value;
            }
        }
        width = rows;
        const int z = center.z;
        center.z = center.x;
        center.x = rows - z - 1;
        map.swap(turned);
    }
}

// TransposeMap (0x00197d56): rows become columns; the constraint grid swaps '-' and '|'.
void TransposeMap(std::vector<std::uint8_t>& map, int& width, cIntPoint& center, bool constraints) {
    std::swap(center.x, center.z);
    if (width == 0) return;
    const int columns = width;
    const int rows = static_cast<int>(map.size()) / columns;
    std::vector<std::uint8_t> out(map.size());
    int at = 0;
    for (int c = 0; c < columns; ++c) {
        for (int r = 0; r < rows; ++r) {
            std::uint8_t value = map[static_cast<std::size_t>(c + r * columns)];
            if (constraints) value = value == 1 ? 2 : value == 2 ? 1 : value;
            out[static_cast<std::size_t>(at + r)] = value;
        }
        at += rows;
    }
    width = rows;
    map.swap(out);
}

// The CheckType tokenizer ((anonymous)::cParser, 0x0015186a / NextToken 0x00151cf2): '#' comments run to the end of the line,
// ';' and a line break end the value, "0x" starts a hex number, a digit (or '.', or '-' before one) a number, a letter an
// identifier (letters, digits, '_'), anything else is its own token. One token can be pushed back.
class cParser {
public:
    enum { kEnd = 0, kUint = 0x100, kFloat = 0x101, kIdent = 0x102 };
    explicit cParser(const std::string& text) : at_(text.data()), end_(text.data() + text.size()) {}

    int NextToken() {
        if (pushed_ >= 0) {
            const int token = pushed_;
            pushed_ = -1;
            return token;
        }
        unsigned char c = 0;
        const char* start = nullptr;
        for (;;) {
            if (at_ >= end_) return kEnd;
            start = at_;
            c = static_cast<unsigned char>(*at_++);
            if (c == '#') {
                while (at_ < end_ && *at_ != '\r' && *at_ != '\n') ++at_;
                continue;
            }
            if (c != ' ' && c != '\t') break;
        }
        if (c == '\r' || c == '\n' || c == ';') return kEnd;
        if (c == '0' && at_ < end_ && *at_ == 'x') {
            const char* digits = ++at_;
            while (at_ < end_ && std::isxdigit(static_cast<unsigned char>(*at_))) ++at_;
            uint_ = RZAtoUint32HexA(std::string(digits, static_cast<std::size_t>(at_ - digits)).c_str());
            return kUint;
        }
        if ((c - '0') < 10u || c == '.' ||
            (c == '-' && at_ < end_ && ((static_cast<unsigned char>(*at_) - '0') < 10u || *at_ == '.'))) {
            bool isFloat = c == '.';
            while (at_ < end_) {
                const unsigned char d = static_cast<unsigned char>(*at_);
                if (d == '.') isFloat = true;
                else if ((d - '0') > 9u && d != 'x') break;
                ++at_;
            }
            const std::string number(start, static_cast<std::size_t>(at_ - start));
            if (isFloat) {
                float_ = static_cast<float>(RZAtofEnglishA(number.c_str()));
                return kFloat;
            }
            uint_ = static_cast<std::uint32_t>(std::strtoul(number.c_str(), nullptr, 0));
            return kUint;
        }
        if (std::isalpha(c)) {
            while (at_ < end_ && (*at_ == '_' || std::isalnum(static_cast<unsigned char>(*at_)))) ++at_;
            ident_.assign(start, static_cast<std::size_t>(at_ - start));
            return kIdent;
        }
        return static_cast<int>(static_cast<signed char>(c));
    }
    bool Expect(int token) { return NextToken() == token; }
    bool ParseUint(std::uint32_t& out) {
        if (NextToken() != kUint) return false;
        out = uint_;
        return true;
    }
    void PushBack(int token) { pushed_ = token; }
    const std::string& Ident() const { return ident_; }

private:
    const char* at_;
    const char* end_;
    int pushed_ = -1;
    std::uint32_t uint_ = 0;
    float float_ = 0.0f;
    std::string ident_;
};

// IntersectionCallbackData (0x00682538): the reader's state across the sections of one file.
struct IntersectionCallbackData {
    cRZIniFile* ini = nullptr;                 // +0x0
    IntersectionRuleMap* rules = nullptr;      // +0x4
    cIntRule* rule = nullptr;                  // +0x8 the rule whose section is being read
    std::list<std::string> cellLayout;         // +0xc
    std::list<std::string> constraintLayout;   // +0x14
    std::int32_t cursor = -1;                  // +0x24 the head of the last ordering line (kept across sections)
};

bool IsKey(const cRZString& key, const char* name) { return strcasecmp(key.ToChar(), name) == 0; }

// ReadIntersectionEntries (0x00154b0c): one key of a rule section. "PieceType" and "ResetPieces" are known to the reader but
// never compared, so those keys are ignored.
void ReadIntersectionEntry(const cRZString& key, const cRZString& value, void* context) {
    auto* data = static_cast<IntersectionCallbackData*>(context);
    cIntRule* rule = data->rule;
    const char* text = value.ToChar();
    if (IsKey(key, "Piece")) {
        cIntPiece piece;
        piece.x = static_cast<float>(RZAtofEnglishA(text));
        const char* comma = std::strchr(text, ',');
        if (!comma) return;
        piece.z = static_cast<float>(RZAtofEnglishA(comma + 1));
        comma = std::strchr(comma + 1, ',');
        if (!comma) return;
        int rotation = 0, flip = 0;
        unsigned int id = 0;
        if (std::sscanf(comma, ", %i , %i , 0x%x", &rotation, &flip, &id) != 3) return;
        piece.id = id;
        piece.rotation = static_cast<std::uint8_t>(rotation);
        piece.flip = static_cast<std::uint8_t>(flip);
        rule->pieces.push_back(piece);
    } else if (IsKey(key, "StepOffsets")) {
        int a = 0, b = 0;
        if (std::sscanf(text, " %d , %d", &a, &b) != 2) return;
        rule->stepOffsetA = a;
        rule->stepOffsetB = b;
    } else if (IsKey(key, "CellLayout")) {
        data->cellLayout.emplace_back(text);
    } else if (IsKey(key, "ConsLayout")) {
        data->constraintLayout.emplace_back(text);
    } else if (IsKey(key, "AutoTileBase")) {
        rule->autoTileBase = ConvertToUint32(text);
    } else if (IsKey(key, "AutoPathBase")) {
        rule->autoPathBase = ConvertToUint32(text);
    } else if (IsKey(key, "CheckType")) {
        std::uint8_t tag = 0;
        cIntRule::CheckType check;
        if (!ParseCheckType(text, tag, check)) return;
        rule->checkTypes[tag] = check;
        for (std::int8_t type : check.types)
            if (type >= 0) rule->typeMask |= kFlagForNetworkType[type];
    } else if (IsKey(key, "CopyFrom")) {
        const auto found = data->rules->find(ConvertToUint32(text));
        if (found == data->rules->end()) return;
        const std::uint32_t id = rule->id;
        *rule = found->second;
        rule->id = id;
        rule->rotationPrev = rule->rotationNext = rule->typePrev = rule->typeNext = id;
    } else if (IsKey(key, "Transpose")) {
        rule->Transpose();
    } else if (IsKey(key, "Rotate")) {
        rule->Rotate(static_cast<int>(ConvertToUint32(text)));
    } else if (IsKey(key, "Translate")) {
        int a = 0, b = 0;
        std::sscanf(text, " %d , %d", &a, &b);
        rule->Translate(a, b);
    } else if (IsKey(key, "ReplacementIntersection")) {
        int a = 0, b = 0;
        std::sscanf(text, " %d , %d", &a, &b);
        rule->hasReplacement = true;
        rule->replacementX = a;
        rule->replacementZ = b;
    } else if (IsKey(key, "ConvertQueryID")) {
        rule->convertQueryId = ConvertToUint32(text);
    } else if (IsKey(key, "PlaceQueryID")) {
        rule->placeQueryId = ConvertToUint32(text);
    } else if (IsKey(key, "HandleOffset")) {
        int a = 0, b = 0;
        if (std::sscanf(text, " %d , %d", &a, &b) != 2) return;
        rule->handleX = a;
        rule->handleZ = b;
    } else if (IsKey(key, "PreviewEffect")) {
        rule->previewEffect = text;
    } else if (IsKey(key, "Costs")) {
        std::sscanf(text, " %d ", &rule->cost);
    } else if (IsKey(key, "AutoPlace")) {
        rule->autoPlace = true;
    } else if (IsKey(key, "OneWayDir")) {
        rule->oneWayDir = static_cast<std::uint8_t>(std::atoi(text));
    }
}

// The comma-separated hex ids of an ordering line (cRZString::SplitTokenDelimited + RZAtoUint32HexA).
std::vector<std::uint32_t> SplitIds(const char* text) {
    std::vector<std::uint32_t> ids;
    const char* at = text;
    for (;;) {
        const char* comma = std::strchr(at, ',');
        const std::string token = comma ? std::string(at, static_cast<std::size_t>(comma - at)) : std::string(at);
        ids.push_back(RZAtoUint32HexA(token.c_str()));
        if (!comma) break;
        at = comma + 1;
    }
    return ids;
}

// Links `rule` behind `previous` in the rotation ring whose head is `first`.
void LinkIntoRotationRing(cIntRule& rule, cIntRule& previous, cIntRule& first) {
    rule.rotationPrev = previous.id;
    rule.rotationNext = first.id;
    previous.rotationNext = rule.id;
    first.rotationPrev = rule.id;
}

// ReadIntersectionOrderingEntries (0x0015647a). "RotationRing" makes one rotation ring of the listed rules (unknown ids left
// out) and remembers its first id as the cursor; "AddTypes" does the same for its rules and puts each into the type ring of
// the rule at the same position of the cursor's rotation ring, then becomes the cursor itself. Other keys are ignored.
void ReadIntersectionOrderingEntry(const cRZString& key, const cRZString& value, void* context) {
    auto* data = static_cast<IntersectionCallbackData*>(context);
    IntersectionRuleMap& rules = *data->rules;
    if (IsKey(key, "RotationRing")) {
        data->cursor = -1;
        cIntRule* previous = nullptr;
        cIntRule* first = nullptr;
        for (std::uint32_t id : SplitIds(value.ToChar())) {
            if (data->cursor < 0) data->cursor = static_cast<std::int32_t>(id);
            const auto found = rules.find(id);
            if (found == rules.end()) continue;
            cIntRule& rule = found->second;
            if (previous) LinkIntoRotationRing(rule, *previous, *first);
            else if (!first) first = &rule;
            previous = &rule;
        }
    } else if (IsKey(key, "AddTypes")) {
        std::uint32_t current = static_cast<std::uint32_t>(data->cursor);
        cIntRule* previous = nullptr;
        cIntRule* first = nullptr;
        for (std::uint32_t id : SplitIds(value.ToChar())) {
            if (!first) data->cursor = static_cast<std::int32_t>(id);
            const auto found = rules.find(id);
            if (found == rules.end()) continue;
            cIntRule& rule = found->second;
            if (previous) LinkIntoRotationRing(rule, *previous, *first);
            else if (!first) first = &rule;
            previous = &rule;
            const auto base = rules.find(current);
            if (base == rules.end()) continue;
            cIntRule& below = base->second;
            cIntRule& next = rules[below.typeNext];
            rule.typePrev = below.id;
            rule.typeNext = next.id;
            below.typeNext = rule.id;
            next.typePrev = rule.id;
            current = below.rotationNext;
        }
    }
}

// ReadIntersectionRule (0x00156306): one section of the file.
void ReadIntersectionSection(const cRZString& section, const cRZString&, void* context) {
    auto* data = static_cast<IntersectionCallbackData*>(context);
    if (section.BeginsWith("Ordering", 8)) {
        data->ini->EnumEntries(section, &ReadIntersectionOrderingEntry, data);
        return;
    }
    if (!section.BeginsWith("HighwayIntersectionInfo_", 0x18)) return;
    const std::uint32_t id = ConvertToUint32(section.ToChar() + 0x18);
    cIntRule& rule = (*data->rules)[id];
    data->rule = &rule;
    rule.id = id;
    rule.rotationPrev = rule.rotationNext = rule.typePrev = rule.typeNext = id;
    data->cellLayout.clear();
    data->constraintLayout.clear();
    data->ini->EnumEntries(section, &ReadIntersectionEntry, data);
    if (!data->cellLayout.empty()) rule.ParseCellsFromASCIIArt(data->cellLayout);
    if (!data->constraintLayout.empty()) rule.ParseConstraintsFromASCIIArt(data->constraintLayout);
}

}  // namespace

bool ParseCheckType(const std::string& value, std::uint8_t& tag, cIntRule::CheckType& out) {
    cParser parser(value);
    cIntRule::CheckType check;
    check.checks[0].mask = check.checks[1].mask = 0;
    if (!parser.Expect(cParser::kIdent)) return false;
    const std::string name = parser.Ident();
    if (!parser.Expect('-')) return false;
    if (name.size() != 1) return false;                                   // "Bad map tag"
    int count = 0;
    for (int token = parser.NextToken(); token != cParser::kEnd; token = parser.NextToken()) {
        if (token != cParser::kIdent) return false;
        const std::string& word = parser.Ident();
        if (word == "check") {
            check.occupies = false;
        } else if (word == "multiok") {
            check.multiOk = true;
        } else if (word == "optional") {
            check.optional = true;
            check.occupies = false;
        } else {
            if (count > 1) return false;                                  // "Too many network checks in one rule"
            int type = 0;
            while (strcasecmp(word.c_str(), kNetworkTypeNames[type]) != 0)
                if (++type == static_cast<int>(kNetworkTypeCount)) return false;   // "Expected network name"
            check.types[count] = static_cast<std::int8_t>(type);
            if (!parser.Expect(':') || !parser.ParseUint(check.checks[count].edges)) return false;
            check.checks[count].mask = 0xffffffffu;
            const int next = parser.NextToken();
            if (next == ',') {
                if (!parser.ParseUint(check.checks[count].mask)) return false;
            } else {
                parser.PushBack(next);
            }
            ++count;
        }
    }
    tag = static_cast<std::uint8_t>(name[0]);
    out = check;
    return true;
}

void ReadHighwayIntersectionRules(cRZIniFile& ini, IntersectionRuleMap& rules) {
    IntersectionCallbackData data;
    data.ini = &ini;
    data.rules = &rules;
    data.cursor = -1;
    rules.clear();
    ini.EnumSections(&ReadIntersectionSection, &data);
}

// ---------------------------------------------------------------------------------------------------- the layouts

// Each line is a row (z from 0), each character a column (x from 0): '.' and ' ' nothing, '+' a new cell, '<' marks the
// centre row and '^' the centre column, a letter or digit with a check type a tagged cell (occupied unless its type only
// checks). Afterwards everything is shifted by minus the centre (a missing centre stays -1, so the cells shift by +1), and
// every occupied cell gets its tile code (row * 16 + column + 1) in the tile grid and its tile in the tile map.
void cIntRule::ParseCellsFromASCIIArt(const std::list<std::string>& lines) {
    minX = 1000;
    minZ = 1000;
    maxX = -1000;
    maxZ = -1000;
    int centerZ = -1, centerX = -1;
    const auto extend = [&](int x, int z) {
        minX = std::min(x, minX);
        maxX = std::max(maxX, x);
        minZ = std::min(z, minZ);
        maxZ = std::max(maxZ, z);
    };
    int z = 0;
    for (const std::string& line : lines) {
        int x = 0;
        for (const char ch : line) {
            const auto c = static_cast<unsigned char>(ch);
            if (c == '.' || c == ' ') {
            } else if (c == '+') {
                newCells.push_back({x, z});
                occupiedCells.push_back({x, z});
                extend(x, z);
            } else if (c == '<') {
                centerZ = z;
            } else if (c == '^') {
                centerX = x;
            } else if (std::isalnum(static_cast<int>(static_cast<signed char>(ch)))) {
                const auto found = checkTypes.find(c);
                if (found != checkTypes.end()) {
                    checkCells.push_back({x, z, c});
                    if (found->second.occupies) {
                        extend(x, z);
                        occupiedCells.push_back({x, z});
                    }
                }
            }
            ++x;
        }
        ++z;
    }
    minX -= centerX;
    maxX -= centerX;
    minZ -= centerZ;
    maxZ -= centerZ;
    TranslatePoints(occupiedCells, -centerX, -centerZ);
    TranslatePoints(newCells, -centerX, -centerZ);
    TranslatePoints(checkCells, -centerX, -centerZ);
    tileWidth = maxX - minX + 1;
    tiles.assign(static_cast<std::size_t>(tileWidth * (maxZ - minZ + 1)), 0);
    for (const cIntPoint& cell : occupiedCells) {
        const int column = cell.x - minX;
        const int row = (cell.z - minZ) * 0x10;
        const auto code = static_cast<std::uint8_t>(row + 1 + column);
        tiles[static_cast<std::size_t>(column + (cell.z - minZ) * tileWidth)] = code;
        cTileDef& def = tileMap[code];
        def.orientation = 0;
        def.tile = static_cast<std::uint32_t>(column * 0x100 + row);
        def.newCell = 1;
        for (const cIntCheckCell& check : checkCells) {
            if (check.x == cell.x && check.z == cell.z) {
                def.newCell = 0;
                break;
            }
        }
    }
}

// The grid is as wide as the longest line; '-' 1, '|' 2, '/' 3, '\' 4, 'F' 5, 'R' 6, '#' 7, 'X' 8; '<' / '^' set the centre.
void cIntRule::ParseConstraintsFromASCIIArt(const std::list<std::string>& lines) {
    int width = 0;
    constraintCenterX = -1;
    constraintCenterZ = -1;
    for (const std::string& line : lines) width = std::max(width, static_cast<int>(line.size()));
    constraints.assign(static_cast<std::size_t>(static_cast<int>(lines.size()) * width), kConstraintNone);
    constraintWidth = width;
    int row = 0;
    for (const std::string& line : lines) {
        int column = 0;
        for (const char c : line) {
            std::uint8_t* at = &constraints[static_cast<std::size_t>(column + row * width)];
            switch (c) {
                case '#': *at = kConstraintFlat; break;
                case '-': *at = kConstraintHorizontal; break;
                case '/': *at = kConstraintSlash; break;
                case '<': constraintCenterZ = row; break;
                case 'F': *at = kConstraintF; break;
                case 'R': *at = kConstraintR; break;
                case 'X': *at = kConstraintX; break;
                case '\\': *at = kConstraintBackslash; break;
                case '^': constraintCenterX = column; break;
                case '|': *at = kConstraintVertical; break;
                default: break;
            }
            ++column;
        }
        ++row;
    }
}

// ---------------------------------------------------------------------------------------------------- the transforms

void cIntRule::Translate(int dx, int dz) {
    TranslatePoints(newCells, dx, dz);
    TranslatePoints(occupiedCells, dx, dz);
    TranslatePoints(checkCells, dx, dz);
    for (cIntPiece& piece : pieces) {
        piece.x = static_cast<float>(dx) + piece.x;
        piece.z = static_cast<float>(dz) + piece.z;
    }
    handleX += dx;
    handleZ += dz;
    constraintCenterX -= dx;
    constraintCenterZ -= dz;
    minX += dx;
    minZ += dz;
    maxX += dx;
    maxZ += dz;
}

// One quarter turn maps (x, z) to (-z, x). The check types' edge words rotate left by a byte per turn, the models and tiles
// turn (backwards when flipped), both grids turn. The bounds of an odd turn keep the original's slip: maxX becomes
// -(old minX) where -(old minZ) would be right, which only IsWithinBounds reads.
void cIntRule::Rotate(int quarterTurns) {
    const int turns = quarterTurns & 3;
    if (turns == 0) return;
    RotatePoints(newCells, turns);
    RotatePoints(occupiedCells, turns);
    RotatePoints(checkCells, turns);
    const int bits = turns * 8;
    for (auto& entry : checkTypes) {
        CheckType& check = entry.second;
        for (int i = 0; i < 2; ++i) {
            if (check.types[i] < 0) continue;
            check.checks[i].edges = (check.checks[i].edges << bits) | (check.checks[i].edges >> (32 - bits));
            check.checks[i].mask = (check.checks[i].mask << bits) | (check.checks[i].mask >> (32 - bits));
        }
    }
    for (cIntPiece& piece : pieces) {
        const int delta = piece.flip != 0 ? -turns : turns;
        piece.rotation = static_cast<std::uint8_t>((delta + piece.rotation) & 3);
        RotatePoint(piece, turns);
    }
    for (auto& entry : tileMap) {
        const auto orientation = static_cast<std::int8_t>(entry.second.orientation);
        entry.second.orientation = orientation < 0 ? static_cast<std::uint8_t>(((orientation - turns) & 3) | 0x80)
                                                   : static_cast<std::uint8_t>((orientation + turns) & 3);
    }
    cIntPoint handle{handleX, handleZ};
    RotatePoint(handle, turns);
    handleX = handle.x;
    handleZ = handle.z;
    cIntPoint center{constraintCenterX, constraintCenterZ};
    RotateMap(constraints, constraintWidth, center, turns, true);
    constraintCenterX = center.x;
    constraintCenterZ = center.z;
    cIntPoint tileCenter{-minX, -minZ};   // turned with the grid and then dropped, as the original does
    RotateMap(tiles, tileWidth, tileCenter, turns, false);
    if (quarterTurns & 2) {
        const int x0 = minX, z0 = minZ;
        minX = -maxX;
        minZ = -maxZ;
        maxX = -x0;
        maxZ = -z0;
    }
    if (quarterTurns & 1) {
        const int x0 = minX;
        minZ = x0;
        minX = -maxZ;
        maxZ = maxX;
        maxX = -x0;
    }
    oneWayDir = static_cast<std::uint8_t>((oneWayDir + turns * 2) & 7);
}

// The mirror across x = z: the cells and models swap x and z, the edge words swap their side bytes (W with N, E with S) and a
// byte whose whole value is 1, 2 or 3 becomes 4 - value, the masks only swap bytes; models and tiles toggle their flip.
void cIntRule::Transpose() {
    TransposePoints(newCells);
    TransposePoints(occupiedCells);
    TransposePoints(checkCells);
    for (auto& entry : checkTypes) {
        CheckType& check = entry.second;
        for (int i = 0; i < 2; ++i) {
            if (check.types[i] < 0) continue;
            std::uint32_t e = check.checks[i].edges;
            std::uint32_t b0 = e & 0xff, b1 = e >> 8 & 0xff, b2 = e >> 16 & 0xff, b3 = e >> 24;
            if (b0 - 1 < 3) b0 = 4 - b0;
            if (b1 - 1 < 3) b1 = 4 - b1;
            if (b2 - 1 < 3) b2 = 4 - b2;
            if (b3 - 1 < 3) b3 = 4 - b3;
            check.checks[i].edges = b1 + b2 * 0x1000000u + b3 * 0x10000u + b0 * 0x100u;
            const std::uint32_t m = check.checks[i].mask;
            check.checks[i].mask = (m >> 8 & 0xff) + (m >> 16) * 0x1000000u + (m >> 24) * 0x10000u + (m & 0xff) * 0x100u;
        }
    }
    for (cIntPiece& piece : pieces) {
        std::swap(piece.x, piece.z);
        const bool flipped = piece.flip == 0;
        piece.flip = flipped ? 1 : 0;
        piece.rotation = static_cast<std::uint8_t>((flipped ? piece.rotation + 1 : piece.rotation - 1) & 3);
    }
    for (auto& entry : tileMap) {
        const auto orientation = static_cast<std::int8_t>(entry.second.orientation);
        entry.second.orientation = orientation < 0 ? static_cast<std::uint8_t>((orientation - 1) & 3)
                                                   : static_cast<std::uint8_t>((orientation + 0x81) & 0x83);
    }
    std::swap(handleX, handleZ);
    cIntPoint center{constraintCenterX, constraintCenterZ};
    TransposeMap(constraints, constraintWidth, center, true);
    constraintCenterX = center.x;
    constraintCenterZ = center.z;
    cIntPoint tileCenter{-minX, -minZ};
    TransposeMap(tiles, tileWidth, tileCenter, false);
    std::swap(minX, minZ);
    std::swap(maxX, maxZ);
    oneWayDir = static_cast<std::uint8_t>((6 - oneWayDir) & 7);
}

// ---------------------------------------------------------------------------------------------------- queries

bool cIntRule::IsWithinBounds(int x, int z, std::uint32_t width, std::uint32_t height) const {
    return ((x + minX) | (z + minZ)) >= 0 && static_cast<std::uint32_t>(x + maxX) < width && static_cast<std::uint32_t>(z + maxZ) < height;
}

// The first network type of the tagged cell at the anchor, else the highway.
int cIntRule::GetTypeAtOrigin() const {
    for (const cIntCheckCell& cell : checkCells) {
        if (cell.x != 0 || cell.z != 0) continue;
        const CheckType* check = FindCheckType(cell.tag);
        return check ? check->types[0] : -1;
    }
    return kNetworkHighway;
}

const cIntRule::cTileDef* cIntRule::GetTileInfo(int dx, int dz) const {
    const auto column = static_cast<std::uint32_t>(dx - minX);
    if (column >= static_cast<std::uint32_t>(tileWidth)) return nullptr;
    const std::uint32_t index = column + static_cast<std::uint32_t>(dz - minZ) * static_cast<std::uint32_t>(tileWidth);
    if (index >= tiles.size()) return nullptr;
    const auto found = tileMap.find(tiles[index]);
    return found == tileMap.end() ? nullptr : &found->second;
}

std::uint8_t cIntRule::GetConstraintAtCell(int dx, int dz) const {
    const auto index = static_cast<std::uint32_t>(constraintCenterX + dx + (constraintCenterZ + dz) * constraintWidth);
    return index < constraints.size() ? constraints[index] : kConstraintNone;
}

const cIntRule::CheckType* cIntRule::FindCheckType(std::uint32_t tag) const {
    const auto found = checkTypes.find(static_cast<std::uint8_t>(tag));
    return found == checkTypes.end() ? nullptr : &found->second;
}

// Every tagged cell that is not optional must have a cell info. An occupied one may not already be part of an intersection
// (a piece with flag 0x100000, or a cell marked +0x52) unless the rule replaces the intersection anchored at its replacement
// offset, and may not be fixed. Each network check needs its type in the cell and its edges under the mask; other types are
// only allowed with multiok; a checked one-way road must point the rule's way. With new cells, none may be water or hold a
// network piece, and the demolition of what stands on them must be allowed.
int cIntRule::Test(int x, int z, cSC4NetworkTool& tool) const {
    const std::uint8_t oneWay = kOneWayDirectionMasks[oneWayDir & 7];
    INetworkToolHost* host = GetNetworkToolHost();
    for (const cIntCheckCell& cell : checkCells) {
        const CheckType* check = FindCheckType(cell.tag);
        if (!check || check->optional) continue;
        cSC4NetworkCellInfo* info = tool.GetCellInfo(static_cast<std::uint32_t>(x + cell.x), static_cast<std::uint32_t>(z + cell.z));
        if (!info) return kFail;
        if (check->occupies) {
            const bool pieceIsIntersection = info->occupant && info->occupant->HasNetworkFlag(0x100000);
            if (pieceIsIntersection || info->unknown5a) {
                if (!hasReplacement) return kNoMatch;
                if (pieceIsIntersection) {
                    // vslot 0x11c; the original compares its outputs even when the piece has no origin property.
                    int originX = -1, originZ = -1;
                    info->occupant->GetComplexPieceOrigin(originX, originZ);
                    if (x + replacementX != originX || z + replacementZ != originZ) return kNoMatch;
                }
            }
            if (info->solvedOrImmovable && !info->unknown5a) return kNoMatch;
        }
        std::uint32_t types = 0, oneWayEdges = 0;
        for (int i = 0; i < 2; ++i) {
            const int type = check->types[i];
            if (type < 0) continue;
            if (type == kNetworkOneWayRoad) oneWayEdges = check->checks[i].edges;
            if ((info->networkFlags & kFlagForNetworkType[type]) == 0) return kNoMatch;
            types |= kFlagForNetworkType[type];
            if ((check->checks[i].mask & (info->tileNeighborhoods[type] ^ check->checks[i].edges)) != 0) return kFail;
        }
        if (!check->multiOk && (~types & info->networkFlags & 0x1fff) != 0) return kNoMatch;
        if ((types & 0x400) != 0 && !check->occupies && ((oneWay ^ info->unknown58) & SideMask(oneWayEdges)) != 0) return kFail;
    }
    if (newCells.empty()) return kMatch;
    std::vector<SC4PointU> cells;
    for (const cIntPoint& cell : newCells) {
        const int cx = x + cell.x, cz = z + cell.z;
        if (host && host->TerrainAllowsNetwork(static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cz))) return kNoMatch;   // the cell has water
        if (host) {
            if (cSC4NetworkOccupant* piece = host->NetworkOccupantAt(static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cz), kNetworkFlagMask)) {
                piece->Release();
                return kNoMatch;
            }
        }
        cells.push_back({static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cz)});
    }
    std::int64_t cost = 0;
    const bool allowed = !host || host->DemolishForNetwork(false, cells, cost);
    return allowed ? kMatch : kNoMatch;
}

// For every network check of every tagged cell (optional ones too): when the cell has no info, or its edges differ under the
// mask, or a checked one-way road points the other way, the check's edges are added to the cell as an additional draw. A
// one-way check on a cell the piece does not take is skipped when the existing one-way arm points against the rule.
bool cIntRule::DoAutoComplete(int x, int z, cSC4NetworkTool& tool) const {
    const std::uint8_t oneWay = kOneWayDirectionMasks[oneWayDir & 7];
    for (const cIntCheckCell& cell : checkCells) {
        const CheckType* check = FindCheckType(cell.tag);
        if (!check) continue;
        const int cx = x + cell.x, cz = z + cell.z;
        for (int i = 0; i < 2; ++i) {
            const int type = check->types[i];
            if (type < 0) continue;
            cSC4NetworkCellInfo* info = tool.GetCellInfo(static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cz));
            const bool mismatch = !info || (check->checks[i].mask & (info->tileNeighborhoods[type] ^ check->checks[i].edges)) != 0;
            bool wrongWay = false;
            std::uint32_t sides = 0;
            if (type == kNetworkOneWayRoad) {
                const std::uint8_t current = info ? info->unknown58 : 0;
                sides = SideMask(check->checks[i].edges);
                const std::uint32_t differs = (static_cast<std::uint32_t>(current) ^ oneWay) & sides;
                wrongWay = differs != 0;
                const auto pairs = static_cast<std::uint8_t>(current | ((current & 0xaa) >> 1) | ((current & 0x55) * 2));
                if (!check->occupies && (differs & pairs) != 0) continue;
            }
            if (!info || mismatch || wrongWay)
                tool.AddEdgesToTile(static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cz), check->checks[i].edges,
                                    static_cast<std::uint32_t>(type), static_cast<std::uint8_t>(sides & oneWay),
                                    static_cast<std::uint8_t>(sides));
        }
    }
    return true;
}

}  // namespace nSC4Networks
}  // namespace sc4

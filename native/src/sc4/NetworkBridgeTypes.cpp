// The bridge types of a network type and its support structures, as cSC4NetworkTypeInfo loads them: LoadBridgeTypes reads the
// network ini's "<Type>BridgeExemplars" section and each named bridge exemplar (ReadBridgeTypeInfo), LoadSupports every support
// structure exemplar of a group (ReadSupportStructureInfos). Recovered from the Mac i386 build with symbols.
#include "sc4/NetworkTool.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "gz/cIGZPersistResourceManager.h"
#include "rz/RZConvert.h"
#include "rz/RZStringUtils.h"
#include "rz/cRZIniFile.h"
#include "sc4/Exemplar.h"
#include "sc4/GeometryUtilities.h"
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
    ReadOr(p, 0x482cb457, b.suspensionValue, 0.0f);
    ReadOr<std::uint32_t>(p, 0x482cb45a, b.minPylons, 0);
    ReadOr<std::uint32_t>(p, 0x482cb45b, b.maxPylons, 100);
    ReadOr<std::uint32_t>(p, 0x482cb45c, b.suspensionA, 0);
    ReadOr<std::uint32_t>(p, 0x482cb45d, b.suspensionB, 0);
    ReadOr(p, 0x482cb471, b.unknownB8, 0.0f);
    ReadOr(p, 0x482cd460, b.unknownBC, 0.0f);
    ReadOr(p, 0x482cd461, b.unknownC0, 0.0f);
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
    if (H::GetPropertyValue(p, 0x482cb46b, b.cableCount) && H::GetPropertyValue(p, 0x482cb46c, b.cableSag) &&
        ReadVector3Pair(p, 0x482cb469, b.cableTowerA) && ReadVector3Pair(p, 0x482cb46a, b.cableTowerB) &&
        ReadVector3Pair(p, 0x482cb46d, b.cableDeck)) {
        for (int i = 0; i != 2; ++i) {
            b.cableDeckMirrored[i][0] = -b.cableDeck[i][0];
            b.cableDeckMirrored[i][1] = b.cableDeck[i][1];
            b.cableDeckMirrored[i][2] = b.cableDeck[i][2];
        }
        if (H::GetPropertyValue(p, 0x482cb46e, b.cableTexture)) b.hasCables = true;
    }
    return true;
}

void LoadNetworkBridgeTypes(::cRZIniFile& ini, const char* section, std::vector<cSC4NetworkBridgeTypeInfo>& out) {
    BridgeIniContext context{&out};
    ini.EnumEntries(cRZString(section), &ReadBridgeTypeEntry, &context);
    // ReadBridgeTypeSpecializations: the specialization script parser is not ported (see cSC4NetworkBridgeSpecialization).
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
        cSCPropertyHelp::GetPropertyValue(holder, 0xe974eaa2, info.models);
        cSCPropertyHelp::GetPropertyValue(holder, 0xe974eaa3, info.footprint);
        AppendQuarterTurns(info.footprint);
        out[id] = info;
        exemplar->Release();
    }
}

}  // namespace sc4

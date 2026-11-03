#include "sc4/BusinessFeature.h"

#include <algorithm>
#include <cctype>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/RZConvert.h"
#include "sc4/DBSegment.h"
#include "sc4/Lot.h"

namespace sc4 {

// ---- business unit -------------------------------------------------------------------------------------

cSC4BusinessUnit::~cSC4BusinessUnit() {
    for (cSC4Lot* lot : members_) lot->Release();
}

bool cSC4BusinessUnit::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4BusinessUnit) *out = static_cast<cISC4BusinessUnit*>(this);
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4BusinessUnit::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4BusinessUnit::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    stream.SetUint32(static_cast<std::uint32_t>(members_.size()));
    for (const cSC4Lot* lot : members_) WriteLotReference(stream, lot);
    return !stream.GetError();
}

// The original pushes a null member when a reference does not resolve (and then dereferences it); the port skips it.
bool cSC4BusinessUnit::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        cSC4Lot* lot = ReadLotReference(stream);
        if (!lot) continue;
        lot->AddRef();
        members_.push_back(lot);
    }
    return !stream.GetError();
}

void cSC4BusinessUnit::Add(cSC4Lot* lot) {
    if (std::find(members_.begin(), members_.end(), lot) == members_.end()) {
        lot->AddRef();
        members_.push_back(lot);
    }
    lot->SetUnit(this);
}

void cSC4BusinessUnit::Remove(cSC4Lot* lot) {
    AddRef();                             // the lot may hold the last reference to the unit
    for (auto it = members_.begin(); it != members_.end();) {
        if (*it == lot) {
            it = members_.erase(it);
            lot->Release();
        } else {
            ++it;
        }
    }
    lot->SetUnit(nullptr);
    lot->SetTravelDesignate(nullptr);
    Release();
}

// ---- feature manager -----------------------------------------------------------------------------------

namespace {
bool g_staticInitialised = false;
std::uint32_t g_installedPacks = 0;
std::set<std::uint32_t> g_features;
IFeatureHost* g_featureHost = nullptr;
}  // namespace

void SetFeatureHost(IFeatureHost* host) { g_featureHost = host; }

bool cSC4FeatureManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cISC4FeatureManager) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4FeatureManager::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4FeatureManager::Init() {
    if (!initialised_) {
        initialised_ = true;
        StaticInit();
    }
    return true;
}

bool cSC4FeatureManager::Shutdown() {
    initialised_ = false;
    return true;
}

bool cSC4FeatureManager::StaticInit() {
    if (g_staticInitialised) return true;
    g_staticInitialised = true;
    g_installedPacks = 0;
    g_features.clear();
    if (!g_featureHost) return true;
    for (unsigned index = 0; index < 0x20; ++index) {
        std::vector<std::pair<std::string, std::string>> entries;
        if (!g_featureHost->LoadFeatureEntries(index, entries)) continue;
        g_installedPacks |= 1u << index;
        for (const auto& entry : entries) EnableFeatures(entry.first, entry.second);
    }
    return true;
}

bool cSC4FeatureManager::StaticShutdown(void*) {
    if (g_staticInitialised) {
        g_staticInitialised = false;
        g_installedPacks = 0;
        g_features.clear();
    }
    return true;
}

void cSC4FeatureManager::EnableFeatures(const std::string& key, const std::string& value) {
    const std::uint32_t feature = RZ::ConvertToUint(key.c_str());
    std::string lowered = value;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lowered == "off") g_features.erase(feature);
    else g_features.insert(feature);
}

bool cSC4FeatureManager::IsFeatureEnabled(std::uint32_t feature) const { return g_features.count(feature) != 0; }

bool cSC4FeatureManager::IsExpansionPackInstalled(int pack) const {
    return ((g_installedPacks >> ((static_cast<std::uint32_t>(pack) - 1u) & 0x1f)) & 1u) != 0;
}

int cSC4FeatureManager::GetCurrentExpansionPack() const {
    for (int pack = 0x20; pack != 0; --pack)
        if (IsFeatureEnabled(static_cast<std::uint32_t>(pack))) return pack;
    return 0;
}

}  // namespace sc4

#include "sc4/NetworkBridgeOccupant.h"

namespace sc4 {

namespace {
IBridgeEffectsHost* g_bridgeEffectsHost = nullptr;
}  // namespace

IBridgeEffectsHost* BridgeEffectsHost() { return g_bridgeEffectsHost; }
void SetBridgeEffectsHost(IBridgeEffectsHost* host) { g_bridgeEffectsHost = host; }

// i386 0x00156bc2: the network flags gain 0xc00000.
cSC4NetworkBridgeOccupant::cSC4NetworkBridgeOccupant() { networkFlags_ |= 0x00c00000u; }

cSC4NetworkBridgeOccupant::~cSC4NetworkBridgeOccupant() {
    if (effects_ && g_bridgeEffectsHost) g_bridgeEffectsHost->ReleaseEffects(effects_);
    effects_ = nullptr;
}

bool cSC4NetworkBridgeOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4NetworkBridgeOccupant) {
        *out = this;
        AddRef();
        return true;
    }
    return cSC4NetworkOccupantWithPreBuiltModel::QueryInterface(iid, out);
}

bool cSC4NetworkBridgeOccupant::Init() {
    if (IsInitialized()) return true;
    return cSC4NetworkOccupantWithPreBuiltModel::Init();
}

bool cSC4NetworkBridgeOccupant::Shutdown() {
    if (!IsInitialized()) return true;
    if (effects_ && g_bridgeEffectsHost) g_bridgeEffectsHost->ReleaseEffects(effects_);
    effects_ = nullptr;
    return cSC4NetworkOccupantWithPreBuiltModel::Shutdown();
}

void cSC4NetworkBridgeOccupant::UpdateBaseModel(s3d::cIS3DModelInstance* model) {
    if (!g_bridgeEffectsHost) return;
    float position[3] = {0.0f, 0.0f, 0.0f};
    GetPosition(position);   // vslot 0x148 (i386)
    effects_ = g_bridgeEffectsHost->UpdateEffects(effects_, model, position);
}

}  // namespace sc4

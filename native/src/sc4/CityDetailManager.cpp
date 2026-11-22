#include "sc4/CityDetailManager.h"

#include "gz/cIGZIStream.h"
#include "platform/Unwired.h"
#include "gz/cIGZOStream.h"

namespace sc4 {

namespace {
ICityDetailHost* g_host = nullptr;
}

void SetCityDetailHost(ICityDetailHost* host) { g_host = host; }

bool cSC4CityDetailManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZIID_cISC4CityDetailManager && iid != 1 && iid != kGZIID_cSC4CityDetailManagerPrimary) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4CityDetailManager::Release() {
    if (--refs_ != 0) return refs_;
    delete this;
    return 0;
}

bool cSC4CityDetailManager::Write(cIGZOStream& stream) const {
    stream.SetUint32(0);   // record version
    stream.SetUint32(detailLevel_);
    return true;
}

bool cSC4CityDetailManager::Read(cIGZIStream& stream) {
    std::uint32_t version = 0;
    stream.GetUint32(version);
    if (version == 0) stream.GetUint32(detailLevel_);
    return version == 0;
}

bool cSC4CityDetailManager::Init() {
    const std::uint8_t preference = g_host ? g_host->DetailPreference() : 0;
    if (!g_host) SC4_UNWIRED("cSC4CityDetailManager::Init: no host: the city detail preference is not compared and Reduce/IncreaseCityDetail never run");
    if (detailLevel_ != 0xffffffff && g_host) {
        if (preference < detailLevel_) g_host->ReduceCityDetail();
        else if (detailLevel_ < preference) g_host->IncreaseCityDetail();
    }
    detailLevel_ = preference;
    return true;
}

}  // namespace sc4

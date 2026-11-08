// The flammability simulator's hotspot and average for the safety advisor (apart: its header's constants clash with the
// fire and building headers the other advisor files include).
#include <cstdint>

#include "app/NativeCity.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4FlammabilitySimulator.h"

namespace native {

bool AdvisorFlammability(std::uint8_t* hotspot, int* x, int* z, std::uint8_t* average) {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::FlammabilitySimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return false;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    auto* f = dynamic_cast<sc4::cSC4FlammabilitySimulator*>(serializable);
    serializable->Release();
    if (!f) return false;
    f->GetFlammabilityHotspot(hotspot, x, z);
    *average = f->GetAverageFlammability();
    return true;
}

}  // namespace native

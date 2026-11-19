#include <cstdio>

#include "gz/GZCOM.h"
#include "gz/DrawContextAsBuffer.h"
#include "gz/cGZMessageServer2.h"
#include "sc4/BusinessFeature.h"
#include "sc4/IndustrialSimulator.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    using namespace gzcom;
    struct Entry { void* (*create)(); std::uint32_t clsid; void (*destroy)(void*); } entries[] = {
        {&GZCOM_CreatecGZMessageServer2, kCLSID_cGZMessageServer2, [](void* p) { delete static_cast<cGZMessageServer2*>(p); }},
        {&GZCOM_CreatecGZDrawContextAsBuffer, kCLSID_cGZDrawContextAsBuffer, [](void* p) { delete static_cast<gz::cGZDrawContextAsBuffer*>(p); }},
        {&GZCOM_CreatecSC4BusinessUnit, kCLSID_cSC4BusinessUnit, [](void* p) { delete static_cast<sc4::cSC4BusinessUnit*>(p); }},
        {&GZCOM_CreatecSC4IndustrialSimulator, kCLSID_cSC4IndustrialSimulator, [](void* p) { delete static_cast<sc4::cSC4IndustrialSimulator*>(p); }}};
    for (const Entry& e : entries) {
        void* object = e.create();
        CHECK(object != nullptr);
        const ClassFactoryInfo* info = FindClassFactory(e.clsid);
        CHECK(info != nullptr && info->create == e.create);
        e.destroy(object);
    }
    std::printf(failures ? "FAILED\n" : "gz_gzcomcreators6 ok\n");
    return failures != 0;
}
